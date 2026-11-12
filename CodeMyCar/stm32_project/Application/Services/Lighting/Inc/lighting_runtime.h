#pragma once
#include "lighting_service.h"

/* UART task producer / LightTask reader, fixed storage and short critical copies. */
void LightingRuntime_NewSession(uint32_t session);
bool LightingRuntime_Command(const uint8_t *payload,size_t length,uint32_t nowMs);
void LightingRuntime_GetHints(LightHints *out);
