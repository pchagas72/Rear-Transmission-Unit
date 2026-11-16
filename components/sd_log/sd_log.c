#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "board_pins.h"
#include "sd_log.h"

static const char *TAG = "SD_LOG";
#define MOUNT_POINT "/sdcard"

static FILE *log_file = NULL;
static int sync_counter = 0;
static char log_path[32];
static bool sd_ready;
static bool sd_mounted;
static SemaphoreHandle_t s_mtx;
static volatile bool s_busy;
static int s_busy_depth;

static void sd_lock(void)
{
    if (s_mtx) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
    }
}

static void sd_unlock(void)
{
    if (s_mtx) {
        xSemaphoreGive(s_mtx);
    }
}

static const char *path_basename(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? (s + 1) : path;
}

static void close_active_unlocked(void)
{
    if (log_file != NULL) {
        fflush(log_file);
        fsync(fileno(log_file));
        fclose(log_file);
        log_file = NULL;
    }
    sd_ready = false;
    sync_counter = 0;
}

bool sd_log_ready(void)
{
    return sd_ready;
}

static unsigned next_log_index(void)
{
    unsigned max_n = 0;
    DIR *dir = opendir(MOUNT_POINT);
    if (dir == NULL) {
        return 1;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        unsigned n = 0;
        if (sscanf(ent->d_name, "LOG%u.BIN", &n) == 1 ||
            sscanf(ent->d_name, "LOG%u.bin", &n) == 1 ||
            sscanf(ent->d_name, "log%u.bin", &n) == 1) {
            if (n > max_n) {
                max_n = n;
            }
        }
    }
    closedir(dir);
    return max_n + 1;
}

static bool try_fopen(const char *path)
{
    log_file = fopen(path, "wb");
    if (log_file == NULL) {
        ESP_LOGE(TAG, "fopen %s failed (errno=%d %s)", path, errno, strerror(errno));
        return false;
    }
    strncpy(log_path, path, sizeof(log_path) - 1);
    log_path[sizeof(log_path) - 1] = '\0';
    ESP_LOGI(TAG, "log file: %s", log_path);
    return true;
}

static void open_boot_log_file(void)
{
    unsigned idx = next_log_index();
    if (idx < 1) {
        idx = 1;
    }
    if (idx > 99999) {
        idx = 99999;
    }

    char path[32];
    snprintf(path, sizeof(path), MOUNT_POINT "/LOG%04u.BIN", idx);
    if (try_fopen(path)) {
        return;
    }
    if (try_fopen(MOUNT_POINT "/LOG.BIN")) {
        ESP_LOGW(TAG, "fallback LOG.BIN");
        return;
    }
    ESP_LOGE(TAG, "SD mounted but not writable");
}

const char *sd_log_current_path(void)
{
    if (!sd_ready || log_path[0] == '\0') {
        return "";
    }
    return log_path;
}

void sd_log_flush(void)
{
    sd_lock();
    if (log_file != NULL) {
        fflush(log_file);
        fsync(fileno(log_file));
        sync_counter = 0;
    }
    sd_unlock();
}

bool sd_log_busy(void)
{
    return s_busy;
}

void sd_log_busy_enter(const char *why)
{
    sd_lock();
    s_busy_depth++;
    s_busy = true;
    if (log_file != NULL) {
        fflush(log_file);
        fsync(fileno(log_file));
        sync_counter = 0;
    }
    sd_unlock();
    ESP_LOGI(TAG, "busy ENTER depth=%d (%s)", s_busy_depth, why ? why : "?");
}

void sd_log_busy_exit(void)
{
    sd_lock();
    if (s_busy_depth > 0) {
        s_busy_depth--;
    }
    if (s_busy_depth == 0) {
        s_busy = false;
    }
    sd_unlock();
    ESP_LOGI(TAG, "busy EXIT depth=%d", s_busy_depth);
}

