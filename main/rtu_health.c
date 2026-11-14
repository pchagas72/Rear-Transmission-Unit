#include "rtu_health.h"
#include "rtu_fw.h"
#include "lora_rate.h"
#include "system_fsm.h"
#include "sd_log.h"
#include "ebyte_e32.h"
#include "can_mgmt.h"

#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "RTU_HEALTH";
static uint32_t g_boot_count;
static uint32_t g_lora_ok;
static uint32_t g_lora_fail;
static int g_reset_reason;

void rtu_health_init(void)
{
    g_reset_reason = (int)esp_reset_reason();
    g_boot_count = 1;
    g_lora_ok = 0;
    g_lora_fail = 0;

    nvs_handle_t h;
    if (nvs_open("rtu", NVS_READWRITE, &h) == ESP_OK) {
        uint32_t n = 0;
        if (nvs_get_u32(h, "boot", &n) != ESP_OK) {
            n = 0;
        }
        n++;
        g_boot_count = n;
        (void)nvs_set_u32(h, "boot", n);
        (void)nvs_set_i32(h, "rst", g_reset_reason);
        (void)nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "boot=%lu reset=%d fw=%s schema=%d",
             (unsigned long)g_boot_count, g_reset_reason, RTU_FW_VER, RTU_FW_SCHEMA);
}

void rtu_health_note_lora(bool ok)
{
    if (ok) {
        g_lora_ok++;
    } else {
        g_lora_fail++;
    }
}

static const char *rst_str(int r)
{
    switch (r) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "SLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "OTHER";
    }
}

void rtu_health_snapshot(rtu_health_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->uptime_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    out->heap_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    out->heap_min = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    out->reset_reason = g_reset_reason;
    out->boot_count = g_boot_count;
    out->sd_ok = sd_log_ready();
    out->e32_aux = e32_aux_ready();
    out->limp = fsm_limp();
    out->fsm = (int)fsm_state();
    out->lora_tx_ok = g_lora_ok;
    out->lora_tx_fail = g_lora_fail;
    can_bus_stats_t can;
    can_get_bus_stats(&can);
    out->can_rx = can.rx_frames;
    const char *path = sd_log_current_path();
    if (path != NULL) {
        strncpy(out->sd_path, path, sizeof(out->sd_path) - 1);
    }
}

int rtu_health_json(char *buf, size_t cap)
{
    if (buf == NULL || cap < 64) {
        return 0;
    }
    rtu_health_t h;
    rtu_health_snapshot(&h);
    int n = snprintf(buf, cap,
                     "{\"fw\":\"%s\",\"schema\":%d,\"uptime_ms\":%lu,"
                     "\"heap\":%lu,\"heap_min\":%lu,\"boot\":%lu,"
                     "\"rst\":\"%s\",\"rst_n\":%d,"
                     "\"sd\":%s,\"sd_path\":\"%s\","
                     "\"e32_aux\":%s,\"limp\":%s,\"fsm\":%d,"
                     "\"lora_ok\":%lu,\"lora_fail\":%lu,\"can_rx\":%lu,"
                     "\"lora_ms\":%lu,\"lora_gap_ms\":%lu}",
                     RTU_FW_VER, RTU_FW_SCHEMA,
                     (unsigned long)h.uptime_ms,
                     (unsigned long)h.heap_free, (unsigned long)h.heap_min,
                     (unsigned long)h.boot_count,
                     rst_str(h.reset_reason), h.reset_reason,
                     h.sd_ok ? "true" : "false", h.sd_path,
                     h.e32_aux ? "true" : "false",
                     h.limp ? "true" : "false", h.fsm,
                     (unsigned long)h.lora_tx_ok, (unsigned long)h.lora_tx_fail,
                     (unsigned long)h.can_rx,
                     (unsigned long)lora_rate_ms(),
                     (unsigned long)lora_rate_gap_ms());
    if (n < 0 || (size_t)n >= cap) {
        return 0;
    }
    return n;
}
