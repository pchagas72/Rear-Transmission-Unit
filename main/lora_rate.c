#include "lora_rate.h"

#include <assert.h>
#include <stdio.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "LORA_RATE";

/* Números fechados: a UI e o rádio só aceitam estes. */
static const uint32_t k_opts[] = { 500u, 1000u, 2000u, 5000u, 10000u };

static volatile uint32_t s_period_ms = LORA_RATE_DEFAULT_MS;
static volatile uint32_t s_gap_ms;
static volatile uint32_t s_last_tx_ms;
static SemaphoreHandle_t s_kick;

static bool allowed(uint32_t ms)
{
    const size_t n = sizeof(k_opts) / sizeof(k_opts[0]);
    assert(n == 5);
    for (size_t i = 0; i < n; i++) {
        if (k_opts[i] == ms) {
            return true;
        }
    }
    return false;
}

static bool persist(uint32_t ms)
{
    assert(allowed(ms));
    nvs_handle_t h;
    if (nvs_open("rtu", NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_u32(h, "lora_ms", ms);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

void lora_rate_init(void)
{
    assert(k_opts[0] == LORA_RATE_MIN_MS);
    assert(k_opts[(sizeof(k_opts) / sizeof(k_opts[0])) - 1] == LORA_RATE_MAX_MS);
    nvs_handle_t h;
    if (nvs_open("rtu", NVS_READONLY, &h) == ESP_OK) {
        uint32_t ms = 0;
        if (nvs_get_u32(h, "lora_ms", &ms) == ESP_OK && allowed(ms)) {
            s_period_ms = ms;
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "periodo=%lums", (unsigned long)s_period_ms);
}

void lora_rate_bind_kick(SemaphoreHandle_t kick)
{
    s_kick = kick;
}

uint32_t lora_rate_ms(void)
{
    uint32_t ms = s_period_ms;
    if (!allowed(ms)) {
        ms = LORA_RATE_DEFAULT_MS;
    }
    assert(allowed(ms));
    assert(ms >= LORA_RATE_MIN_MS && ms <= LORA_RATE_MAX_MS);
    return ms;
}

bool lora_rate_set_ms(uint32_t ms)
{
    if (!allowed(ms)) {
        return false;
    }
    assert(ms >= LORA_RATE_MIN_MS);
    assert(ms <= LORA_RATE_MAX_MS);
    if (s_period_ms == ms) {
        return true;
    }
    s_period_ms = ms;
    if (!persist(ms)) {
        ESP_LOGW(TAG, "NVS nao gravou %lums; vale ate o reboot", (unsigned long)ms);
    }
    ESP_LOGI(TAG, "periodo → %lums", (unsigned long)ms);
    if (s_kick != NULL) {
        BaseType_t kicked = xSemaphoreGive(s_kick);
        assert(kicked == pdTRUE || kicked == pdFALSE);
        (void)kicked;
    }
    return true;
}

void lora_rate_note_tx(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000LL);
    uint32_t prev = s_last_tx_ms;
    if (prev != 0 && now > prev) {
        s_gap_ms = now - prev;
    }
    s_last_tx_ms = now;
    assert(s_last_tx_ms == now);
    assert(allowed(s_period_ms));
}

uint32_t lora_rate_gap_ms(void)
{
    return s_gap_ms;
}

int lora_rate_json(char *buf, size_t cap)
{
    assert(buf != NULL);
    assert(cap >= 32);
    if (buf == NULL || cap < 32) {
        return 0;
    }
    int n = snprintf(buf, cap, "{\"ms\":%lu,\"gap_ms\":%lu,\"opts\":[",
                     (unsigned long)lora_rate_ms(),
                     (unsigned long)lora_rate_gap_ms());
    if (n < 0 || (size_t)n >= cap) {
        return 0;
    }
    for (size_t i = 0; i < sizeof(k_opts) / sizeof(k_opts[0]); i++) {
        int m = snprintf(buf + n, cap - (size_t)n, "%s%lu",
                         i ? "," : "", (unsigned long)k_opts[i]);
        if (m < 0 || (size_t)n + (size_t)m >= cap) {
            return 0;
        }
        n += m;
    }
    int m = snprintf(buf + n, cap - (size_t)n, "]}");
    if (m < 0 || (size_t)n + (size_t)m >= cap) {
        return 0;
    }
    return n + m;
}
