#ifndef CMCAR_POWER_CONFIG_H
#define CMCAR_POWER_CONFIG_H

/* INA219 dedicated I2C2: PB10 SCL / PB11 SDA, 3.3V logic. */
#define POWER_INA219_ADDRESS 0x40u
#define POWER_SAMPLE_PERIOD_MS 250u
#define POWER_CONVERSION_WAIT_MS 25u
#define POWER_REPORT_PERIOD_MS 1000u
#define POWER_I2C_TIMEOUT_MS 25u /* Includes preemption by higher-priority control/IMU tasks. */
/* Current assembled board: user confirmed R100=100mOhm on 2026-11-12.
 * Hardware presets supply 100; direct/unconfigured builds fail closed at 0.
 * Other resistors: R100=100, R010=10.
 * 0 intentionally disables current/power output; raw shunt uV still works. */
#ifndef CMCAR_POWER_SHUNT_MILLIOHM
#define CMCAR_POWER_SHUNT_MILLIOHM 0u
#endif
#endif
