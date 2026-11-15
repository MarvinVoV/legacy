#ifndef DISPLAY_FACE_H
#define DISPLAY_FACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pure renderer: no RTOS/HAL, no allocation. RGB565 pixels are in SPI byte order. */
#define FACE_WIDTH 240u
#define FACE_HEIGHT 240u
#define FACE_REGION_COUNT 3u

typedef struct { uint16_t x, y, w, h; } FaceRect;
typedef struct {
    uint8_t mood;
    bool manual_gaze, manual_open, manual_color;
    float gaze_x, gaze_y, openness;
    uint16_t color;
} FaceRequest;
typedef struct {
    uint32_t last_ms, next_look_ms, next_blink_ms, blink_start_ms, rng;
    bool blinking;
    float gaze_x, gaze_y, target_x, target_y, blink;
    float height_left, height_right, tilt, smile, red, green, blue;
} FaceModel;

void Face_Init(FaceModel *face, uint32_t now_ms);
void Face_Update(FaceModel *face, const FaceRequest *request, uint32_t now_ms);
/* First call requires a zeroed framebuffer; subsequent calls preserve it.
 * Each returned region encloses every changed pixel in that part of the face. */
size_t Face_Render(const FaceModel *face, uint16_t *framebuffer, FaceRect dirty[FACE_REGION_COUNT]);
#endif
