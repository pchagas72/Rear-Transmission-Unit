#include "telemetry_agg.h"
#include "can_mgmt.h"
#include "rtu_diag.h"
#include <assert.h>
#include <string.h>

static_assert(sizeof(telemetry_packet_t) == 52, "pacote LoRa travado em 52 bytes");

static uint16_t pack_status_flags(const car_state_t *s, bool led_debug)
{
    const bool fcu_down = rtu_diag_fcu_down_confirmed();
    const bool rcu_down = rtu_diag_rcu_down_confirmed();
    uint16_t flags = 0;
    if (fcu_down) {
        flags |= FLAG_FCU_LINK_DOWN;
    } else {
        if (s->fcu_imu_ok) {
            flags |= FLAG_FCU_IMU_OK;
        }
        if (s->fcu_can_ok) {
            flags |= FLAG_FCU_CAN_OK;
        }
        if (s->fcu_rpm_warning) {
            flags |= FLAG_FCU_RPM_WARN;
        }
    }
    if (rcu_down) {
        flags |= FLAG_RCU_LINK_DOWN;
    } else {
        if (s->rcu_can_ok) {
            flags |= FLAG_RCU_CAN_OK;
        }
        if (s->rcu_mlx_ok) {
            flags |= FLAG_RCU_MLX_OK;
        }
    }
    if (fcu_down || rcu_down) {
        flags |= FLAG_LINK_DOWN;
    }
    if (s->gps_from_can) {
        flags |= FLAG_GPS_OK;
    }
    if (led_debug) {
        flags |= FLAG_LED_DEBUG;
    }
    return flags;
}

void telemetry_fill(telemetry_packet_t *pkt, const car_state_t *car, bool led_debug)
{
    assert(pkt != NULL);
    assert(car != NULL);
    if (pkt == NULL || car == NULL) {
        return;
    }
    const bool fcu_down = rtu_diag_fcu_down_confirmed();
    const bool rcu_down = rtu_diag_rcu_down_confirmed();
    memset(pkt, 0, sizeof(*pkt));
    pkt->voltage = car->voltage;
    pkt->rpm = car->rpm;
    pkt->speed = car->speed;
    pkt->cvt_temp = car->cvt_temp;
    pkt->eng_temp = car->eng_temp;
    pkt->roll = car->roll;
    pkt->pitch = car->pitch;
    pkt->acc_x = car->acc_x;
    pkt->acc_y = car->acc_y;
    pkt->acc_z = car->acc_z;
    pkt->dps_x = car->dps_x;
    pkt->dps_y = car->dps_y;
    pkt->dps_z = car->dps_z;
    pkt->latitude = car->latitude;
    pkt->longitude = car->longitude;
    pkt->gps_speed = car->gps_speed;
    pkt->fcu_state = fcu_down ? ECU_STATE_DOWN : car->fcu_state;
    pkt->rcu_state = rcu_down ? ECU_STATE_DOWN : car->rcu_state;
    pkt->fcu_boot_tries = car->fcu_boot_tries;
    pkt->rcu_boot_tries = car->rcu_boot_tries;
    pkt->flags = pack_status_flags(car, led_debug);
    pkt->soc = can_pilot_box_latched() ? TELEM_SOC_PILOT_BOX : 0;
    pkt->crc8 = telemetry_crc8(pkt);
}
