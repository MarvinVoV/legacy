#include "lighting_service.h"
#include <string.h>

void Lighting_RenderBringup(uint32_t ms, LedRgb out[WS2812_LED_COUNT])
{
    memset(out, 0, sizeof(LedRgb) * WS2812_LED_COUNT);
    if (ms < 300u) return;
    LedRgb color = {0};
    if (ms < 1300u) color.r = 12;
    else if (ms < 2300u) color.g = 12;
    else if (ms < 3300u) color.b = 12;
    else if (ms < 5700u) {
        out[(ms-3300u)/400u] = (LedRgb){.r=8,.g=8,.b=8};
        return;
    } else {
        const uint32_t phase = (ms-5700u) % 4000u;
        const uint32_t triangle = phase < 2000u ? phase : 4000u-phase;
        color.g = (uint8_t)(2u + triangle*10u/2000u);
        color.b = (uint8_t)(1u + triangle*6u/2000u);
    }
    for (unsigned i=0; i<WS2812_LED_COUNT; ++i) out[i]=color;
}
