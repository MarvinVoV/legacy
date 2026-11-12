#include "log_manager.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static osKernelState_t kernel;
static unsigned allocations, frees, localWrites, protocolWrites, queuePuts;
static log_entry_t *queued;
static int full;
void *pvPortMalloc(size_t n) { ++allocations;return malloc(n); }
void vPortFree(void *p) { ++frees;free(p); }
osKernelState_t osKernelGetState(void) { return kernel; }
uint32_t osKernelGetTickCount(void) { return 123; }
osMessageQueueId_t osMessageQueueNew(uint32_t n,uint32_t size,const void *attr)
{ (void)attr;assert(n==LOG_QUEUE_SIZE && size==sizeof(log_entry_t*));return (void*)1; }
osStatus_t osMessageQueuePut(osMessageQueueId_t q,const void *p,uint8_t priority,uint32_t timeout)
{
    assert(q && !priority && !timeout); /* Producers must never wait. */
    ++queuePuts;
    if (full) return osErrorResource;
    assert(!queued);queued=*(log_entry_t*const*)p;return osOK;
}
void QueueManager_registerHandler(int type,osMessageQueueId_t q) { assert(type && q); }
void LogTask_Init(void) {}
HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *uart,uint8_t *data,uint16_t size,uint32_t timeout)
{ (void)timeout;assert(uart->id==1 && size && data[0]);++localWrites;return HAL_OK; }
bool UART_Send_Protocol_Log(log_entry_t *entry)
{ assert(entry && !strcmp(entry->message,"test 42"));++protocolWrites;return true; }
int main(void)
{
    UART_HandleTypeDef local={1},remote={4};
    log_manager_config_t cfg={.channel_count=2,.channels={
        {.huart=&local,.level_mask=LOG_LEVEL_ALL,.module_mask=LOG_MODULE_ALL,.direct_output=true},
        {.huart=&remote,.level_mask=LOG_LEVEL_ALL,.module_mask=LOG_MODULE_ALL,.direct_output=false}}};
    assert(LogManager_Config(&cfg));
    log_record(LOG_LEVEL_INFO,LOG_MODULE_SYSTEM,"test %d",42);
    assert(localWrites==1 && !protocolWrites && !queuePuts && frees==1);
    kernel=osKernelRunning;
    log_record(LOG_LEVEL_INFO,LOG_MODULE_SYSTEM,"test %d",42);
    assert(queued && queuePuts==1 && localWrites==1 && !protocolWrites && frees==1);
    assert(queued->timestamp==123 && !strcmp(queued->message,"test 42"));
    LogManager_WriteOutputs(queued);
    assert(localWrites==2 && protocolWrites==1 && frees==1);
    vPortFree(queued);queued=NULL;
    full=1;
    log_record(LOG_LEVEL_INFO,LOG_MODULE_SYSTEM,"test %d",42);
    assert(!queued && frees==3 && LogManager_DroppedCount()==1);
    cfg.channels[0].level_mask=0;cfg.channels[1].module_mask=0;
    log_record(LOG_LEVEL_INFO,LOG_MODULE_SYSTEM,"filtered");
    assert(queuePuts==2 && allocations==frees);
    puts("logging: boot output, deferred dual output, single ownership, filtering and nonblocking full queue PASS");
}
