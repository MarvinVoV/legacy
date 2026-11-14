/*
 * display_eye.c
 *
 * 3D 风格大眼睛：整屏渲染到帧缓冲，再整帧推送到 GC9A01。
 *
 * - 帧缓冲放在 RAM_D1（.fb_buffer），不占 DTCM 栈空间
 * - 上眼睑主导的眨眼（带缓动），下眼睑轻微上抬
 * - 上眼睑边缘绘制睫毛
 * - 金黄色虹膜：径向渐变 + 边缘暗环 + 双高光
 */

#include "display_eye.h"

#include "cmsis_os2.h"
#include "display.h"
#include "display_config.h"
#include "log_manager.h"

#include <math.h>

#define EYE_W DISPLAY_WIDTH
#define EYE_H DISPLAY_HEIGHT

/* 默认虹膜：金黄色 (230,180,60) */
#define EYE_IRIS_GOLD 0xE5A7u

typedef struct
{
    float    gaze_x;   /* -1..1，右为正 */
    float    gaze_y;   /* -1..1，下为正 */
    float    close;    /* 0=睁开 1=闭合 */
    uint16_t iris;     /* RGB565 */
} eye_params_t;

/* 情绪 → 眼睛参数。下标与 LightMood 一致：
 * 0 calm / 1 happy / 2 curious / 3 comforting / 4 sad */
typedef struct
{
    float    lid_drop;    /* 上眼睑额外下压像素 */
    float    lid_curve;   /* 眼睑弧度系数（两侧下垂量） */
    float    pupil_scale; /* 瞳孔缩放 */
    float    iris_bright; /* 虹膜亮度系数 */
    uint16_t iris;        /* 虹膜颜色 */
    uint32_t blink_ms;    /* 眨眼周期 */
    float    gaze_y_bias; /* 视线纵向偏好（正=向下） */
} eye_mood_t;

static const eye_mood_t k_moods[5] = {
    /* calm      */ {  0.0f, 20.0f, 1.00f, 1.00f, 0xE5A7u, 5000u,  0.00f },
    /* happy     */ { 78.0f, 46.0f, 1.00f, 1.18f, 0xF6C6u, 3000u, -0.08f },
    /* curious   */ { -14.0f, 10.0f, 1.30f, 1.10f, 0xE5A7u, 4000u, -0.12f },
    /* comforting*/ { 46.0f, 24.0f, 1.00f, 0.92f, 0xD89Au, 7000u,  0.06f },
    /* sad       */ { 62.0f,  6.0f, 0.85f, 0.80f, 0x4A86u, 8000u,  0.30f },
};

static uint8_t s_mood; /* 当前情绪 */

static inline const eye_mood_t *cur_mood(void)
{
    return &k_moods[s_mood < 5u ? s_mood : 0u];
}

static uint16_t __attribute__((section(".fb_buffer"))) s_fb[EYE_W * EYE_H];

static eye_params_t s_eye = {0.0f, 0.0f, 0.0f, EYE_IRIS_GOLD};
static volatile bool s_running;

/* 视线扫视状态 */
static uint32_t s_rng = 0x12345678u;
static uint32_t s_next_saccade;
static float    s_target_x;
static float    s_target_y;

/* ==================== 基础工具 ==================== */

static inline void fb_put(int x, int y, uint16_t color)
{
    if ((unsigned)x >= EYE_W || (unsigned)y >= EYE_H) { return; }
    /* 帧缓冲按 SPI 字节序存放（高字节在前） */
    s_fb[(uint32_t)y * EYE_W + (uint32_t)x] =
        (uint16_t)((color >> 8) | (color << 8));
}

