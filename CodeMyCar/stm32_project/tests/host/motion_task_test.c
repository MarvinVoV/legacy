/* Runs production MotionTask and reporter against a deliberately stalled TX
 * consumer. Control must continue and coalesce states without calling UART. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "mock.h"
#include "../../Application/Tasks/MotionTask/Src/motion_task.c"
static jmp_buf escape;
static unsigned clockMs,cycles,uartCalls;
static bool consumer;
uint32_t HAL_GetTick(void){return clockMs;}
TickType_t xTaskGetTickCount(void){return clockMs;}
void vTaskDelayUntil(TickType_t* last,TickType_t interval)
{
    *last+=interval;clockMs=*last;
    if(cycles==8) longjmp(escape,1);
}
osThreadId_t osThreadNew(void (*entry)(void*),void* ctx,const osThreadAttr_t* attr)
{(void)entry;(void)ctx;assert(attr->stack_size>=2048);return (void*)1;}
QueueHandle_t xQueueCreateStatic(unsigned n,unsigned size,uint8_t* data,StaticQueue_t* q)
{assert(n==1);*q=(StaticQueue_t){.size=size,.data=data};return q;}
int xQueueOverwrite(QueueHandle_t q,const void* in)
{memcpy(q->data,in,q->size);q->full=true;return 1;}
int xQueueReceive(QueueHandle_t q,void* out,unsigned wait)
{
    assert(consumer && wait==portMAX_DELAY);
    if(!q->full)longjmp(escape,1);
    memcpy(out,q->data,q->size);q->full=false;return pdTRUE;
}
void MotionService_Run(MotionContext* ctx)
{(void)ctx;assert(!consumer && uartCalls==0);cycles++;}
void SafetySupervisor_Heartbeat(int id){assert(id==TASK_HEALTH_MOTION);}
bool MotionService_ConsumeUrgentStatus(MotionContext* ctx){(void)ctx;return true;}
bool MotionService_ShouldReportFast(MotionContext* ctx){(void)ctx;return true;}
void MotionService_GetStatus(MotionContext* ctx,motion_status_t* status)
{(void)ctx;*status=(motion_status_t){.seq=(uint8_t)cycles};}
bool UART_Send_Frame(int type,uint8_t seq,uint8_t flags,const uint8_t* data,uint16_t len)
{
    (void)flags;assert(consumer && type==7 && seq==8 && len==MOTION_STATUS_SIZE);
    motion_status_t status;
    assert(motion_status_unpack(data,len,&status)==0 && status.seq==8);
    uartCalls++;clockMs+=500;return false; /* delayed/failed TX */
}
int main(void)
{
    MotionContext ctx={0};MotionTask_init(&ctx);
    if(!setjmp(escape))MotionTask_doTask(&ctx);
    assert(cycles==8 && uartCalls==0 && s_motionMaxGapMs==5);
    consumer=true;
    if(!setjmp(escape))MotionStatusTask_doTask(NULL);
    assert(uartCalls==1 && s_motionTxFailures==1);
    puts("motion_task: stalled TX coalesces states; control never calls UART; reporter failure counted");
}