static bool name_is_log_bin(const char *name)
{
    if (name == NULL) {
        return false;
    }
    /* LOG + dígitos + .BIN — sem path traversal. */
    if (strncmp(name, "LOG", 3) != 0 && strncmp(name, "log", 3) != 0) {
        return false;
    }
    const char *p = name + 3;
    int digits = 0;
    while (*p && isdigit((unsigned char)*p)) {
        digits++;
        p++;
    }
    if (digits < 1 || digits > 5) {
        return false;
    }
    if (strcasecmp(p, ".BIN") != 0) {
        return false;
    }
    for (const char *c = name; *c; c++) {
        if (*c == '/' || *c == '\\') {
            return false;
        }
    }
    return true;
}

int sd_log_list_json(char *buf, size_t cap)
{
    if (buf == NULL || cap < 4) {
        return 0;
    }
    if (!sd_mounted) {
        return snprintf(buf, cap, "[]");
    }
    sd_lock();
    DIR *dir = opendir(MOUNT_POINT);
    if (dir == NULL) {
        sd_unlock();
        return snprintf(buf, cap, "[]");
    }
    int n = 0;
    int w = snprintf(buf + n, cap - (size_t)n, "[");
    if (w < 0) {
        closedir(dir);
        sd_unlock();
        return 0;
    }
    n += w;
    bool first = true;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!name_is_log_bin(ent->d_name)) {
            continue;
        }
        char full[40];
        if (snprintf(full, sizeof(full), MOUNT_POINT "/%s", ent->d_name) >= (int)sizeof(full)) {
            continue;
        }
        struct stat st;
        uint32_t sz = 0;
        if (stat(full, &st) == 0 && st.st_size >= 0) {
            sz = (uint32_t)st.st_size;
        }
        w = snprintf(buf + n, cap - (size_t)n,
                     "%s{\"name\":\"%s\",\"size\":%lu}",
                     first ? "" : ",",
                     ent->d_name, (unsigned long)sz);
        if (w < 0 || (size_t)(n + w) >= cap) {
            break;
        }
        n += w;
        first = false;
    }
    closedir(dir);
    sd_unlock();
    w = snprintf(buf + n, cap - (size_t)n, "]");
    if (w > 0 && (size_t)(n + w) < cap) {
        n += w;
    }
    return n;
}

FILE *sd_log_open_read(const char *name, uint32_t *size_out)
{
    if (size_out) {
        *size_out = 0;
    }
    if (!sd_mounted || !name_is_log_bin(name)) {
        return NULL;
    }
    sd_lock();
    if (log_file != NULL) {
        fflush(log_file);
        fsync(fileno(log_file));
        sync_counter = 0;
    }
    char full[40];
    if (snprintf(full, sizeof(full), MOUNT_POINT "/%s", name) >= (int)sizeof(full)) {
        sd_unlock();
        return NULL;
    }
    struct stat st;
    if (stat(full, &st) != 0 || st.st_size < 0) {
        sd_unlock();
        return NULL;
    }
    FILE *f = fopen(full, "rb");
    sd_unlock();
    if (f == NULL) {
        return NULL;
    }
    if (size_out) {
        *size_out = (uint32_t)st.st_size;
    }
    return f;
}

bool sd_log_erase(const char *name)
{
    if (!sd_mounted || !name_is_log_bin(name)) {
        return false;
    }
    char full[40];
    if (snprintf(full, sizeof(full), MOUNT_POINT "/%s", name) >= (int)sizeof(full)) {
        return false;
    }

    sd_lock();
    bool was_active = (log_file != NULL && strcasecmp(path_basename(log_path), name) == 0);
    if (was_active) {
        close_active_unlocked();
        log_path[0] = '\0';
    }
    int rc = unlink(full);
    if (rc != 0) {
        ESP_LOGW(TAG, "erase %s fail errno=%d %s", name, errno, strerror(errno));
        if (was_active) {
            open_boot_log_file();
            sd_ready = (log_file != NULL);
        }
        sd_unlock();
        return false;
    }
    ESP_LOGI(TAG, "erased %s%s", name, was_active ? " (era ativo)" : "");
    if (was_active) {
        open_boot_log_file();
        sd_ready = (log_file != NULL);
    }
    sd_unlock();
    return true;
}