static inline float clampf(float v, float lo, float hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static inline float smoothstep(float e0, float e1, float x)
{
    const float t = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static inline uint16_t rgb565(float r, float g, float b)
{
    r = clampf(r, 0.0f, 255.0f);
    g = clampf(g, 0.0f, 255.0f);
    b = clampf(b, 0.0f, 255.0f);
    return (uint16_t)((((uint16_t)r & 0xF8u) << 8) |
                      (((uint16_t)g & 0xFCu) << 3) |
                      (((uint16_t)b & 0xF8u) >> 3));
}

/* 眼睑边缘：中间高、两侧低的弧形（ax = dx/eye_r） */
static inline float upper_edge_at(float ax)
{
    const eye_mood_t *m = cur_mood();
    return (-119.0f + 230.0f * s_eye.close + m->lid_drop) + m->lid_curve * ax * ax;
}

static inline float lower_edge_at(float ax)
{
    return (119.0f - 44.0f * s_eye.close) - 12.0f * ax * ax;
}

/* ==================== 睫毛 ==================== */

static void draw_lashes(void)
{
    if (s_eye.close > 0.75f) { return; }

    const float cx = EYE_W * 0.5f;
    const float cy = EYE_H * 0.5f;

    for (int i = 0; i < 13; ++i)
    {
        const float ax = -0.82f + (float)i * (1.64f / 12.0f);
        const float bx = cx + ax * 119.0f;
        const float by = cy + upper_edge_at(ax) + 2.0f;
        const float len = 13.0f + 5.0f * sinf((float)i * 1.7f);
        const float slant = 5.0f * sinf((float)i * 0.9f);

        for (float t = 0.0f; t <= 1.0f; t += 0.06f)
        {
            const int px = (int)(bx + slant * t);
            const int py = (int)(by + len * t);
            fb_put(px, py, 0x0000u);
            fb_put(px + 1, py, 0x0000u);
        }
    }
}

/* ==================== 渲染 ==================== */

static void eye_render(void)
{
    const float cx = EYE_W * 0.5f;
    const float cy = EYE_H * 0.5f;
    const float eye_r  = 119.0f;
    const float iris_r = 74.0f;
    const float pupil_r = 30.0f * cur_mood()->pupil_scale;
    const float bright = cur_mood()->iris_bright;

    const float icx = cx + s_eye.gaze_x * 26.0f;
    const float icy = cy + s_eye.gaze_y * 26.0f;

    /* 虹膜颜色拆成 8 位分量 */
    const uint16_t iris_color = cur_mood()->iris;
    const float ir = (float)((iris_color >> 11) & 0x1Fu) * (255.0f / 31.0f) * bright;
    const float ig = (float)((iris_color >> 5) & 0x3Fu) * (255.0f / 63.0f) * bright;
    const float ib = (float)(iris_color & 0x1Fu) * (255.0f / 31.0f) * bright;

    const float eye_r2 = eye_r * eye_r;

    for (int y = 0; y < EYE_H; ++y)
    {
        for (int x = 0; x < EYE_W; ++x)
        {
            const float dx = (float)x - cx;
            const float dy = (float)y - cy;
            const float r2 = dx * dx + dy * dy;

            if (r2 > eye_r2) /* 圆屏之外 */
            {
                fb_put(x, y, 0x0000u);
                continue;
            }

            const float ax = dx / eye_r;

            /* ---- 眼睑：上睑主导，下睑轻微上抬 ---- */
            if (dy < upper_edge_at(ax) || dy > lower_edge_at(ax))
            {
                /* 眼睑用肤色，靠上方带一点眉影，闭眼时像"眼皮"而不是黑屏 */
                const float t = clampf((-dy - 40.0f) / 79.0f, 0.0f, 1.0f);
                float lr = 232.0f - 55.0f * t;
                float lg = 196.0f - 62.0f * t;
                float lb = 178.0f - 64.0f * t;
                /* 下眼睑更暗一些 */
                if (dy > 0.0f) { lr -= 18.0f; lg -= 20.0f; lb -= 20.0f; }
                fb_put(x, y, rgb565(lr, lg, lb));
                continue;
            }

            /* ---- 虹膜 / 瞳孔 / 巩膜 ---- */
            const float ix = (float)x - icx;
            const float iy = (float)y - icy;
            const float d = sqrtf(ix * ix + iy * iy);
            float R, G, B;

            if (d < pupil_r)
            {
                const float t = d / pupil_r;
                R = 6.0f + 10.0f * t;
                G = 5.0f + 8.0f * t;
                B = 8.0f + 10.0f * t;
            }
            else if (d < iris_r)
            {
                const float t = (d - pupil_r) / (iris_r - pupil_r);
                float k = 1.10f - 0.60f * t;
                /* 廉价放射纹理，避免逐像素 atan2f/sinf */
                k *= 0.95f + 0.05f * (ix * iy) / (iris_r * iris_r);
                const float limbal = smoothstep(0.82f, 1.0f, t);
                k *= 1.0f - 0.65f * limbal;
                R = ir * k;
                G = ig * k;
                B = ib * k;
            }
            else
            {
                const float t = clampf((d - iris_r) / (eye_r - iris_r), 0.0f, 1.0f);
                const float sphere = 1.0f - 0.38f * t * t;
                const float top = (dy < 0.0f)
                    ? (1.0f - 0.22f * (-dy / eye_r)) : 1.0f;
                R = 236.0f * sphere * top;
                G = 239.0f * sphere * top;
                B = 246.0f * sphere * top;
                const float fade = smoothstep(iris_r, iris_r + 3.0f, d);
                R = R * fade + ir * 0.25f * (1.0f - fade);
                G = G * fade + ig * 0.25f * (1.0f - fade);
                B = B * fade + ib * 0.25f * (1.0f - fade);
            }

            /* ---- 高光 ---- */
            if (d < iris_r + 8.0f)
            {
                const float hx = (float)x - (icx - 26.0f);
                const float hy = (float)y - (icy - 28.0f);
                const float h1 = smoothstep(20.0f, 10.0f, sqrtf(hx * hx + hy * hy));
                const float hx2 = (float)x - (icx + 20.0f);
                const float hy2 = (float)y - (icy + 22.0f);
                const float h2 = smoothstep(9.0f, 4.0f, sqrtf(hx2 * hx2 + hy2 * hy2));
                const float h = clampf(h1 + h2 * 0.7f, 0.0f, 1.0f);
                R += (255.0f - R) * h;
                G += (255.0f - G) * h;
                B += (255.0f - B) * h;
            }

            fb_put(x, y, rgb565(R, G, B));
        }
    }

    draw_lashes();

    /* 闭合时的睫毛线：上下眼睑交汇处画一条深色弧线 */
    if (s_eye.close > 0.55f)
    {
        const float cx2 = EYE_W * 0.5f;
        const float cy2 = EYE_H * 0.5f;
        for (int x = 0; x < EYE_W; ++x)
        {
            const float ax = ((float)x - cx2) / 119.0f;
            if (fabsf(ax) > 0.95f) { continue; }
            const float yl = cy2 + upper_edge_at(ax) + 1.0f;
            for (int k = 0; k < 3; ++k) { fb_put(x, (int)yl + k, 0x0861u); }
        }
    }
}

/* ==================== 动画任务 ==================== */

static void eye_task(void *arg)
{
    (void)arg;

    uint32_t frames = 0u;

    for (;;)
    {
        const uint32_t now = (uint32_t)osKernelGetTickCount();
        const float ts = (float)now * 0.001f;

        /* 视线：偶发扫视 + 平滑跟随，比单纯正弦更有"活着"的感觉 */
        s_rng = s_rng * 1103515245u + 12345u;
        if (now > s_next_saccade)
        {
            const float rx = (float)((s_rng >> 16) % 1000u) * 0.001f;
            const float ry = (float)((s_rng >> 8) % 1000u) * 0.001f;
            s_target_x = (rx * 2.0f - 1.0f) * 0.55f;
            s_target_y = (ry * 2.0f - 1.0f) * 0.28f;
            s_next_saccade = now + 800u + ((s_rng >> 4) % 1800u);
        }
        s_eye.gaze_x += (s_target_x - s_eye.gaze_x) * 0.14f;
        s_eye.gaze_y += (s_target_y + cur_mood()->gaze_y_bias - s_eye.gaze_y) * 0.14f;
        (void)ts;

        /* 每 5 秒眨眼一次，持续 900ms，闭合/睁开各带缓动 */
        const uint32_t blink_period = cur_mood()->blink_ms;
        const uint32_t blink_len = blink_period / 5u;
        const uint32_t phase = now % blink_period;
        if (phase > blink_period - blink_len)
        {
            const float t = (float)(phase - (blink_period - blink_len)) / (float)blink_len;
            const float tri = (t < 0.5f) ? (t * 2.0f) : (2.0f - t * 2.0f);
            s_eye.close = smoothstep(0.0f, 1.0f, tri);
        }
        else
        {
            s_eye.close = 0.0f;
        }

        if (!s_running) { break; }

        eye_render();
        Display_BlitBuffer(s_fb, 0u, 0u, EYE_W, EYE_H);
        if ((++frames % 20u) == 0u)
        {
            LOG_INFO(LOG_MODULE_SYSTEM, "DISP frames=%lu mood=%u close=%d",
                     (unsigned long)frames, (unsigned)s_mood,
                     (int)(s_eye.close * 100.0f));
        }

        osDelay(10u);
    }

    osThreadExit();
}

/* ==================== 对外接口 ==================== */

void DisplayEye_Start(void)
{
    s_running = true;
    const osThreadAttr_t attrs = {
        .name = "DisplayEye",
        .stack_size = 3072,
        .priority = osPriorityLow,
    };
    if (osThreadNew(eye_task, NULL, &attrs) == NULL)
    {
        LOG_ERROR(LOG_MODULE_SYSTEM, "Failed to create DisplayEye task");
    }
}

void DisplayEye_Stop(void)
{
    s_running = false;
}

void DisplayEye_SetGaze(float x, float y)
{
    s_eye.gaze_x = clampf(x, -1.0f, 1.0f);
    s_eye.gaze_y = clampf(y, -1.0f, 1.0f);
}

void DisplayEye_SetOpenness(float openness)
{
    s_eye.close = clampf(1.0f - openness, 0.0f, 1.0f);
}

void DisplayEye_SetMood(uint8_t mood)
{
    const uint8_t m = (mood < 5u) ? mood : 0u;
    if (m != s_mood)
    {
        LOG_INFO(LOG_MODULE_SYSTEM, "DISP mood=%u", (unsigned)m);
        s_mood = m;
    }
}

void DisplayEye_SetIrisColor(uint16_t color)
{
    s_eye.iris = color;
}
