#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* One task owns inference; capture and control never call the model. */
bool voice_wake_init(void);
bool voice_wake_feed(const int16_t *pcm, size_t samples, uint32_t sequence);
void voice_wake_reset(void);
void voice_wake_destroy(void);
