#ifndef CMCAR_WS2812_ENCODER_H
#define CMCAR_WS2812_ENCODER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WS2812_LED_COUNT 6u
#define WS2812_BUFFER_WORDS 480u
typedef struct { uint8_t r, g, b; } LedRgb;
typedef struct {
    uint32_t periodTicks, zeroTicks, oneTicks, resetSlots;
} Ws2812Timing;

bool Ws2812_Timing(uint32_t timerHz, Ws2812Timing *out);
/* GRB, MSB first; final zero-duty slots keep DIN low for >=400us. */
size_t Ws2812_Encode(const LedRgb pixels[WS2812_LED_COUNT],
                     const Ws2812Timing *timing, uint32_t *out, size_t capacity);
#endif
