#include "rtu_cfg.h"

#include <stdio.h>
#include <string.h>
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "RTU_CFG";
static const char *NS = "rtu";

static char g_ap_pass[RTU_CFG_PASS_MAX];
static char g_token[RTU_CFG_TOKEN_MAX];
static uint8_t g_lora_ch;
static bool g_ready;

static void set_defaults(void)
{
    strncpy(g_ap_pass, RTU_CFG_DEFAULT_AP_PASS, sizeof(g_ap_pass) - 1);
    g_ap_pass[sizeof(g_ap_pass) - 1] = '\0';
    strncpy(g_token, RTU_CFG_DEFAULT_TOKEN, sizeof(g_token) - 1);
    g_token[sizeof(g_token) - 1] = '\0';
    g_lora_ch = RTU_CFG_DEFAULT_LORA_CH;
}

esp_err_t rtu_cfg_init(void)
{
    set_defaults();
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed (%s) — defaults", esp_err_to_name(err));
        g_ready = true;
        return err;
    }

    bool dirty = false;
    size_t len = sizeof(g_ap_pass);
    if (nvs_get_str(h, "ap_pass", g_ap_pass, &len) == ESP_ERR_NVS_NOT_FOUND) {
        strncpy(g_ap_pass, RTU_CFG_DEFAULT_AP_PASS, sizeof(g_ap_pass) - 1);
        (void)nvs_set_str(h, "ap_pass", g_ap_pass);
        dirty = true;
    }
    g_ap_pass[sizeof(g_ap_pass) - 1] = '\0';

    len = sizeof(g_token);
    if (nvs_get_str(h, "token", g_token, &len) == ESP_ERR_NVS_NOT_FOUND) {
        strncpy(g_token, RTU_CFG_DEFAULT_TOKEN, sizeof(g_token) - 1);
        (void)nvs_set_str(h, "token", g_token);
        dirty = true;
    }
    g_token[sizeof(g_token) - 1] = '\0';

    uint8_t ch = 0;
    if (nvs_get_u8(h, "lora_ch", &ch) == ESP_ERR_NVS_NOT_FOUND) {
        g_lora_ch = RTU_CFG_DEFAULT_LORA_CH;
        (void)nvs_set_u8(h, "lora_ch", g_lora_ch);
        dirty = true;
    } else {
        g_lora_ch = ch;
    }

    if (dirty) {
        (void)nvs_commit(h);
    }
    nvs_close(h);
    g_ready = true;
    ESP_LOGI(TAG, "cfg ready ap_pass_len=%u token_len=%u lora_ch=0x%02X",
             (unsigned)strlen(g_ap_pass), (unsigned)strlen(g_token), g_lora_ch);
    return ESP_OK;
}

const char *rtu_cfg_ap_pass(void)
{
    return g_ready ? g_ap_pass : RTU_CFG_DEFAULT_AP_PASS;
}

const char *rtu_cfg_api_token(void)
{
    return g_ready ? g_token : RTU_CFG_DEFAULT_TOKEN;
}

uint8_t rtu_cfg_lora_chan(void)
{
    return g_ready ? g_lora_ch : RTU_CFG_DEFAULT_LORA_CH;
}

bool rtu_cfg_token_ok(const char *got)
{
    if (got == NULL || got[0] == '\0') {
        return false;
    }
    const char *exp = rtu_cfg_api_token();
    size_t n = strlen(exp);
    if (n == 0 || strlen(got) != n) {
        return false;
    }
    /* strcmp ok — token curto; timing attack irrelevante no SoftAP local. */
    return strcmp(got, exp) == 0;
}

static bool valid_secret(const char *s)
{
    if (s == NULL) {
        return false;
    }
    size_t n = strlen(s);
    return n >= 8 && n < RTU_CFG_PASS_MAX;
}

esp_err_t rtu_cfg_set(const char *ap_pass, const char *api_token, int lora_chan)
{
    if (ap_pass && ap_pass[0] && !valid_secret(ap_pass)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (api_token && api_token[0] && !valid_secret(api_token)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (lora_chan >= 0 && (lora_chan > 0x1F)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    if (ap_pass && ap_pass[0]) {
        strncpy(g_ap_pass, ap_pass, sizeof(g_ap_pass) - 1);
        g_ap_pass[sizeof(g_ap_pass) - 1] = '\0';
        err = nvs_set_str(h, "ap_pass", g_ap_pass);
        if (err != ESP_OK) {
            nvs_close(h);
            return err;
        }
    }
    if (api_token && api_token[0]) {
        strncpy(g_token, api_token, sizeof(g_token) - 1);
        g_token[sizeof(g_token) - 1] = '\0';
        err = nvs_set_str(h, "token", g_token);
        if (err != ESP_OK) {
            nvs_close(h);
            return err;
        }
    }
    if (lora_chan >= 0) {
        g_lora_ch = (uint8_t)lora_chan;
        err = nvs_set_u8(h, "lora_ch", g_lora_ch);
        if (err != ESP_OK) {
            nvs_close(h);
            return err;
        }
    }
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "cfg updated ap_pass_len=%u token_len=%u lora_ch=0x%02X",
             (unsigned)strlen(g_ap_pass), (unsigned)strlen(g_token), g_lora_ch);
    return err;
}
