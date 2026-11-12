#include "mqtt_codec.h"
#include <string.h>
#include <math.h>
#include "cJSON.h"

// Helper para evitar floats gigantes no JSON (ex: 25.400001525878906)
static inline double round2(float val) {
    return round((double)val * 100.0) / 100.0;
}

int mqtt_codec_encode_telemetry(const telemetry_packet_t *pkt, char *out_buf, size_t buf_size) {
    if (!pkt || !out_buf || buf_size == 0) {
        return -1;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return -1;
    }

    // Substitua os campos abaixo pelos membros reais da sua struct telemetry_packet_t
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddNumberToObject(root, "ts", pkt->timestamp_ms);
    cJSON_AddNumberToObject(root, "rpm", pkt->rpm);
    cJSON_AddNumberToObject(root, "t_oil", round2(pkt->oil_temp));
    cJSON_AddNumberToObject(root, "v_bat", round2(pkt->battery_v));
    cJSON_AddNumberToObject(root, "gear", pkt->gear);
    cJSON_AddNumberToObject(root, "err", pkt->error_flags);

    // Serializa sem espaços no buffer pré-alocado (false = unformatted)
    cJSON_bool ok = cJSON_PrintPreallocated(root, out_buf, (int)buf_size, false);
    
    // Libera os nós internos da árvore cJSON
    cJSON_Delete(root);

    if (!ok) {
        return -1; // Buffer insuficiente
    }

    return (int)strlen(out_buf);
}
