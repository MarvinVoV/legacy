/* Execute the real worker with a virtual clock and injected panel failures. */
#include "display.h"
#include "display_eye.h"
#include "cmsis_os2.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>

static jmp_buf done;
static void (*worker)(void *);
static unsigned now, init_count, transfers, long_sleeps;
static uint32_t irq_mask;

uint32_t __get_PRIMASK(void) { return irq_mask; }
void __disable_irq(void) { irq_mask = 1; }
void __set_PRIMASK(uint32_t key) { irq_mask = key; }
uint32_t HAL_GetTick(void) { return now; }
uint32_t osKernelGetTickFreq(void) { return 1000; }
int32_t osKernelLock(void) { return 0; }
int32_t osKernelRestoreLock(int32_t lock) { return lock; }
osThreadId_t osThreadNew(void (*task)(void *), void *arg, const osThreadAttr_t *attrs) {
    assert(!arg && attrs->cb_mem && attrs->stack_mem && attrs->stack_size == 3072);
    assert(attrs->priority == osPriorityLow && !worker);
    worker = task;
    return attrs->cb_mem;
}
void osDelay(uint32_t ticks) {
    assert(irq_mask == 0 && ticks > 0);
    now += ticks;
    if (ticks >= 2000) ++long_sleeps;
    if (transfers >= 2 && ticks <= 50) longjmp(done, 1);
    assert(now <= 10000);
}
bool Display_Init(void) {
    assert(irq_mask == 0 && now >= init_count * 2000);
    return ++init_count > 1;
}
void Display_Fill(uint16_t color) { (void)color; }
bool Display_BlitBuffer(const uint16_t *buf,uint16_t x,uint16_t y,uint16_t w,uint16_t h) {
    assert(irq_mask == 0 && buf && x==0 && y==0 && w==240 && h==240);
    return ++transfers > 1;
}
int main(void) {
    DisplayEye_Start();
    DisplayEye_Start();
    assert(worker && !init_count && !transfers && !now);
    if (!setjmp(done)) worker(NULL);
    assert(init_count == 3 && long_sleeps == 2 && transfers == 2);
    puts("display worker: boot isolation, init/transfer faults, backoff and full-frame recovery passed");
}
