#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"

#include "board_pins.h"
#include "can_ids.h"
#include "can_mgmt.h"
#include "ebyte_e32.h"
#include "sd_log.h"
#include "a7670_mqtt.h"
#include "telemetry_packets.h"
#include "rtu_cmd_protocol.h"
#include "data_types.h"
#include "system_fsm.h"
#include "telemetry_agg.h"
#include "cmd_parser.h"
#include "status_led.h"
#include "telem_store.h"
#include "wifi_ap.h"
#include "http_telem.h"
#include "rtu_diag.h"
#include "rtu_health.h"
#include "rtu_fw.h"
#include "lora_rate.h"

static const char *TAG = "RTU";

static rtu_queues_t q;
static rtu_runtime_t rt;

#define CMD_RX_TIMEOUT_MS 100

static void queue_command_ack_ex(uint16_t flag, uint8_t soc)
{
    if (q.ack_mutex == NULL || q.tx_trigger == NULL) {
        return;
    }
    if (xSemaphoreTake(q.ack_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    rt.pending_ack_flags |= flag;
    if (soc != 0) {
        rt.pending_soc_ack = soc;
    }
    rt.pending_ack_ttl = 3;
    assert(rt.pending_ack_ttl == 3);
    BaseType_t gave = xSemaphoreGive(q.ack_mutex);
    assert(gave == pdTRUE);
    if (gave != pdTRUE) {
        return;
    }
    /* Binário: pdFALSE quer dizer que a task de TX já está acordada. */
    BaseType_t kicked = xSemaphoreGive(q.tx_trigger);
    assert(kicked == pdTRUE || kicked == pdFALSE);
    (void)kicked;
}

static void queue_command_ack(uint16_t flag)
{
    queue_command_ack_ex(flag, 0);
}

static void can_svc_task(void *arg)
{
    (void)arg;
    car_state_t car;
    telemetry_packet_t pkt;
    SD_Storage_Packet sd_pkt;

    memset(&car, 0, sizeof(car));
    TickType_t wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(20);

    esp_task_wdt_add(NULL);
    ESP_LOGI(TAG, "can_svc on core %d", xPortGetCoreID());

    while (1) {
        esp_task_wdt_reset();

        (void)can_update_state(&car);
        bool led_from_can;
        if (can_consume_led_debug(&led_from_can)) {
            rt.led_debug = led_from_can;
        }

        can_bus_stats_t bus_now;
        can_get_bus_stats(&bus_now);
        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        rtu_diag_tick(&car, bus_now.state, now_ms);

        bool can_ok = can_is_running();
        bool e32_ok = e32_aux_ready();
        /* Limp só com LOC confirmed (maturação) — não buraco curto de CAN.
         * SoftAP/Dados NÃO depende do E32: se AUX cair, LoRa fica limpo mas
         * telem_store continua (antes FSM→ERROR e /api/json congelava). */
        fsm_tick(can_ok, e32_ok, sd_log_ready(),
                 rtu_diag_fcu_down_confirmed(), rtu_diag_rcu_down_confirmed());

        /* GPIO2=CAN, GPIO25=LoRa (bancada). limp FCU/RCU não mexe no LED. */
        status_led_tick(fsm_state(), can_ok, e32_ok);

        /* Sempre publicar snapshot CAN → SoftAP, mesmo em ERROR/limp/E32 down. */
        telemetry_fill(&pkt, &car, rt.led_debug);
        xQueueOverwrite(q.telemetry, &pkt);
        telem_store_update(&pkt);

        if (fsm_state() == FSM_RUNNING || fsm_state() == FSM_SELF_CHECK) {
            sd_pkt.timestamp_ms = now_ms;
            sd_pkt.telem = pkt;
            xQueueSend(q.sd, &sd_pkt, 0);

            /* Rebroadcast LED só se já houve RX (evita BUS_OFF sozinho no barramento). */
            if (bus_now.rx_frames > 0) {
                static uint32_t last_led_cmd_ms;
                static bool last_led_sent = true;
                bool changed = (last_led_sent != rt.led_debug);
                if (changed || last_led_cmd_ms == 0 || (now_ms - last_led_cmd_ms) >= 5000) {
                    last_led_cmd_ms = now_ms;
                    last_led_sent = rt.led_debug;
                    twai_message_t led_msg;
                    memset(&led_msg, 0, sizeof(led_msg));
                    led_msg.identifier = DEBUG_LED_CONTROL;
                    led_msg.data_length_code = 1;
                    led_msg.data[0] = rt.led_debug ? 1 : 0;
                    led_msg.ss = 1;
                    (void)can_enqueue_tx(&led_msg);
                }
            }
        }

        vTaskDelayUntil(&wake, period);
    }
}

/* Um ciclo = espera do período + silêncio + duas esperas de AUX + UART. */
#define LORA_TX_QUIET_SPINS    25
#define LORA_TX_QUIET_SLICE_MS 10
#define LORA_TX_QUIET_MS       150
#define LORA_TX_CYCLE_SLACK_MS 1000
#define LORA_TX_CYCLE_MAX_MS \
    (LORA_RATE_MAX_MS \
     + (LORA_TX_QUIET_SPINS * LORA_TX_QUIET_SLICE_MS) \
     + (2u * E32_AUX_TIMEOUT_MS) \
     + E32_UART_TX_TIMEOUT_MS \
     + LORA_TX_CYCLE_SLACK_MS)

static void lora_tx_wait_period(uint32_t period_ms)
{
    assert(q.tx_trigger != NULL);
    assert(period_ms >= LORA_RATE_MIN_MS && period_ms <= LORA_RATE_MAX_MS);
    if (q.tx_trigger == NULL) {
        return;
    }
    BaseType_t kicked = xSemaphoreTake(q.tx_trigger, pdMS_TO_TICKS(period_ms));
    assert(kicked == pdTRUE || kicked == pdFALSE);
    (void)kicked;
}

static void lora_tx_wait_quiet(void)
{
    assert(LORA_TX_QUIET_SPINS > 0);
    assert(LORA_TX_QUIET_SPINS <= 25);
    for (int i = 0; i < LORA_TX_QUIET_SPINS; i++) {
        TickType_t quiet = xTaskGetTickCount() - rt.e32_last_rx_tick;
        size_t pending_rx = 0;
        esp_err_t err = uart_get_buffered_data_len(E32_UART_NUM, &pending_rx);
        if (err != ESP_OK) {
            return;
        }
        if (pending_rx == 0 && quiet >= pdMS_TO_TICKS(LORA_TX_QUIET_MS)) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(LORA_TX_QUIET_SLICE_MS));
    }
}

static bool lora_tx_read_ack(uint16_t *flags, uint8_t *soc)
{
    assert(flags != NULL);
    assert(soc != NULL);
    if (flags == NULL || soc == NULL) {
        return false;
    }
    *flags = 0;
    *soc = 0;
    if (q.ack_mutex == NULL || xSemaphoreTake(q.ack_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    if (rt.pending_ack_ttl > 0) {
        *flags = rt.pending_ack_flags;
        *soc = rt.pending_soc_ack;
    }
    BaseType_t gave = xSemaphoreGive(q.ack_mutex);
    assert(gave == pdTRUE);
    return gave == pdTRUE;
}

static void lora_tx_ack_sent(void)
{
    if (q.ack_mutex == NULL || xSemaphoreTake(q.ack_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (rt.pending_ack_ttl > 0) {
        rt.pending_ack_ttl--;
        if (rt.pending_ack_ttl == 0) {
            rt.pending_ack_flags = 0;
            rt.pending_soc_ack = 0;
        }
    }
    assert(rt.pending_ack_ttl <= 3);
    BaseType_t gave = xSemaphoreGive(q.ack_mutex);
    assert(gave == pdTRUE);
}

static bool lora_tx_emit(telemetry_packet_t *pkt)
{
    assert(pkt != NULL);
    assert(sizeof(*pkt) == 52);
    if (pkt == NULL || sizeof(*pkt) != 52) {
        return false;
    }
    uint16_t ack_flags = 0;
    uint8_t ack_soc = 0;
    if (!lora_tx_read_ack(&ack_flags, &ack_soc)) {
        return false;
    }
    pkt->flags |= ack_flags;
    if (ack_soc != 0) {
        pkt->soc = ack_soc;
    } else if (can_pilot_box_latched()) {
        pkt->soc = TELEM_SOC_PILOT_BOX;
    }
    pkt->seq = rt.lora_seq++;
    pkt->ber = 0;
    pkt->per = 0;
    pkt->crc8 = telemetry_crc8(pkt);
    if (!e32_send_struct(pkt, sizeof(*pkt))) {
        rtu_health_note_lora(false);
        ESP_LOGW(TAG, "lora_tx FAIL seq=%u AUX=%d",
                 (unsigned)pkt->seq, (int)e32_aux_ready());
        return false;
    }
    rtu_health_note_lora(true);
    lora_rate_note_tx();
    lora_tx_ack_sent();
    return true;
}

static void lora_tx_task(void *arg)
{
    (void)arg;
    telemetry_packet_t tx_copy;
    uint32_t logged_ms = 0;
    uint32_t tx_ok = 0;
    ESP_LOGI(TAG, "lora_tx on core %d ciclo<=%ums", xPortGetCoreID(), LORA_TX_CYCLE_MAX_MS);

    /* Único laço sem fim: cada volta cabe em LORA_TX_CYCLE_MAX_MS. */
    while (1) {
        TickType_t cycle_start = xTaskGetTickCount();
        uint32_t period_ms = lora_rate_ms();
        if (period_ms != logged_ms) {
            ESP_LOGI(TAG, "lora_tx period=%lums", (unsigned long)period_ms);
            logged_ms = period_ms;
        }
        lora_tx_wait_period(period_ms);

        bool running = fsm_is_running() || fsm_state() == FSM_SELF_CHECK;
        if (running) {
            lora_tx_wait_quiet();
            if (xQueuePeek(q.telemetry, &tx_copy, 0) == pdTRUE && lora_tx_emit(&tx_copy)) {
                tx_ok++;
                if ((tx_ok % 8u) == 1u) {
                    ESP_LOGI(TAG, "lora_tx ok seq=%u n=%lu AUX=%d",
                             (unsigned)tx_copy.seq, (unsigned long)tx_ok,
                             (int)e32_aux_ready());
                }
            }
        }

        TickType_t elapsed = xTaskGetTickCount() - cycle_start;
        assert(elapsed <= pdMS_TO_TICKS(LORA_TX_CYCLE_MAX_MS));
    }
}

static void cmd_dispatch(const cmd_result_t *cmd)
{
    if (cmd == NULL || !cmd->handled) {
        return;
    }

    if (cmd->is_coming_ack) {
        can_pilot_box_clear();
        (void)can_post_u8(PILOT_BOX_CONTROL, 0);
        return;
    }

    if (cmd->is_led) {
        rt.led_debug = cmd->led_on;
        if (can_post_u8(cmd->msg.identifier, cmd->msg.data[0])) {
            queue_command_ack(cmd->ack_flag);
        }
        return;
    }

    if (cmd->msg.data_length_code > 1) {
        if (can_post(cmd->msg.identifier, cmd->msg.data, cmd->msg.data_length_code)) {
            queue_command_ack(cmd->ack_flag);
        }
        return;
    }

    if (can_post_u8(cmd->msg.identifier, cmd->msg.data[0])) {
        if (cmd->is_box) {
            queue_command_ack_ex(0, cmd->ack_soc);
        } else {
            queue_command_ack(cmd->ack_flag);
        }
    }
}

static bool http_ascii_cmd(const char *line)
{
    uint8_t op = 0;
    if (strcmp(line, "kill") == 0) {
        op = RTU_CMD_KILL;
    } else if (strcmp(line, "start") == 0) {
        op = RTU_CMD_START;
    } else if (strcmp(line, "light_on") == 0) {
        op = RTU_CMD_LIGHT_ON;
    } else if (strcmp(line, "light_off") == 0) {
        op = RTU_CMD_LIGHT_OFF;
    } else if (strcmp(line, "led_on") == 0) {
        op = RTU_CMD_LED_ON;
    } else if (strcmp(line, "led_off") == 0) {
        op = RTU_CMD_LED_OFF;
    } else if (strcmp(line, "box") == 0) {
        op = RTU_CMD_BOX;
    } else if (strcmp(line, "box_off") == 0) {
        op = RTU_CMD_BOX_OFF;
    } else if (strcmp(line, "coming_ack") == 0) {
        op = RTU_CMD_COMING_ACK;
    } else {
        return false;
    }

    cmd_result_t cmd;
    if (!cmd_parse_opcode(op, NULL, 0, &cmd)) {
        return false;
    }
    cmd_dispatch(&cmd);
    ESP_LOGI(TAG, "cmd←WiFi '%s'", line);
    return true;
}

static void lora_rx_task(void *arg)
{
    (void)arg;
    /* body = opcode|plen|payload|crc (sem sync) */
    uint8_t body[2u + RTU_CMD_PAYLOAD_MAX + 1u];
    size_t body_len = 0;
    uint8_t expect_plen = 0;
    enum {
        ST_SYNC0,
        ST_SYNC1,
        ST_OPCODE,
        ST_PLEN,
        ST_PAYLOAD_CRC,
    } st = ST_SYNC0;
    TickType_t frame_start = 0;

    ESP_LOGI(TAG, "lora_rx on core %d (cmd binário A5 5A)", xPortGetCoreID());

    while (1) {
        uint8_t byte;
        int len = uart_read_bytes(E32_UART_NUM, &byte, 1, pdMS_TO_TICKS(20));
        if (len <= 0) {
            if (st != ST_SYNC0 &&
                (xTaskGetTickCount() - frame_start) > pdMS_TO_TICKS(CMD_RX_TIMEOUT_MS)) {
                st = ST_SYNC0;
                body_len = 0;
            }
            continue;
        }
        rt.e32_last_rx_tick = xTaskGetTickCount();

        switch (st) {
        case ST_SYNC0:
            if (byte == RTU_CMD_SYNC0) {
                st = ST_SYNC1;
                frame_start = xTaskGetTickCount();
            }
            break;
        case ST_SYNC1:
            if (byte == RTU_CMD_SYNC1) {
                st = ST_OPCODE;
                body_len = 0;
            } else if (byte == RTU_CMD_SYNC0) {
                frame_start = xTaskGetTickCount();
            } else {
                st = ST_SYNC0;
            }
            break;
        case ST_OPCODE:
            body[0] = byte;
            body_len = 1;
            st = ST_PLEN;
            break;
        case ST_PLEN:
            expect_plen = byte;
            if (expect_plen > RTU_CMD_PAYLOAD_MAX) {
                ESP_LOGW(TAG, "cmd plen inválido %u", (unsigned)expect_plen);
                st = (byte == RTU_CMD_SYNC0) ? ST_SYNC1 : ST_SYNC0;
                body_len = 0;
                break;
            }
            body[1] = byte;
            body_len = 2;
            st = ST_PAYLOAD_CRC;
            break;
        case ST_PAYLOAD_CRC: {
            body[body_len++] = byte;
            size_t need = 2u + (size_t)expect_plen + 1u;
            if (body_len < need) {
                break;
            }

            uint8_t opcode = 0;
            uint8_t plen = 0;
            const uint8_t *payload = NULL;
            if (rtu_cmd_check_body(body, body_len, &opcode, &payload, &plen)) {
                cmd_result_t cmd;
                if (cmd_parse_opcode(opcode, payload, plen, &cmd)) {
                    cmd_dispatch(&cmd);
                }
            } else {
                ESP_LOGW(TAG, "cmd frame CRC/formato inválido");
            }
            st = ST_SYNC0;
            body_len = 0;
            break;
        }
        }
    }
}

static void sd_log_task(void *arg)
{
    (void)arg;
    SD_Storage_Packet sd_pkt;
    ESP_LOGI(TAG, "sd_log on core %d", xPortGetCoreID());
    sd_log_init();

    while (1) {
        if (xQueueReceive(q.sd, &sd_pkt, portMAX_DELAY) == pdTRUE) {
            sd_log_data(&sd_pkt);
        }
    }
}

static void gprs_svc_task(void *arg)
{
    (void)arg;
    telemetry_packet_t pkt;
    ESP_LOGI(TAG, "gprs_svc stub on core %d", xPortGetCoreID());
    a7670_init();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (!fsm_is_running() || !a7670_ready()) {
            continue;
        }
        if (xQueuePeek(q.telemetry, &pkt, 0) == pdTRUE) {
            (void)a7670_publish_telemetry(&pkt);
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Rear Transmission Unit — pinout connections.md, sem GPS local");

    memset(&rt, 0, sizeof(rt));
    rt.led_debug = true;

    telem_store_init();
    rtu_diag_init();

    if (wifi_ap_start() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi SoftAP falhou");
    } else {
        rtu_health_init(); /* NVS boot/reset após nvs_flash do SoftAP */
        lora_rate_init();
        if (http_telem_start(http_ascii_cmd) != ESP_OK) {
            ESP_LOGE(TAG, "HTTP UI falhou");
        }
    }

    status_led_init();
    fsm_init();
    can_init();
    e32_init();

    q.telemetry = xQueueCreate(1, sizeof(telemetry_packet_t));
    q.sd = xQueueCreate(SD_QUEUE_SIZE, sizeof(SD_Storage_Packet));
    q.tx_trigger = xSemaphoreCreateBinary();
    q.ack_mutex = xSemaphoreCreateMutex();
    lora_rate_bind_kick(q.tx_trigger);

    if (q.telemetry == NULL || q.sd == NULL || q.tx_trigger == NULL || q.ack_mutex == NULL) {
        ESP_LOGE(TAG, "queue/mutex alloc failed — restart");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }

    xTaskCreatePinnedToCore(can_svc_task, "can_svc", 4096, NULL, 6, NULL, 0);
    /* RX LoRa > TX: comandos da estação não competem com telemetria 1 Hz. */
    xTaskCreatePinnedToCore(lora_tx_task, "lora_tx", 4096, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(lora_rx_task, "lora_rx", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(sd_log_task, "sd_log", 8192, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(gprs_svc_task, "gprs_svc", 4096, NULL, 2, NULL, 1);

    ESP_LOGI(TAG, "tasks · SoftAP %s · fw %s schema %d · http://192.168.4.1/",
             WIFI_AP_SSID, RTU_FW_VER, RTU_FW_SCHEMA);
}
