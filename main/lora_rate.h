#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Cadência do pacote no ar. SoftAP continua ~20 ms; isto só muda o E32.
 * Períodos abaixo de 500 ms não cabem no air rate 2.4 kbps (SPED 0x1A). */

#define LORA_RATE_MIN_MS     500u
#define LORA_RATE_DEFAULT_MS 1000u
#define LORA_RATE_MAX_MS     10000u

void lora_rate_init(void);

/* Semáforo binário do lora_tx: ao mudar o período, acorda a task. */
void lora_rate_bind_kick(SemaphoreHandle_t kick);

uint32_t lora_rate_ms(void);

/* true só se ms está na tabela discreta. Persiste em NVS. */
bool lora_rate_set_ms(uint32_t ms);

/* Chamado após TX LoRa ok. Mede o intervalo real entre envios. */
void lora_rate_note_tx(void);
uint32_t lora_rate_gap_ms(void);

/* {"ms":1000,"gap_ms":1004,"opts":[500,1000,2000,5000,10000]} */
int lora_rate_json(char *buf, size_t cap);
