#ifndef SD_LOG_H
#define SD_LOG_H

#include "telemetry_packets.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

void sd_log_init(void);
bool sd_log_ready(void);
void sd_log_data(SD_Storage_Packet *pkt);

/* SoftAP: caminho do log ativo ("" se nenhum). */
const char *sd_log_current_path(void);

/* Flush do log ativo antes de download. */
void sd_log_flush(void);

/*
 * Modo box/manutenção: pausa gravação SD para download/erase exclusivos.
 * SoftAP / LoRa devem consultar sd_log_busy() e aliviar carga.
 */
void sd_log_busy_enter(const char *why);
void sd_log_busy_exit(void);
bool sd_log_busy(void);

/*
 * Lista LOG*.BIN em JSON: [{"name":"LOG0001.BIN","size":123},...]
 * Retorna bytes escritos ou 0.
 */
int sd_log_list_json(char *buf, size_t cap);

/* Abre arquivo sob /sdcard se o nome for LOG####.BIN seguro. */
FILE *sd_log_open_read(const char *name, uint32_t *size_out);

/*
 * Apaga um LOG*.BIN. Se for o log ativo, fecha, apaga e abre o próximo.
 * Retorna true se unlink ok.
 */
bool sd_log_erase(const char *name);

/* Apaga todos LOG*.BIN; reabre log novo. Retorna quantos foram removidos (-1 erro). */
int sd_log_erase_all(void);

#endif
