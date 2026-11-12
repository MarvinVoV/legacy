#ifndef CMCAR_LIGHTING_SERVICE_H
#define CMCAR_LIGHTING_SERVICE_H
#include "ws2812_encoder.h"
#include <stdbool.h>

#define LIGHTING_MAX_CHANNEL 12u
#define LIGHTING_COMMAND_SIZE 20u
#define LIGHTING_STATUS_SIZE 44u

typedef enum { LIGHT_CALM, LIGHT_HAPPY, LIGHT_CURIOUS, LIGHT_COMFORTING, LIGHT_SAD, LIGHT_MOOD_COUNT } LightMood;
typedef enum { LIGHT_VOICE_IDLE, LIGHT_VOICE_WAKE, LIGHT_VOICE_LISTEN, LIGHT_VOICE_THINK,
               LIGHT_VOICE_SPEAK, LIGHT_VOICE_COUNT } LightVoice;
typedef enum { LIGHT_EXPRESSION=1, LIGHT_VOICE=2, LIGHT_LEVEL=3, LIGHT_CLEAR=4 } LightHintKind;
typedef enum { LIGHT_IDLE, LIGHT_FORWARD, LIGHT_REVERSE, LIGHT_LEFT, LIGHT_RIGHT,
               LIGHT_BRAKE, LIGHT_FAULT, LIGHT_WARNING } LightMode;

typedef struct {
    uint32_t session, epoch, event;
    uint16_t ttlMs;
    uint8_t kind, value, intensity;
} LightCommand;
typedef struct {
    uint32_t receivedMs, event;
    uint16_t ttlMs;
    uint8_t value, intensity;
    bool seen;
} LightHint;
typedef struct {
    uint32_t session, epoch;
    bool epochKnown;
    LightHint mood, voice, level;
    uint16_t accepted, rejected;
} LightHints;
typedef struct {
    uint32_t sampledMs, batteryMs;
    int16_t speedMmps;
    int8_t steering; /* -1 left, +1 right: submitted PWM, not measured wheel angle. */
    uint8_t batteryPercent;
    bool motionValid, stopRequested, braking, fault, batteryValid;
} LightLocal;
typedef struct {
    uint32_t lastMs, phaseMs, brakeMs, batterySinceMs;
    uint16_t smooth[WS2812_LED_COUNT][3]; /* Q8 internal channel values */
    uint8_t baseKey;
    bool ready, wasMoving, wasBraking, brakeActive;
    bool batteryTimer, lowBattery, recoveringBattery;
} LightRenderer;
typedef struct {
    uint8_t mode, mood, voice, flags, intensity;
    uint16_t moodRemainingMs;
} LightView;

/* Pure C: no allocation, I/O, HAL, or RTOS calls. */
bool Lighting_UnpackCommand(const uint8_t *data, size_t size, LightCommand *out);
void Lighting_NewSession(LightHints *hints, uint32_t session);
bool Lighting_Accept(LightHints *hints, const LightCommand *command, uint32_t nowMs);
void Lighting_Render(LightRenderer *renderer, const LightHints *hints,
                     const LightLocal *local, uint32_t nowMs,
                     LedRgb out[WS2812_LED_COUNT], LightView *view);
void Lighting_PackStatus(const LightHints *hints, const LightView *view,
                         const LedRgb pixels[WS2812_LED_COUNT], uint32_t nowMs,
                         uint8_t out[LIGHTING_STATUS_SIZE]);
/* Explicit bench pattern only; normal startup uses Lighting_Render(). */
void Lighting_RenderBringup(uint32_t elapsedMs, LedRgb out[WS2812_LED_COUNT]);
#endif
