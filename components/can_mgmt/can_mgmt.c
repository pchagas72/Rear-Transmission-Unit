#include "can_mgmt.h"
#include "board_pins.h"
#include "can_ids.h"
/* IDF 6.1: driver/twai.h ainda funciona, mas emite #warning (vira -Werror=cpp). */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcpp"
#include "driver/twai.h"
#pragma GCC diagnostic pop
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>

#define TAG "CAN_MGMT"

/* Bancada: 1 = NO_ACK + probe + flip TX/RX (só diagnóstico). 0 = operação normal. */
#define CAN_XCVR_BENCH_TEST      0
#define CAN_PROBE_ID              0x7EE

#define CAN_TXQ_LEN               8
#define CAN_RX_DRAIN_MAX          64
#define CAN_TEC_TX_GUARD          96
#define CAN_RECOVERY_TIMEOUT_MS   1000
#define CAN_REINSTALL_COOLDOWN_MS 2000

static QueueHandle_t can_txq;
static bool driver_installed;
static bool recovery_issued;
static uint32_t recovering_since_ms;
static uint32_t last_reinstall_ms;
static twai_state_t last_logged_state = TWAI_STATE_STOPPED;
static volatile bool led_cmd_pending;
static volatile bool led_cmd_value;
static volatile bool ctrl_pending;
static uint32_t ctrl_id;
static uint8_t ctrl_dlc;
static uint8_t ctrl_data[8];
static volatile bool g_pilot_box;
static uint32_t g_rx_frames;
static uint32_t g_last_rx_id;
#if CAN_XCVR_BENCH_TEST
static gpio_num_t g_can_tx = PIN_CAN_TX;
static gpio_num_t g_can_rx = PIN_CAN_RX;
static int g_xcvr_phase; /* 0=STD, 1=FLIP, 2=done→STD */
#endif

static uint8_t ecu_state_to_wire(int8_t state)
{
    return (state < 0) ? 3 : (uint8_t)state;
}

static uint16_t le_u16(const uint8_t *d)
{
    return (uint16_t)(d[0] | (d[1] << 8));
}

static int16_t le_i16(const uint8_t *d)
{
    return (int16_t)(d[0] | (d[1] << 8));
}

static uint32_t now_ms(void)
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static void log_state_change(const twai_status_info_t *status)
{
    if (status->state == last_logged_state) {
        return;
    }
    ESP_LOGW(TAG, "TWAI %d -> %d  TEC=%lu REC=%lu",
             (int)last_logged_state, (int)status->state,
             (unsigned long)status->tx_error_counter,
             (unsigned long)status->rx_error_counter);
    last_logged_state = status->state;
}

static bool can_is_control_id(uint32_t id)
{
    return id >= 0x700u && id <= 0x704u;
}

