/* Low-priority presentation adapter. Vehicle control never waits for this task. */
#include "display_eye.h"
#include "display_face.h"
#include "display.h"
#include "display_config.h"
#include "cmsis_os2.h"
#include "stm32h7xx_hal.h"
#include "log_manager.h"
#include <math.h>
#include <string.h>

#if DISPLAY_ENABLED
static uint16_t __attribute__((section(".fb_buffer"), aligned(32))) s_fb[FACE_WIDTH*FACE_HEIGHT];
static FaceRequest s_request;
static volatile bool s_running;
static osThreadId_t s_thread;

/* Only a few scalar request fields cross task boundaries. No SPI or rendering
 * inside this short critical section. The render model belongs to DisplayEye. */
static uint32_t request_lock(void) { uint32_t key=__get_PRIMASK(); __disable_irq(); return key; }
static void request_unlock(uint32_t key) { __set_PRIMASK(key); }
static float bounded(float v,float lo,float hi) { return isfinite(v) ? fminf(hi,fmaxf(lo,v)) : 0; }

static void eye_task(void *arg)
{
    (void)arg;
    FaceModel face;
    Face_Init(&face,HAL_GetTick());
    memset(s_fb,0,sizeof(s_fb));
    Display_Fill(DISPLAY_COLOR_BLACK);
    uint32_t frames=0,failed=0,render_max=0,transfer_max=0,bytes=0,last_log=HAL_GetTick();
    bool visible=true,full_refresh=true;
    for (;;) {
        if (!s_running) {
            if (visible) { Display_Fill(DISPLAY_COLOR_BLACK); visible=false; }
            osDelay(20u);
            full_refresh=true;
            continue;
        }
        visible=true;
        const uint32_t begin=HAL_GetTick();
        uint32_t key=request_lock();
        FaceRequest request=s_request;
        request_unlock(key);
        Face_Update(&face,&request,begin);
        FaceRect dirty[FACE_REGION_COUNT];
        const size_t count=Face_Render(&face,s_fb,dirty);
        uint32_t render_ms=HAL_GetTick()-begin;
        if (render_ms>render_max) render_max=render_ms;
        const uint32_t transfer_begin=HAL_GetTick();
        /* Failed partial writes are repaired by a complete frame on the next
         * iteration; the model/framebuffer may already have advanced. */
        if (full_refresh) {
            full_refresh=!Display_BlitBuffer(s_fb,0,0,FACE_WIDTH,FACE_HEIGHT);
            bytes+=FACE_WIDTH*FACE_HEIGHT*2u;
            if (full_refresh) failed++;
        } else {
            for (size_t i=0;i<count;i++) {
                FaceRect r=dirty[i];
                if (!Display_BlitBuffer(s_fb,r.x,r.y,r.w,r.h)) { full_refresh=true;failed++;break; }
                bytes+=(uint32_t)r.w*r.h*2u;
            }
        }
        uint32_t transfer_ms=HAL_GetTick()-transfer_begin;
        if (transfer_ms>transfer_max) transfer_max=transfer_ms;
        frames++;
        uint32_t elapsed=HAL_GetTick()-last_log;
        if (elapsed>=5000u) {
            LOG_INFO(LOG_MODULE_SYSTEM,"DISP fps10=%lu render_max=%lu spi_max=%lu bytes_s=%lu errors=%lu mood=%u",
                (unsigned long)(frames*10000u/elapsed),(unsigned long)render_max,
                (unsigned long)transfer_max,(unsigned long)((uint64_t)bytes*1000u/elapsed),
                (unsigned long)failed,request.mood);
            last_log=HAL_GetTick();frames=0;bytes=0;render_max=0;transfer_max=0;
        }
        /* Always yield, even if a full recovery frame exceeded the 50ms budget. */
        uint32_t spent=HAL_GetTick()-begin;
        uint32_t delay_ms=spent<50u ? 50u-spent : 1u;
        uint32_t ticks=(delay_ms*osKernelGetTickFreq()+999u)/1000u;
        osDelay(ticks ? ticks : 1u);
    }
}
#endif

void DisplayEye_Start(void)
{
#if DISPLAY_ENABLED
    /* Public start/stop are task-context APIs. One persistent task avoids the
     * old stop/start race creating two concurrent SPI writers. */
    int32_t lock=osKernelLock();
    s_running=true;
    if (!s_thread) {
        const osThreadAttr_t attrs={.name="DisplayEye",.stack_size=3072,.priority=osPriorityLow};
        s_thread=osThreadNew(eye_task,NULL,&attrs);
        if (!s_thread) { s_running=false; LOG_ERROR(LOG_MODULE_SYSTEM,"Failed to create DisplayEye task"); }
    }
    if (lock>=0) osKernelRestoreLock(lock);
#endif
}
void DisplayEye_Stop(void) {
#if DISPLAY_ENABLED
    s_running=false; /* clears after the current bounded transfer */
#endif
}
void DisplayEye_SetGaze(float x,float y) {
#if DISPLAY_ENABLED
    uint32_t key=request_lock();
    s_request.manual_gaze=true;s_request.gaze_x=bounded(x,-1,1);s_request.gaze_y=bounded(y,-1,1);
    request_unlock(key);
#else
    (void)x;(void)y;
#endif
}
void DisplayEye_SetOpenness(float openness) {
#if DISPLAY_ENABLED
    uint32_t key=request_lock();s_request.manual_open=true;s_request.openness=bounded(openness,0,1);request_unlock(key);
#else
    (void)openness;
#endif
}
void DisplayEye_SetMood(uint8_t mood) {
#if DISPLAY_ENABLED
    uint32_t key=request_lock();s_request.mood=mood<5u ? mood : 0u;request_unlock(key);
#else
    (void)mood;
#endif
}
void DisplayEye_SetIrisColor(uint16_t color) {
#if DISPLAY_ENABLED
    uint32_t key=request_lock();s_request.manual_color=true;s_request.color=color;request_unlock(key);
#else
    (void)color;
#endif
}
void DisplayEye_ResetOverrides(void) {
#if DISPLAY_ENABLED
    uint32_t key=request_lock();
    s_request.manual_gaze=false;s_request.manual_open=false;s_request.manual_color=false;
    request_unlock(key);
#endif
}
