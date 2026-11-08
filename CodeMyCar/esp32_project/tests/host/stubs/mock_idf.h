#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_TIMEOUT 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_NO_MEM 4
#define CMCAR_ERR_QUEUE_FULL 5
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define CONFIG_CMCAR_UART_PORT 1
#define CONFIG_CMCAR_UART_BAUD_RATE 115200
#define CONFIG_CMCAR_UART_TX_PIN 1
#define CONFIG_CMCAR_UART_RX_PIN 2
#define CONFIG_CMCAR_UART_RX_BUF_SIZE 512
#define CONFIG_CMCAR_UART_TX_BUF_SIZE 512
#define CONFIG_CMCAR_UART_RINGBUF_SIZE 512
#define CONFIG_CMCAR_UART_TX_QUEUE_DEPTH 4
#define TASK_PRIO_UART_RX 1
#define TASK_PRIO_UART_TX 1
#define ESP_INTR_FLAG_IRAM 1
#define CMCAR_EVENTS 0
#define CMCAR_UART_ERROR 0
#define UART_DATA 1
#define UART_FIFO_OVF 2
#define UART_BUFFER_FULL 3
#define UART_PARITY_ERR 4
#define UART_FRAME_ERR 5
#define UART_DATA_8_BITS 8
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_1 1
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_PIN_NO_CHANGE -1
#define RINGBUF_TYPE_BYTEBUF 0
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(x) (x)
#define pdPASS 1
#define pdTRUE 1
typedef int BaseType_t;
typedef uint32_t StackType_t;
typedef struct { int unused; } StaticTask_t, StaticRingbuffer_t;
typedef struct { unsigned count, depth, size; uint8_t *storage; } StaticQueue_t;
typedef struct { bool held; } StaticSemaphore_t;
typedef StaticQueue_t* QueueHandle_t;
typedef StaticSemaphore_t* SemaphoreHandle_t;
typedef void *RingbufHandle_t, *TaskHandle_t;
typedef struct { int type; } uart_event_t;
typedef struct { int baud_rate, data_bits, parity, stop_bits, flow_ctrl; } uart_config_t;
QueueHandle_t xQueueCreateStatic(unsigned, unsigned, uint8_t*, StaticQueue_t*);
int xQueueReceive(QueueHandle_t, void*, uint32_t);
int xQueueSend(QueueHandle_t, const void*, uint32_t);
int xQueueSendToFront(QueueHandle_t, const void*, uint32_t);
int xQueueReset(QueueHandle_t);
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t*);
int xSemaphoreTake(SemaphoreHandle_t, uint32_t);
int xSemaphoreGive(SemaphoreHandle_t);
int uart_write_bytes(int, const void*, size_t);
int uart_read_bytes(int, void*, size_t, uint32_t);
int uart_flush_input(int);
int uart_param_config(int, const uart_config_t*);
int uart_set_pin(int,int,int,int,int);
int uart_driver_install(int,int,int,int,QueueHandle_t*,int);
int uart_driver_delete(int);
const char* esp_err_to_name(int);
int esp_event_post(int,int,const void*,size_t,uint32_t);
RingbufHandle_t xRingbufferCreateStatic(size_t,int,uint8_t*,StaticRingbuffer_t*);
int xRingbufferSend(RingbufHandle_t,const void*,size_t,uint32_t);
void* xRingbufferReceive(RingbufHandle_t,size_t*,uint32_t);
void vRingbufferReturnItem(RingbufHandle_t,void*);
void vRingbufferDelete(RingbufHandle_t);
TaskHandle_t xTaskCreateStatic(void (*)(void*),const char*,unsigned,void*,unsigned,StackType_t*,StaticTask_t*);
