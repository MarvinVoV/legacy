/* configUSE_NEWLIB_REENTRANT supplies per-task errno/stdio state; it does NOT
 * serialize newlib's shared malloc arena. The bundled nano default lock is a
 * no-op. Suspend scheduling (nestable), keeping interrupts enabled. Never call
 * libc allocation/printf from an ISR. FreeRTOS heap_4 has its own protection. */
#include "FreeRTOS.h"
#include "task.h"
#include "stm32h7xx.h"
#include <reent.h>

void __malloc_lock(struct _reent *reent)
{
    (void)reent;
    configASSERT(__get_IPSR() == 0u);
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) vTaskSuspendAll();
}

void __malloc_unlock(struct _reent *reent)
{
    (void)reent;
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) (void)xTaskResumeAll();
}