static bool can_try_transmit(const twai_message_t *msg)
{
    twai_status_info_t status;
    if (twai_get_status_info(&status) != ESP_OK) {
        return false;
    }
    if (status.state != TWAI_STATE_RUNNING) {
        return false;
    }
    if (!can_is_control_id(msg->identifier) &&
        status.tx_error_counter >= CAN_TEC_TX_GUARD) {
        return false;
    }
    TickType_t wait = can_is_control_id(msg->identifier) ? pdMS_TO_TICKS(50)
                                                         : pdMS_TO_TICKS(5);
    esp_err_t err = twai_transmit(msg, wait);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TX drop id=0x%lX (%s)",
                 (unsigned long)msg->identifier, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool can_enqueue_tx(const twai_message_t *msg)
{
    if (can_txq == NULL || msg == NULL) {
        return false;
    }
    if (xQueueSend(can_txq, msg, 0) != pdTRUE) {
        twai_message_t dumped;
        xQueueReceive(can_txq, &dumped, 0);
        return xQueueSend(can_txq, msg, 0) == pdTRUE;
    }
    return true;
}

static void can_drain_txq(void)
{
    if (can_txq == NULL) {
        return;
    }
    twai_message_t msg;
    while (xQueueReceive(can_txq, &msg, 0) == pdTRUE) {
        if (!can_try_transmit(&msg)) {
            if (can_is_control_id(msg.identifier)) {
                continue;
            }
            xQueueSendToFront(can_txq, &msg, 0);
            break;
        }
    }
}

bool can_post(uint32_t id, const uint8_t *data, uint8_t dlc)
{
    if (data == NULL || dlc < 1 || dlc > 8) {
        return false;
    }
    ctrl_id = id;
    ctrl_dlc = dlc;
    memcpy(ctrl_data, data, dlc);
    ctrl_pending = true;
    twai_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.identifier = id;
    msg.data_length_code = dlc;
    memcpy(msg.data, data, dlc);
    msg.ss = 1;
    (void)can_enqueue_tx(&msg);
    return true;
}

bool can_post_u8(uint32_t id, uint8_t value)
{
    return can_post(id, &value, 1);
}

bool can_is_running(void)
{
    twai_status_info_t status;
    if (twai_get_status_info(&status) != ESP_OK) {
        return false;
    }
    return status.state == TWAI_STATE_RUNNING;
}

bool can_consume_led_debug(bool *on)
{
    if (!led_cmd_pending || on == NULL) {
        return false;
    }
    *on = led_cmd_value;
    led_cmd_pending = false;
    return true;
}

void can_pilot_box_clear(void)
{
    g_pilot_box = false;
}

bool can_pilot_box_latched(void)
{
    return g_pilot_box;
}

static bool can_driver_install_and_start(void)
{
#if CAN_XCVR_BENCH_TEST
    twai_mode_t mode = TWAI_MODE_NO_ACK; /* self-test: TX sem precisar de ACK no bus */
    gpio_num_t tx = g_can_tx;
    gpio_num_t rx = g_can_rx;
#else
    twai_mode_t mode = TWAI_MODE_NORMAL;
    gpio_num_t tx = PIN_CAN_TX;
    gpio_num_t rx = PIN_CAN_RX;
#endif
    twai_general_config_t g_config =
        TWAI_GENERAL_CONFIG_DEFAULT(tx, rx, mode);
    g_config.rx_queue_len = 64;
    g_config.tx_queue_len = 16;
    g_config.alerts_enabled =
        TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED |
        TWAI_ALERT_ERR_PASS | TWAI_ALERT_ABOVE_ERR_WARN |
        TWAI_ALERT_RX_QUEUE_FULL | TWAI_ALERT_TX_FAILED |
        TWAI_ALERT_BUS_ERROR | TWAI_ALERT_TX_SUCCESS;

    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t err = twai_driver_install(&g_config, &t_config, &f_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "twai_driver_install failed: %s", esp_err_to_name(err));
        driver_installed = false;
        return false;
    }
    driver_installed = true;

    err = twai_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "twai_start failed: %s", esp_err_to_name(err));
        return false;
    }
    recovery_issued = false;
    recovering_since_ms = 0;
    last_logged_state = TWAI_STATE_RUNNING;
    ESP_LOGI(TAG, "TWAI running TX=%d RX=%d @500k mode=%s",
             (int)tx, (int)rx,
             mode == TWAI_MODE_NO_ACK ? "NO_ACK(bench)" : "NORMAL");
    return true;
}

#if CAN_XCVR_BENCH_TEST
static void can_xcvr_rebind(gpio_num_t tx, gpio_num_t rx)
{
    g_can_tx = tx;
    g_can_rx = rx;
    last_reinstall_ms = 0;
    if (driver_installed) {
        twai_stop();
        twai_driver_uninstall();
        driver_installed = false;
    }
    vTaskDelay(pdMS_TO_TICKS(30));
    (void)can_driver_install_and_start();
}
#endif

static void can_hard_reinstall(void)
{
    uint32_t t = now_ms();
    if ((t - last_reinstall_ms) < CAN_REINSTALL_COOLDOWN_MS && last_reinstall_ms != 0) {
        return;
    }
    last_reinstall_ms = t;
    ESP_LOGE(TAG, "TWAI hard reinstall");

    if (driver_installed) {
        twai_stop();
        twai_driver_uninstall();
        driver_installed = false;
    }
    recovery_issued = false;
    recovering_since_ms = 0;
    vTaskDelay(pdMS_TO_TICKS(20));
    can_driver_install_and_start();
}

