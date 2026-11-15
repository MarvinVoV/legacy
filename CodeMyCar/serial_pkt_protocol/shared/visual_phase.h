#pragma once
#include <stdint.h>

/* Wire values shared by the S3 gateway, STM32 lights and face renderer. */
typedef enum { LIGHT_CALM, LIGHT_HAPPY, LIGHT_CURIOUS, LIGHT_COMFORTING, LIGHT_SAD,
               LIGHT_EXCITED, LIGHT_MOOD_COUNT } LightMood;
typedef enum { LIGHT_VOICE_IDLE, LIGHT_VOICE_WAKE, LIGHT_VOICE_LISTEN, LIGHT_VOICE_THINK,
               LIGHT_VOICE_SPEAK, LIGHT_VOICE_COUNT } LightVoice;
enum { VISUAL_DEVICE_PHASE = 5, VISUAL_PHASE_LEASE_MS = 1200, VISUAL_PHASE_REFRESH_MS = 600 };

/* Standard v1 VISUAL payload. Device phases are independent of server epochs. */
static inline void visual_pack_phase(uint8_t out[20], LightVoice phase,
                                     uint32_t session, uint32_t event)
{
    const uint8_t header[8] = {1, VISUAL_DEVICE_PHASE, (uint8_t)phase, 100,
        (uint8_t)VISUAL_PHASE_LEASE_MS, VISUAL_PHASE_LEASE_MS >> 8, 0, 0};
    for (unsigned i=0; i<8; ++i) out[i]=header[i];
    for (unsigned i=0; i<4; ++i) {
        out[8+i]=(uint8_t)(session >> (8*i));
        out[12+i]=0;
        out[16+i]=(uint8_t)(event >> (8*i));
    }
}
