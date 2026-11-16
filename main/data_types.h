#ifndef DATA_TYPES_H
#define DATA_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "can_mgmt.h"
#include "telemetry_packets.h"

#define SD_QUEUE_SIZE 256  /* buffer se SPI/SD atrasar (download/erase) */

typedef struct {
    QueueHandle_t telemetry;
    QueueHandle_t sd;
    SemaphoreHandle_t tx_trigger;
    SemaphoreHandle_t ack_mutex;
} rtu_queues_t;

typedef struct {
    uint16_t pending_ack_flags;
    uint8_t pending_ack_ttl;
    uint8_t pending_soc_ack;
    uint16_t lora_seq;
    bool led_debug;
    TickType_t e32_last_rx_tick;
} rtu_runtime_t;

#endif
