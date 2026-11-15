#include "motion_status.h"

#include <stddef.h>

static void put_u32le(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void put_u16le(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static uint32_t rd_u32le(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16le(const uint8_t* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

int motion_status_pack(const motion_status_t* s, uint8_t out[MOTION_STATUS_SIZE])
{
    if (s == NULL || out == NULL)
    {
        return -1;
    }

    out[0] = s->seq;
    out[1] = s->state;
    out[2] = s->error;
    out[3] = s->flags;
    put_u32le(&out[4], (uint32_t)s->xMm);
    put_u32le(&out[8], (uint32_t)s->yMm);
    put_u32le(&out[12], (uint32_t)s->thetaMdeg);
    put_u32le(&out[16], (uint32_t)s->odometerMm);
    put_u32le(&out[20], (uint32_t)s->goalDistanceMm);
    put_u32le(&out[24], (uint32_t)s->goalTurnMdeg);
    put_u32le(&out[28], s->goalDurationMs);
    put_u16le(&out[32], (uint16_t)s->leftMmps);
    put_u16le(&out[34], (uint16_t)s->rightMmps);
    return 0;
}

int motion_status_unpack(const uint8_t* in, uint8_t len, motion_status_t* s)
{
    if (in == NULL || s == NULL || len < MOTION_STATUS_SIZE)
    {
        return -1;
    }

    s->seq            = in[0];
    s->state          = in[1];
    s->error          = in[2];
    s->flags          = in[3];
    s->xMm            = (int32_t)rd_u32le(&in[4]);
    s->yMm            = (int32_t)rd_u32le(&in[8]);
    s->thetaMdeg      = (int32_t)rd_u32le(&in[12]);
    s->odometerMm     = (int32_t)rd_u32le(&in[16]);
    s->goalDistanceMm = (int32_t)rd_u32le(&in[20]);
    s->goalTurnMdeg   = (int32_t)rd_u32le(&in[24]);
    s->goalDurationMs = rd_u32le(&in[28]);
    s->leftMmps       = (int16_t)rd_u16le(&in[32]);
    s->rightMmps      = (int16_t)rd_u16le(&in[34]);
    return 0;
}