void can_init(void)
{
    if (can_txq == NULL) {
        can_txq = xQueueCreate(CAN_TXQ_LEN, sizeof(twai_message_t));
    }

    for (int attempt = 0; attempt < 3; attempt++) {
        if (can_driver_install_and_start()) {
            return;
        }
        if (driver_installed) {
            twai_stop();
            twai_driver_uninstall();
            driver_installed = false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGE(TAG, "TWAI failed to start after retries");
}

void can_recover_if_needed(void)
{
    twai_status_info_t status;
    uint32_t alerts = 0;

    if (!driver_installed) {
        can_hard_reinstall();
        return;
    }

    (void)twai_read_alerts(&alerts, 0);
    if (alerts & TWAI_ALERT_BUS_OFF) {
        ESP_LOGE(TAG, "Alert: BUS_OFF");
    }
    if (alerts & TWAI_ALERT_BUS_RECOVERED) {
        ESP_LOGW(TAG, "Alert: BUS_RECOVERED");
    }
    if (alerts & TWAI_ALERT_RX_QUEUE_FULL) {
        ESP_LOGW(TAG, "Alert: RX queue full");
    }

    if (twai_get_status_info(&status) != ESP_OK) {
        can_hard_reinstall();
        return;
    }
    log_state_change(&status);

    uint32_t t = now_ms();

    switch (status.state) {
    case TWAI_STATE_RUNNING:
        recovery_issued = false;
        recovering_since_ms = 0;
        break;
    case TWAI_STATE_BUS_OFF:
        if (!recovery_issued) {
            if (twai_initiate_recovery() == ESP_OK) {
                recovery_issued = true;
                recovering_since_ms = t;
            }
        } else if ((t - recovering_since_ms) > CAN_RECOVERY_TIMEOUT_MS) {
            can_hard_reinstall();
        }
        break;
    case TWAI_STATE_RECOVERING:
        if (recovering_since_ms == 0) {
            recovering_since_ms = t;
        }
        if ((t - recovering_since_ms) > CAN_RECOVERY_TIMEOUT_MS) {
            can_hard_reinstall();
        }
        break;
    case TWAI_STATE_STOPPED:
        if (twai_start() != ESP_OK) {
            can_hard_reinstall();
        } else {
            recovery_issued = false;
            recovering_since_ms = 0;
        }
        break;
    default:
        break;
    }
}

void can_refresh_link_status(car_state_t *state, uint32_t now)
{
    state->fcu_link_down =
        (state->last_fcu_ms == 0) || ((now - state->last_fcu_ms) > LINK_DOWN_TIMEOUT_MS);
    state->rcu_link_down =
        (state->last_rcu_ms == 0) || ((now - state->last_rcu_ms) > LINK_DOWN_TIMEOUT_MS);
    state->fcu_imu_ok =
        (state->last_imu_ms != 0) && ((now - state->last_imu_ms) < SENSOR_ALIVE_MS);
    state->rcu_mlx_ok =
        (state->last_cvt_ms != 0) && ((now - state->last_cvt_ms) < SENSOR_ALIVE_MS);
    state->gps_from_can =
        (state->last_gps_ms != 0) && ((now - state->last_gps_ms) < 5000);
    /* Sem frame fresco: SNA (0) — evita valor fantasma no SoftAP/LoRa. */
    if ((state->last_speed_ms == 0) || ((now - state->last_speed_ms) > SENSOR_ALIVE_MS)) {
        state->speed = 0;
    }
    if (!state->rcu_mlx_ok) {
        /* -1 = SNA (MLX desconectado / timeout) — SoftAP/pit mostram "SNA". */
        state->cvt_temp = -1;
    } else if (state->cvt_temp < 0) {
        state->cvt_temp = -1;
    }
    if ((state->last_rpm_ms == 0) || ((now - state->last_rpm_ms) > SENSOR_ALIVE_MS)) {
        state->rpm = 0;
    }
    if ((state->last_eng_ms == 0) || ((now - state->last_eng_ms) > SENSOR_ALIVE_MS)) {
        /* -1 = SNA (NTC open / timeout) — SoftAP mostra "SNA", não 0 °C falso. */
        state->eng_temp = -1;
    } else if (state->eng_temp < 0 || state->eng_temp > 18000) {
        state->eng_temp = -1;
    }
}

bool can_update_state(car_state_t *state)
{
    twai_message_t msg;
    bool updated = false;
    int msgs_processed = 0;
    uint32_t t = now_ms();
#if CAN_XCVR_BENCH_TEST
    static uint32_t last_probe_ms;
    static uint32_t probe_tx_ok;
    static uint32_t probe_tx_fail;
    static uint32_t probe_rx;
    static uint32_t phase_probes;
    static bool phase_logged;
#endif

    can_recover_if_needed();

#if CAN_XCVR_BENCH_TEST
    if (!phase_logged && g_xcvr_phase == 0) {
        phase_logged = true;
        ESP_LOGW(TAG, "XCVR PHASE A STD TX=%d RX=%d", (int)g_can_tx, (int)g_can_rx);
    }

    if (g_xcvr_phase == 0 && phase_probes >= 8) {
        ESP_LOGW(TAG, "XCVR PHASE B FLIP TX/RX %d/%d → %d/%d",
                 (int)PIN_CAN_TX, (int)PIN_CAN_RX, (int)PIN_CAN_RX, (int)PIN_CAN_TX);
        probe_tx_ok = probe_tx_fail = probe_rx = phase_probes = 0;
        g_xcvr_phase = 1;
        phase_logged = false;
        can_xcvr_rebind(PIN_CAN_RX, PIN_CAN_TX);
    } else if (g_xcvr_phase == 1 && phase_probes >= 8) {
        ESP_LOGW(TAG, "XCVR PHASE C restore STD TX=%d RX=%d (FLIP pior)",
                 (int)PIN_CAN_TX, (int)PIN_CAN_RX);
        probe_tx_ok = probe_tx_fail = probe_rx = phase_probes = 0;
        g_xcvr_phase = 2;
        can_xcvr_rebind(PIN_CAN_TX, PIN_CAN_RX);
    }

    if (last_probe_ms == 0 || (t - last_probe_ms) >= 1000) {
        last_probe_ms = t;
        twai_message_t probe = {0};
        probe.identifier = CAN_PROBE_ID;
        probe.data_length_code = 2;
        probe.data[0] = (uint8_t)(probe_tx_ok & 0xFF);
        probe.data[1] = (uint8_t)(probe_tx_fail & 0xFF);
        esp_err_t terr = twai_transmit(&probe, pdMS_TO_TICKS(50));
        if (terr == ESP_OK) {
            probe_tx_ok++;
        } else {
            probe_tx_fail++;
        }
        if (g_xcvr_phase < 2) {
            phase_probes++;
        }

        uint32_t alerts = 0;
        (void)twai_read_alerts(&alerts, 0);
        twai_status_info_t st = {0};
        (void)twai_get_status_info(&st);
        const char *ph = g_xcvr_phase == 0 ? "STD " : (g_xcvr_phase == 1 ? "FLIP" : "KEEP");
        ESP_LOGI(TAG,
                 "XCVR %s TX=%d RX=%d probe=%s ok=%lu fail=%lu rx=%lu state=%d "
                 "TEC=%lu REC=%lu alerts=0x%lx",
                 ph, (int)g_can_tx, (int)g_can_rx,
                 terr == ESP_OK ? "OK" : esp_err_to_name(terr),
                 (unsigned long)probe_tx_ok, (unsigned long)probe_tx_fail,
                 (unsigned long)probe_rx, (int)st.state,
                 (unsigned long)st.tx_error_counter,
                 (unsigned long)st.rx_error_counter,
                 (unsigned long)alerts);
    }
#endif

    while (twai_receive(&msg, 0) == ESP_OK && msgs_processed < CAN_RX_DRAIN_MAX) {
        updated = true;
        msgs_processed++;
        g_rx_frames++;
        g_last_rx_id = msg.identifier;
#if CAN_XCVR_BENCH_TEST
        probe_rx++;
        ESP_LOGI(TAG, "XCVR RX id=0x%lX dlc=%u",
                 (unsigned long)msg.identifier, (unsigned)msg.data_length_code);
#endif

        switch (msg.identifier) {
        case CAN_RPM_ID:
            state->rpm = le_u16(msg.data);
            state->last_fcu_ms = t;
            state->last_rpm_ms = t;
            break;
        case CAN_SPEED_ID:
            state->speed = le_u16(msg.data);
            state->last_rcu_ms = t;
            state->last_speed_ms = t;
            break;
        case CAN_CVT_TEMP_ID: {
            /* °C×100. RCU manda -1 se MLX falhou (desconectado). */
            int16_t cvt = le_i16(msg.data);
            state->last_rcu_ms = t;
            if (cvt < 0) {
                state->cvt_temp = -1; /* SNA */
                state->last_cvt_ms = 0; /* força FLAG_RCU_MLX_OK off */
            } else {
                state->cvt_temp = cvt;
                state->last_cvt_ms = t;
            }
            break;
        }
        case CAN_ENG_TEMP_ID: {
            /* °C×100. NTC aberto → ADC alto → calc. negativo/absurdo ou -1. */
            int16_t eng = le_i16(msg.data);
            state->last_rcu_ms = t;
            if (eng < 0 || eng > 18000) {
                state->eng_temp = -1; /* SNA */
                state->last_eng_ms = 0;
            } else {
                state->eng_temp = eng;
                state->last_eng_ms = t;
            }
            break;
        }
        case CAN_VOLTAGE_ID:
            state->voltage = (float)le_u16(msg.data) / 100.0f;
            state->last_rcu_ms = t;
            state->last_volt_ms = t;
            break;
        case CAN_IMU_ID:
            state->roll = le_i16(&msg.data[0]);
            state->pitch = le_i16(&msg.data[2]);
            state->last_fcu_ms = t;
            state->last_imu_ms = t;
            break;
        case CAN_ACC_ID:
            state->acc_x = le_i16(&msg.data[0]);
            state->acc_y = le_i16(&msg.data[2]);
            state->acc_z = le_i16(&msg.data[4]);
            state->last_fcu_ms = t;
            state->last_imu_ms = t;
            break;
        case CAN_DPS_ID:
            state->dps_x = le_i16(&msg.data[0]);
            state->dps_y = le_i16(&msg.data[2]);
            state->dps_z = le_i16(&msg.data[4]);
            state->last_fcu_ms = t;
            state->last_imu_ms = t;
            break;
        case CAN_GPS_ID:
            /* Origem: FCU (não há GPS nesta placa). */
            memcpy(&state->latitude, &msg.data[0], sizeof(float));
            memcpy(&state->longitude, &msg.data[4], sizeof(float));
            state->last_gps_ms = t;
            break;
        case CAN_GPS_SPEED_ID:
            memcpy(&state->gps_speed, &msg.data[0], sizeof(float));
            state->last_gps_ms = t;
            break;
        case CAN_FCU_DEBUG_ID:
            if (msg.data_length_code >= 5) {
                state->fcu_state = ecu_state_to_wire((int8_t)msg.data[0]);
                state->fcu_can_ok = msg.data[2] != 0;
                state->fcu_rpm_warning = msg.data[3] != 0;
                state->fcu_boot_tries = msg.data[4];
                state->last_fcu_ms = t;
            }
            break;
        case CAN_TCU_DEBUG_ID:
            if (msg.data_length_code >= 4) {
                state->rcu_state = ecu_state_to_wire((int8_t)msg.data[0]);
                state->rcu_can_ok = msg.data[1] != 0;
                state->rcu_boot_tries = msg.data[3];
                state->last_rcu_ms = t;
            }
            break;
        case DEBUG_LED_CONTROL:
            if (msg.data_length_code >= 1) {
                led_cmd_value = msg.data[0] != 0;
                led_cmd_pending = true;
            }
            break;
        case PILOT_BOX_CONTROL:
            if (msg.data_length_code >= 1) {
                g_pilot_box = msg.data[0] != 0;
            }
            break;
        default:
            break;
        }
    }

    can_drain_txq();

    if (ctrl_pending && ctrl_dlc >= 1 && ctrl_dlc <= 8) {
        twai_message_t retry;
        memset(&retry, 0, sizeof(retry));
        retry.identifier = ctrl_id;
        retry.data_length_code = ctrl_dlc;
        memcpy(retry.data, ctrl_data, ctrl_dlc);
        retry.ss = 1;
        if (can_try_transmit(&retry)) {
            ctrl_pending = false;
        }
    }

    can_refresh_link_status(state, t);
    return updated;
}

void can_get_bus_stats(can_bus_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->rx_frames = g_rx_frames;
    out->last_rx_id = g_last_rx_id;
    twai_status_info_t st;
    if (driver_installed && twai_get_status_info(&st) == ESP_OK) {
        out->tx_error_counter = st.tx_error_counter;
        out->rx_error_counter = st.rx_error_counter;
        out->state = (int)st.state;
    }
}
