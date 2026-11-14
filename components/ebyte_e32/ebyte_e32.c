#include "ebyte_e32.h"
#include <assert.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "EBYTE_E32";

#define E32_AUX_MAX_SPINS (E32_AUX_TIMEOUT_MS / E32_AUX_POLL_MS)

bool e32_aux_ready(void)
{
    int level = gpio_get_level(PIN_E32_AUX);
    assert(level == 0 || level == 1);
    return level != 0;
}

/* No máximo E32_AUX_MAX_SPINS voltas, e nunca além de timeout_ms. */
static bool e32_wait_aux(uint32_t timeout_ms)
{
    assert(timeout_ms > 0);
    assert(timeout_ms <= E32_AUX_TIMEOUT_MS);
    if (timeout_ms == 0 || timeout_ms > E32_AUX_TIMEOUT_MS) {
        return false;
    }
    TickType_t start = xTaskGetTickCount();
    for (int i = 0; i < E32_AUX_MAX_SPINS; i++) {
        if (e32_aux_ready()) {
            return true;
        }
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(timeout_ms)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(E32_AUX_POLL_MS));
    }
    return false;
}

static void e32_set_mode(int m0, int m1)
{
    gpio_set_level(PIN_E32_M0, m0 ? 1 : 0);
    gpio_set_level(PIN_E32_M1, m1 ? 1 : 0);
    vTaskDelay(pdMS_TO_TICKS(40));
}

/* ADD=0000 SPED=0x1A CHAN=0x06 OPT=0x44 — lockstep com estacao_lora.
 * Retorna bytes lidos na resposta C1 (6 = OK). */
static int e32_try_radio_profile(void)
{
    ESP_LOGI(TAG, "cfg probe AUX=%d (pré sleep)", gpio_get_level(PIN_E32_AUX));
    e32_set_mode(1, 1);
    (void)e32_wait_aux(200);
    ESP_LOGI(TAG, "cfg probe AUX=%d (sleep M0=M1=1)", gpio_get_level(PIN_E32_AUX));
    uart_flush(E32_UART_NUM);

    const uint8_t cmd[6] = {0xC0, 0x00, 0x00, 0x1A, 0x06, 0x44};
    uart_write_bytes(E32_UART_NUM, (const char *)cmd, sizeof(cmd));
    (void)uart_wait_tx_done(E32_UART_NUM, pdMS_TO_TICKS(200));
    vTaskDelay(pdMS_TO_TICKS(50));

    uart_flush(E32_UART_NUM);
    const uint8_t rd[3] = {0xC1, 0xC1, 0xC1};
    uart_write_bytes(E32_UART_NUM, (const char *)rd, sizeof(rd));
    (void)uart_wait_tx_done(E32_UART_NUM, pdMS_TO_TICKS(100));

    uint8_t resp[6];
    int n = uart_read_bytes(E32_UART_NUM, resp, sizeof(resp), pdMS_TO_TICKS(500));
    e32_set_mode(0, 0);
    (void)e32_wait_aux(200);
    uart_flush(E32_UART_NUM);

    if (n >= 6) {
        ESP_LOGI(TAG, "radio ADD=%02X%02X SPED=0x%02X CHAN=0x%02X OPT=0x%02X",
                 resp[1], resp[2], resp[3], resp[4], resp[5]);
    }
    return n;
}

static void e32_apply_radio_profile(void)
{
    int n = e32_try_radio_profile();
    if (n >= 6) {
        ESP_LOGI(TAG, "UART integrity OK (TX=%d RX=%d)",
                 (int)PIN_E32_TX, (int)PIN_E32_RX);
        return;
    }

    ESP_LOGW(TAG, "cfg %d B em TX=%d RX=%d — tentando swap UART",
             n, (int)PIN_E32_TX, (int)PIN_E32_RX);
    uart_set_pin(E32_UART_NUM, PIN_E32_RX, PIN_E32_TX,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    n = e32_try_radio_profile();
    if (n >= 6) {
        ESP_LOGW(TAG, "UART integrity OK só com SWAP (ESP TX=%d RX=%d)",
                 (int)PIN_E32_RX, (int)PIN_E32_TX);
        return;
    }

    /* Restaura pinout documentado; TX RF pode ainda funcionar em default. */
    uart_set_pin(E32_UART_NUM, PIN_E32_TX, PIN_E32_RX,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    ESP_LOGE(TAG, "UART integrity FAIL — 0 resposta C1 nas 2 orientações "
             "(cheque M0/M1/AUX/VCC/GND/fio RX)");
}

void e32_init(void)
{
    ESP_LOGI(TAG, "Init M0=%d M1=%d AUX=%d UART TX=%d RX=%d",
             (int)PIN_E32_M0, (int)PIN_E32_M1, (int)PIN_E32_AUX,
             (int)PIN_E32_TX, (int)PIN_E32_RX);

    gpio_reset_pin(PIN_E32_M0);
    gpio_set_direction(PIN_E32_M0, GPIO_MODE_OUTPUT);
    gpio_reset_pin(PIN_E32_M1);
    gpio_set_direction(PIN_E32_M1, GPIO_MODE_OUTPUT);
    e32_set_mode(0, 0);

    gpio_reset_pin(PIN_E32_AUX);
    gpio_set_direction(PIN_E32_AUX, GPIO_MODE_INPUT);
    /* Sem pull-up: AUX flutuante ≠ ready (integridade real). */
    gpio_set_pull_mode(PIN_E32_AUX, GPIO_FLOATING);

    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    uart_driver_install(E32_UART_NUM, 1024, 0, 0, NULL, 0);
    uart_param_config(E32_UART_NUM, &uart_config);
    uart_set_pin(E32_UART_NUM, PIN_E32_TX, PIN_E32_RX,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    e32_apply_radio_profile();
    ESP_LOGI(TAG, "pós-cfg AUX=%d transparent", gpio_get_level(PIN_E32_AUX));
}

bool e32_send_raw(const void *data, size_t size)
{
    assert(data != NULL);
    assert(size > 0 && size <= E32_FRAME_MAX);
    if (data == NULL || size == 0 || size > E32_FRAME_MAX) {
        return false;
    }
    if (!e32_wait_aux(E32_AUX_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "AUX stuck — skip TX");
        return false;
    }
    int written = uart_write_bytes(E32_UART_NUM, data, size);
    if (written < 0 || (size_t)written != size) {
        ESP_LOGW(TAG, "UART escreveu %d de %u", written, (unsigned)size);
        return false;
    }
    if (uart_wait_tx_done(E32_UART_NUM, pdMS_TO_TICKS(E32_UART_TX_TIMEOUT_MS)) != ESP_OK) {
        return false;
    }
    if (!e32_wait_aux(E32_AUX_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "AUX stuck — TX incompleto");
        return false;
    }
    assert(e32_aux_ready());
    return true;
}

bool e32_send_struct(void *data, size_t size)
{
    assert(data != NULL);
    assert(size > 0 && size <= E32_PAYLOAD_MAX);
    if (data == NULL || size == 0 || size > E32_PAYLOAD_MAX) {
        return false;
    }
    const uint8_t start_marker[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    uint8_t frame[E32_FRAME_MAX];
    assert(sizeof(frame) == E32_FRAME_MAX);
    memcpy(frame, start_marker, sizeof(start_marker));
    memcpy(frame + sizeof(start_marker), data, size);
    return e32_send_raw(frame, sizeof(start_marker) + size);
}
