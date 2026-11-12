#ifndef CMCAR_LED_STRIP_H
#define CMCAR_LED_STRIP_H
#include "ws2812_encoder.h"
bool LedStrip_Init(void);
bool LedStrip_Show(const LedRgb pixels[WS2812_LED_COUNT]);
void LedStrip_DmaIRQHandler(void);
uint32_t LedStrip_CompletedFrames(void);
uint32_t LedStrip_ErrorCount(void);
#endif
