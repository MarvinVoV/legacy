#include "ws2812_encoder.h"

bool Ws2812_Timing(uint32_t hz, Ws2812Timing *t)
{
    if (!t || hz < 20000000u || hz > 600000000u) return false;
    const uint32_t period = (hz + 400000u) / 800000u;
    *t = (Ws2812Timing){
        .periodTicks = period,
        .zeroTicks = (uint32_t)(((uint64_t)hz * 380u + 500000000u) / 1000000000u),
        .oneTicks = (uint32_t)(((uint64_t)hz * 750u + 500000000u) / 1000000000u),
        /* Two extra slots tolerate stopping at the DMA completion boundary. */
        .resetSlots = (uint32_t)(((uint64_t)hz * 400u + (uint64_t)period * 1000000u - 1u) /
                                ((uint64_t)period * 1000000u)) + 2u,
    };
    return t->zeroTicks && t->zeroTicks < t->oneTicks && t->oneTicks < period &&
           24u * WS2812_LED_COUNT + t->resetSlots <= WS2812_BUFFER_WORDS;
}

size_t Ws2812_Encode(const LedRgb pixels[WS2812_LED_COUNT], const Ws2812Timing *t, uint32_t *out, size_t cap)
{
    if (!pixels || !t || !out || !t->periodTicks ||
        !t->zeroTicks || t->zeroTicks >= t->oneTicks || t->oneTicks >= t->periodTicks ||
        t->resetSlots < 2u || t->resetSlots > WS2812_BUFFER_WORDS) return 0;
    const size_t count = 24u * WS2812_LED_COUNT + t->resetSlots;
    if (cap < count) return 0;
    size_t offset = 0;
    for (unsigned i = 0; i < WS2812_LED_COUNT; ++i) {
        const uint8_t channels[3] = {pixels[i].g, pixels[i].r, pixels[i].b};
        for (unsigned c = 0; c < 3; ++c) {
            for (int bit = 7; bit >= 0; --bit)
                out[offset++] = (channels[c] & (1u << bit)) ? t->oneTicks : t->zeroTicks;
        }
    }
    while (offset < count) out[offset++] = 0;
    return offset;
}
