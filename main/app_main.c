#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/uart.h"
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

static const char *TAG = "RTU";

static rtu_queues_t q;
static rtu_runtime_t rt;

#define CMD_RX_TIMEOUT_MS 100

static void queue_command_ack_ex(uint16_t flag, uint8_t soc)
{
    xSemaphoreTake(q.ack_mutex, portMAX_DELAY);
    rt.pending_ack_flags |= flag;
    if (soc != 0) {
        rt.pending_soc_ack = soc;
    }
    /* Box (soc): mais janelas — estação precisa ouvir 0xBB no ar. */
    const uint8_t ttl = (soc == TELEM_SOC_ACK_BOX) ? 16u : 10u;
    rt.pending_ack_ttl = ttl;
    xSemaphoreGive(q.ack_mutex);
    xSemaphoreGive(q.tx_trigger); /* acorda lora_tx já — prioridade ACK */
    if (soc == TELEM_SOC_ACK_BOX) {
        ESP_LOGI(TAG, "ACK BOX queued (soc=0x%02X ttl=%u)", (unsigned)soc,
                 (unsigned)ttl);
    }
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

        if ((fsm_state() == FSM_RUNNING || fsm_state() == FSM_SELF_CHECK) &&
            !sd_log_busy()) {
            /* SD sempre em corrida; em box/download softAP trava o resto. */
            sd_pkt.timestamp_ms = now_ms;
            sd_pkt.telem = pkt;
            if (xQueueSend(q.sd, &sd_pkt, 0) != pdTRUE) {
                static uint32_t sd_drop;
                sd_drop++;
                if ((sd_drop % 50u) == 1u) {
                    ESP_LOGW(TAG, "sd queue full — drop #%lu (SPI lento?)",
                             (unsigned long)sd_drop);
                }
            }

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

#define LORA_TX_PERIOD_MS     1000  /* telem normal 1 Hz */
#define LORA_ACK_BURST_MS      200  /* ACK: re-TX rápido (ok “perder” telem no ar) */

static void lora_tx_task(void *arg)
{
    (void)arg;
    telemetry_packet_t tx_copy;
    ESP_LOGI(TAG, "lora_tx on core %d period=%dms ack_burst=%dms",
             xPortGetCoreID(), LORA_TX_PERIOD_MS, LORA_ACK_BURST_MS);

    while (1) {
        /* Cadência: 1 Hz normal; com ACK pendente, burst ~200 ms. */
        xSemaphoreTake(q.ack_mutex, portMAX_DELAY);
        const bool ack_wait = (rt.pending_ack_ttl > 0);
        xSemaphoreGive(q.ack_mutex);
        (void)xSemaphoreTake(q.tx_trigger,
                             pdMS_TO_TICKS(ack_wait ? LORA_ACK_BURST_MS : LORA_TX_PERIOD_MS));

        if (!fsm_is_running() && fsm_state() != FSM_SELF_CHECK) {
            continue;
        }
        uint16_t ack_flags = 0;
        uint8_t ack_soc = 0;
        xSemaphoreTake(q.ack_mutex, portMAX_DELAY);
        const bool ack_pending = (rt.pending_ack_ttl > 0);
        if (ack_pending) {
            ack_flags = rt.pending_ack_flags;
            ack_soc = rt.pending_soc_ack;
        }
        xSemaphoreGive(q.ack_mutex);

        /* SoftAP download pausa telem; ACK (ex.: box 0xBB) ainda sobe no ar. */
        if (sd_log_busy() && !ack_pending) {
            continue;
        }

        /* Quiet após RX. ACK: quiet mínimo — prioridade no ar (estação pode dropar telem). */
        const int quiet_need = ack_pending ? 20 : 150;
        const int quiet_loops = ack_pending ? 4 : 25;
        for (int i = 0; i < quiet_loops; i++) {
            TickType_t quiet = xTaskGetTickCount() - rt.e32_last_rx_tick;
            size_t pending_rx = 0;
            (void)uart_get_buffered_data_len(E32_UART_NUM, &pending_rx);
            if (pending_rx == 0 && quiet >= pdMS_TO_TICKS(quiet_need)) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }

        if (xQueuePeek(q.telemetry, &tx_copy, 0) != pdTRUE) {
            continue;
        }

        tx_copy.flags |= ack_flags;
        if (ack_soc != 0) {
            tx_copy.soc = ack_soc;
        } else if (can_pilot_box_latched()) {
            tx_copy.soc = TELEM_SOC_PILOT_BOX;
        }
        tx_copy.seq = rt.lora_seq++;
        tx_copy.ber = 0;
        tx_copy.per = 0;
        tx_copy.crc8 = telemetry_crc8(&tx_copy);

        if (e32_send_struct(&tx_copy, sizeof(tx_copy))) {
            rtu_health_note_lora(true);
            /* SoftAP/Diag vê o mesmo snapshot que foi ao ar (incl. soc ACK box). */
            telem_store_update(&tx_copy);
            static uint32_t tx_ok;
            tx_ok++;
            if (ack_pending || (tx_ok % 8u) == 1u) {
                ESP_LOGI(TAG, "lora_tx ok seq=%u n=%lu AUX=%d%s%s",
                         (unsigned)tx_copy.seq, (unsigned long)tx_ok,
                         (int)e32_aux_ready(),
                         ack_pending ? " ACK" : "",
                         (ack_soc == TELEM_SOC_ACK_BOX) ? " BOX" : "");
            }
            xSemaphoreTake(q.ack_mutex, portMAX_DELAY);
            if (rt.pending_ack_ttl > 0) {
                rt.pending_ack_ttl--;
                if (rt.pending_ack_ttl == 0) {
                    rt.pending_ack_flags = 0;
                    rt.pending_soc_ack = 0;
                }
            }
            xSemaphoreGive(q.ack_mutex);
        } else {
            rtu_health_note_lora(false);
            ESP_LOGW(TAG, "lora_tx FAIL seq=%u AUX=%d%s",
                     (unsigned)tx_copy.seq, (int)e32_aux_ready(),
                     ack_pending ? " ACK" : "");
            /* Falha: não consome TTL — re-tenta no próximo burst. */
        }
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
