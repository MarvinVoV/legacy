#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_TIMEOUT 4
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
#define CONFIG_IDF_TARGET_ESP32S3 1
#define TASK_PRIO_VOICE_OUTPUT 3
#define TASK_PRIO_VOICE_NETWORK 4
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#ifndef HOST_TICK_MS
#define HOST_TICK_MS 10
#endif
#define pdMS_TO_TICKS(ms) ((ms) / HOST_TICK_MS)
typedef unsigned TickType_t;
typedef struct {
 pthread_mutex_t mutex; pthread_cond_t readable;
 uint8_t *storage; size_t head, count, capacity, item_size;
} StaticQueue_t;
typedef StaticQueue_t *QueueHandle_t;
typedef pthread_mutex_t *SemaphoreHandle_t;
typedef pthread_t *TaskHandle_t;
void *heap_caps_malloc(size_t,unsigned);
void heap_caps_free(void *);
int64_t esp_timer_get_time(void);
const char *esp_err_to_name(esp_err_t);
#define ESP_LOGI(tag,...) ((void)(tag))
#define ESP_LOGW(tag,...) ((void)(tag))
SemaphoreHandle_t xSemaphoreCreateMutex(void);
void xSemaphoreTake(SemaphoreHandle_t,unsigned);
void xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
QueueHandle_t xQueueCreateStatic(unsigned,unsigned,uint8_t*,StaticQueue_t*);
int xQueueReceive(QueueHandle_t,void*,unsigned);
int xQueueSend(QueueHandle_t,const void*,unsigned);
void xQueueReset(QueueHandle_t);
void vQueueDelete(QueueHandle_t);
int xTaskCreate(void (*)(void*),const char*,unsigned,void*,unsigned,TaskHandle_t*);
void vTaskDelay(unsigned);
void vTaskDelete(TaskHandle_t);
#define pdFALSE 0
typedef unsigned EventBits_t;
typedef atomic_uint *EventGroupHandle_t;
EventGroupHandle_t xEventGroupCreate(void);
unsigned xEventGroupGetBits(EventGroupHandle_t);
unsigned xEventGroupSetBits(EventGroupHandle_t,unsigned);
unsigned xEventGroupClearBits(EventGroupHandle_t,unsigned);
unsigned xEventGroupWaitBits(EventGroupHandle_t,unsigned,int,int,unsigned);
void vEventGroupDelete(EventGroupHandle_t);
