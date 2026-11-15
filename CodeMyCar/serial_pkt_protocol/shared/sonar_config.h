#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Both directions use flags=0. A legacy board must never emit a motion ACK. */
#define SONAR_REQUEST_TYPE 0x0cu
#define SONAR_STATUS_TYPE  0x0du
#define SONAR_SET  1u
#define SONAR_BIND 2u
#define SONAR_SET_SIZE 18u
#define SONAR_BIND_SIZE 5u
#define SONAR_STATUS_SIZE 20u
#define SONAR_MAX_AGE_MS 3000u

enum { SONAR_OK, SONAR_BUSY, SONAR_UNSUPPORTED, SONAR_CONFLICT, SONAR_EXPIRED };
enum { SONAR_SUPPORTED = 1u, SONAR_ENABLED = 2u, SONAR_READY = 4u };

typedef struct {
    uint32_t epoch, generation, request_id, observed_ms;
    uint8_t op, enable;
} SonarRequest;

typedef struct {
    uint32_t epoch, generation, request_id;
    uint8_t result;
    bool enabled;
} SonarConfig;

static inline uint32_t sonar_read32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static inline void sonar_write32(uint8_t *p, uint32_t n)
{
    p[0]=(uint8_t)n; p[1]=(uint8_t)(n>>8); p[2]=(uint8_t)(n>>16); p[3]=(uint8_t)(n>>24);
}
static inline bool sonar_decode_request(const uint8_t *p, size_t n, SonarRequest *out)
{
    if (!p || !out || !n) return false;
    if (p[0]==SONAR_BIND && n==SONAR_BIND_SIZE) {
        *out=(SonarRequest){.op=SONAR_BIND,.epoch=sonar_read32(p+1)};
        return out->epoch!=0;
    }
    if (n!=SONAR_SET_SIZE || p[0]!=SONAR_SET || p[1]>1u) return false;
    *out=(SonarRequest){.op=SONAR_SET,.enable=p[1],.epoch=sonar_read32(p+2),
        .generation=sonar_read32(p+6),.request_id=sonar_read32(p+10),.observed_ms=sonar_read32(p+14)};
    return out->epoch && out->request_id;
}

/* Single writer on STM32. Return true only for a real hardware transition. */
static inline bool sonar_apply(SonarConfig *s, const SonarRequest *r, uint32_t now,
                               bool supported, bool parked)
{
    if (r->op==SONAR_BIND) {
        if (!s->epoch) s->epoch=r->epoch;
        return false;
    }
    if (!s->epoch || s->epoch!=r->epoch || s->request_id==r->request_id) return false;
    s->request_id=r->request_id;
    if (!supported) s->result=SONAR_UNSUPPORTED;
    else if (r->generation!=s->generation) s->result=SONAR_CONFLICT;
    else if (now-r->observed_ms>SONAR_MAX_AGE_MS) s->result=SONAR_EXPIRED;
    else if (!r->enable && s->enabled && !parked) s->result=SONAR_BUSY;
    else {
        const bool changed=s->enabled!=(bool)r->enable;
        s->result=SONAR_OK;
        s->generation++;
        s->enabled=r->enable;
        return changed;
    }
    return false;
}
