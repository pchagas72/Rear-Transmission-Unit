#ifndef EBYTE_E32_H
#define EBYTE_E32_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "board_pins.h"

#define E32_UART_NUM PIN_E32_UART_NUM

/* Tetos do envio. O ciclo da task LoRa soma estes limites. */
#define E32_AUX_TIMEOUT_MS    800
#define E32_AUX_POLL_MS       5
#define E32_UART_TX_TIMEOUT_MS 300
#define E32_PAYLOAD_MAX       64
#define E32_FRAME_MAX         (4 + E32_PAYLOAD_MAX)

void e32_init(void);
bool e32_aux_ready(void);
/* Telemetria: marker AA BB CC DD + payload. */
bool e32_send_struct(void *data, size_t size);
/* Bytes crus (ex.: frame de comando A5 5A… da estação). */
bool e32_send_raw(const void *data, size_t size);

#endif
