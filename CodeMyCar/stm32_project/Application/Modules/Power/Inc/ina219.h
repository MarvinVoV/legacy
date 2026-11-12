#ifndef CMCAR_INA219_H
#define CMCAR_INA219_H
#include <stdbool.h>
#include <stdint.h>

/* Portable register transport. The hardware adapter owns the bus and timeout. */
typedef bool (*Ina219Read)(void *ctx, uint8_t reg, uint16_t *value);
typedef bool (*Ina219Write)(void *ctx, uint8_t reg, uint16_t value);
typedef struct { void *ctx; Ina219Read read; Ina219Write write; } Ina219;
typedef enum {
    INA219_OK = 0, INA219_IO = 1, INA219_NOT_READY = 2,
    INA219_RANGE = 3, INA219_CONFIG = 4
} Ina219Result;
typedef struct { uint16_t busMv; int32_t shuntUv; } Ina219Sample;

/* Single shot, bus + shunt, +/-320mV, 16 ADC samples each (~17.02ms). */
#define INA219_CONFIG_WORD 0x3e63u
Ina219Result Ina219_Configure(Ina219 *device);
Ina219Result Ina219_Trigger(Ina219 *device);
Ina219Result Ina219_Read(Ina219 *device, Ina219Sample *sample);
#endif
