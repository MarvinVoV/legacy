#ifndef CMCAR_LIGHTING_SERVICE_H
#define CMCAR_LIGHTING_SERVICE_H
#include "ws2812_encoder.h"
/* Bring-up only: low-brightness RGB/one-pixel test, then idle breathing.
 * Vehicle/voice/emotion arbitration is the documented next integration phase. */
void Lighting_RenderBringup(uint32_t elapsedMs, LedRgb out[WS2812_LED_COUNT]);
#endif
