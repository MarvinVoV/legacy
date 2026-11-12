#ifndef CMCAR_BATTERY_SERVICE_H
#define CMCAR_BATTERY_SERVICE_H
#include "ina219.h"

#define BATTERY_FRAME_SIZE 29u
#define BATTERY_VALID 1u
#define BATTERY_CURRENT_CALIBRATED 2u
#define BATTERY_SOC_VALID 4u

typedef struct {
    /* Zero means unknown; no current/power is published until configured. */
    uint16_t shuntMilliOhm;
    uint8_t address;
    bool filterReady;
    int32_t filteredMvQ8;
} BatteryService;
typedef struct {
    uint32_t timestamp;
    uint16_t busMv, batteryMv, filteredMv, shuntMilliOhm;
    int32_t shuntUv, currentMa, powerMw;
    uint8_t percent, flags, error, address;
} BatteryReading;

/* Generic 3S / 4.2V-cell Li-ion voltage estimate, not a fuel gauge. */
bool BatteryService_VoltagePercent(uint16_t packMv, uint8_t *percent);
void BatteryService_Update(BatteryService *service, Ina219Result result,
                           const Ina219Sample *sample, uint32_t now,
                           BatteryReading *reading);
void BatteryService_Pack(const BatteryReading *reading, uint8_t out[BATTERY_FRAME_SIZE]);
#endif
