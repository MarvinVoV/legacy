#pragma once

#include "esp_err.h"

/** Start capture/frontend/session tasks. Failure disables voice only; caller
 * must keep the control bridge running. Serial 's' is a diagnostic bypass. */
esp_err_t voice_service_start(void);

/* Snapshot from this boot. String fields are immutable firmware literals. */
typedef struct {
    const char *startup;
    const char *state;
    const char *error_stage;
    esp_err_t error_code;
} voice_health_t;
voice_health_t voice_service_health(void);
