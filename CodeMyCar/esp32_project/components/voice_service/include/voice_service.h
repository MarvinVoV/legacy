#pragma once

#include "esp_err.h"

/** Start capture/frontend/session tasks. Failure disables voice only; caller
 * must keep the control bridge running. Serial 's' is a diagnostic bypass. */
esp_err_t voice_service_start(void);
