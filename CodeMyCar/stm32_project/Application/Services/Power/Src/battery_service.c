#include "battery_service.h"
#include <string.h>

bool BatteryService_VoltagePercent(uint16_t packMv, uint8_t *percent)
{
    static const uint16_t mv[] = {9000,9900,10500,10800,11100,11400,11700,12000,12300,12600};
    static const uint8_t pc[] = {0,5,10,20,40,60,75,85,95,100};
    if (!percent || packMv < 7500u || packMv > 12900u) return false;
    if (packMv <= mv[0]) { *percent = 0; return true; }
    for (unsigned i = 1; i < sizeof(pc); ++i) {
        if (packMv <= mv[i]) {
            *percent = (uint8_t)(pc[i-1] +
                ((uint32_t)(packMv-mv[i-1])*(pc[i]-pc[i-1]) + (mv[i]-mv[i-1])/2u) /
                (mv[i]-mv[i-1]));
            return true;
        }
    }
    *percent = 100;
    return true;
}

void BatteryService_Update(BatteryService *s, Ina219Result rc,
                           const Ina219Sample *sample, uint32_t now, BatteryReading *r)
{
    memset(r, 0, sizeof(*r));
    r->timestamp = now; r->error = (uint8_t)rc;
    r->address = s->address; r->shuntMilliOhm = s->shuntMilliOhm;
    if (rc != INA219_OK || !sample) {
        if (rc == INA219_OK) r->error = INA219_CONFIG;
        s->filterReady = false;
        return;
    }
    int32_t batteryMv = (int32_t)sample->busMv + sample->shuntUv / 1000;
    if (batteryMv < 0 || batteryMv > 26320) {
        r->error = INA219_RANGE; s->filterReady = false; return;
    }
    r->busMv = sample->busMv; r->batteryMv = (uint16_t)batteryMv;
    r->shuntUv = sample->shuntUv; r->flags = BATTERY_VALID;
    /* 4Hz sampling, EMA alpha=1/8 (~1.9s time constant). */
    if (!s->filterReady) { s->filteredMvQ8 = batteryMv * 256; s->filterReady = true; }
    else s->filteredMvQ8 += (batteryMv * 256 - s->filteredMvQ8) / 8;
    r->filteredMv = (uint16_t)((s->filteredMvQ8 + 128) / 256);
    /* An unplugged/out-of-profile supply must not retain an old percentage. */
    uint8_t ignored;
    if (BatteryService_VoltagePercent(r->batteryMv, &ignored) &&
        BatteryService_VoltagePercent(r->filteredMv, &r->percent)) r->flags |= BATTERY_SOC_VALID;
    else { s->filterReady = false; r->filteredMv = r->batteryMv; }
    if (s->shuntMilliOhm != 0u) {
        r->currentMa = sample->shuntUv / s->shuntMilliOhm;
        r->powerMw = (int32_t)((int64_t)r->batteryMv * r->currentMa / 1000);
        r->flags |= BATTERY_CURRENT_CALIBRATED;
    }
}

static void put16(uint8_t *p, uint16_t n) { p[0]=(uint8_t)n; p[1]=(uint8_t)(n>>8); }
static void put32(uint8_t *p, uint32_t n) {
    p[0]=(uint8_t)n; p[1]=(uint8_t)(n>>8); p[2]=(uint8_t)(n>>16); p[3]=(uint8_t)(n>>24);
}
void BatteryService_Pack(const BatteryReading *r, uint8_t out[BATTERY_FRAME_SIZE])
{
    put32(out, r->timestamp); out[4]=5u; /* SENSOR_BATTERY */
    put16(out+5,r->busMv); put16(out+7,r->batteryMv);
    put32(out+9,(uint32_t)r->shuntUv); put32(out+13,(uint32_t)r->currentMa);
    put32(out+17,(uint32_t)r->powerMw); out[21]=r->percent;
    out[22]=r->flags; out[23]=r->error; out[24]=r->address;
    put16(out+25,r->shuntMilliOhm); put16(out+27,r->filteredMv);
}
