#include "ina219.h"
#include <stddef.h>

Ina219Result Ina219_Trigger(Ina219 *d)
{
    if (!d || !d->write || !d->read) return INA219_CONFIG;
    return d->write(d->ctx, 0u, INA219_CONFIG_WORD) ? INA219_OK : INA219_IO;
}

Ina219Result Ina219_Configure(Ina219 *d)
{
    Ina219Result rc = Ina219_Trigger(d);
    if (rc != INA219_OK) return rc;
    uint16_t value;
    if (!d->read(d->ctx, 0u, &value)) return INA219_IO;
    /* Mode may return to power-down after a triggered conversion. */
    return (value & 0x3ff8u) == (INA219_CONFIG_WORD & 0x3ff8u)
        ? INA219_OK : INA219_CONFIG;
}

Ina219Result Ina219_Read(Ina219 *d, Ina219Sample *out)
{
    if (!d || !d->read || !out) return INA219_CONFIG;
    uint16_t bus, shunt;
    if (!d->read(d->ctx, 2u, &bus)) return INA219_IO;
    if (!(bus & 2u)) return INA219_NOT_READY;
    if (!d->read(d->ctx, 1u, &shunt)) return INA219_IO;
    const int32_t uv = (int32_t)(int16_t)shunt * 10;
    const uint16_t mv = (uint16_t)((bus >> 3) * 4u);
    /* Never pass saturation through as a believable current reading. */
    if ((bus & 1u) || mv > 26000u || uv >= 320000 || uv <= -320000)
        return INA219_RANGE;
    *out = (Ina219Sample){.busMv = mv, .shuntUv = uv};
    return INA219_OK;
}
