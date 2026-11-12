#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* Board-local wake acknowledgment ("我在").
 *
 * Played before the WebSocket session is opened so a wake still gets a
 * response when WiFi or the server is unavailable. The PCM is 16 kHz /
 * 16-bit / mono, generated from the same Baidu voice used for replies.
 */
/* Queue the local acknowledgment; *duration_ms receives its real length so
 * the caller can keep the microphone muted for the whole prompt. */
esp_err_t voice_wake_ack_play(uint32_t *duration_ms);
bool voice_wake_ack_available(void);