int sd_log_erase_all(void)
{
    if (!sd_mounted) {
        return -1;
    }

    sd_lock();
    close_active_unlocked();
    log_path[0] = '\0';

    DIR *dir = opendir(MOUNT_POINT);
    if (dir == NULL) {
        open_boot_log_file();
        sd_ready = (log_file != NULL);
        sd_unlock();
        return -1;
    }

    int n_del = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!name_is_log_bin(ent->d_name)) {
            continue;
        }
        char full[40];
        if (snprintf(full, sizeof(full), MOUNT_POINT "/%s", ent->d_name) >= (int)sizeof(full)) {
            continue;
        }
        if (unlink(full) == 0) {
            n_del++;
            ESP_LOGI(TAG, "erased %s", ent->d_name);
        } else {
            ESP_LOGW(TAG, "erase %s fail errno=%d", ent->d_name, errno);
        }
        /* Yield mantendo o mutex: WiFi/httpd seguem; evita corrida FATFS. */
        vTaskDelay(1);
    }
    closedir(dir);

    open_boot_log_file();
    sd_ready = (log_file != NULL);
    sd_unlock();
    ESP_LOGI(TAG, "erase_all removed %d file(s)", n_del);
    return n_del;
}

void sd_log_init(void)
{
    sd_ready = false;
    sd_mounted = false;
    log_path[0] = '\0';
    if (s_mtx == NULL) {
        s_mtx = xSemaphoreCreateMutex();
    }
    ESP_LOGI(TAG, "SD SPI CS=%d MOSI=%d CLK=%d MISO=%d",
             (int)PIN_SD_CS, (int)PIN_SD_MOSI, (int)PIN_SD_CLK, (int)PIN_SD_MISO);

    if (PIN_SD_CS < 0) {
        ESP_LOGW(TAG, "SD CS ausente em connections.md — mount skip (HW deve definir CS)");
        return;
    }

    /* Protoboard: limpar strapping residual antes do SPI. */
    gpio_reset_pin(PIN_SD_CS);
    gpio_reset_pin(PIN_SD_MOSI);
    gpio_reset_pin(PIN_SD_MISO);
    gpio_reset_pin(PIN_SD_CLK);

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t *card = NULL;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_SD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .sclk_io_num = PIN_SD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed (%s)", esp_err_to_name(ret));
        return;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    /* Jumpers longos: começar em 400 kHz (PROBING). */
    host.max_freq_khz = SDMMC_FREQ_PROBING;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_SD_CS;
    slot_config.host_id = SPI2_HOST;

    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mount failed (%s) — checar cartao/CS%d/MISO%d/MOSI%d/CLK%d/3V3",
                 esp_err_to_name(ret),
                 (int)PIN_SD_CS, (int)PIN_SD_MISO, (int)PIN_SD_MOSI, (int)PIN_SD_CLK);
        return;
    }

    ESP_LOGI(TAG, "SD mounted");
    sd_mounted = true;
    sdmmc_card_print_info(stdout, card);
    open_boot_log_file();
    sd_ready = (log_file != NULL);
}

void sd_log_data(SD_Storage_Packet *pkt)
{
    if (pkt == NULL || s_busy) {
        return; /* box/download: SPI exclusivo para SoftAP */
    }
    sd_lock();
    if (log_file == NULL || s_busy) {
        sd_unlock();
        return;
    }
    fwrite(pkt, sizeof(SD_Storage_Packet), 1, log_file);
    sync_counter++;
    if (sync_counter >= 50) {
        fflush(log_file);
        fsync(fileno(log_file));
        sync_counter = 0;
    }
    sd_unlock();
}
