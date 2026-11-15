#include "voice_visual_state.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

/* Independent of codec lifetime. Never hold this lock over I/O or queues. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static LightVoice s_base;
static int64_t s_audio_until;

void voice_visual_set_base(LightVoice phase)
{
    portENTER_CRITICAL(&s_lock);
    s_base = phase;
    portEXIT_CRITICAL(&s_lock);
}

void voice_visual_play_until(int64_t deadline_us)
{
    portENTER_CRITICAL(&s_lock);
    s_audio_until = deadline_us;
    portEXIT_CRITICAL(&s_lock);
}

LightVoice voice_visual_current(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    LightVoice phase = s_base;
    if (phase != LIGHT_VOICE_IDLE && now < s_audio_until) phase = LIGHT_VOICE_SPEAK;
    portEXIT_CRITICAL(&s_lock);
    return phase;
}
