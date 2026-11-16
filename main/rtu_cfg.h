#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define RTU_CFG_PASS_MAX   32
#define RTU_CFG_TOKEN_MAX  32

/* Defaults de fábrica (gravados na NVS na 1ª boot). */
#define RTU_CFG_DEFAULT_AP_PASS  "manguebaja"
#define RTU_CFG_DEFAULT_TOKEN    "manguebaja"
#define RTU_CFG_DEFAULT_LORA_CH  0x06

esp_err_t rtu_cfg_init(void);

const char *rtu_cfg_ap_pass(void);
const char *rtu_cfg_api_token(void);
uint8_t rtu_cfg_lora_chan(void);

bool rtu_cfg_token_ok(const char *got);

/* Atualiza e persiste. Strings vazias = sem mudança. pass/token min 8 chars. */
esp_err_t rtu_cfg_set(const char *ap_pass, const char *api_token, int lora_chan /* -1 = keep */);
