/**
 * @file uart_service.c
 * @brief UART 硬件服务实现
 *
 * 架构：
 *   uart_rx_task: UART ISR → event queue → read → RingBuf → pkt_parser → rx_cb
 *   uart_tx_task: tx_queue → uart_write_bytes
 *
 * 所有资源静态分配，热路径零 malloc。
 */

#include "uart_service.h"

#include <string.h>

#include "cmcar_errors.h"
#include "cmcar_events.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "task_priorities.h"

static const char* TAG = "uart_svc";

#define UART_PORT      CONFIG_CMCAR_UART_PORT
#define UART_BAUD      CONFIG_CMCAR_UART_BAUD_RATE
#define UART_TX_PIN    CONFIG_CMCAR_UART_TX_PIN
#define UART_RX_PIN    CONFIG_CMCAR_UART_RX_PIN
#define RX_BUF_SIZE    CONFIG_CMCAR_UART_RX_BUF_SIZE
#define TX_BUF_SIZE    CONFIG_CMCAR_UART_TX_BUF_SIZE
#define RINGBUF_SIZE   CONFIG_CMCAR_UART_RINGBUF_SIZE
#define TX_QUEUE_DEPTH CONFIG_CMCAR_UART_TX_QUEUE_DEPTH

#define UART_EVENT_QUEUE_SIZE 10
#define UART_RX_TASK_STACK    3072
#define UART_TX_TASK_STACK    2048
#define UART_TMP_BUF_SIZE     256

/* ==================== 静态资源 ==================== */

/* 任务 */
static StackType_t s_rx_stack[UART_RX_TASK_STACK / sizeof(StackType_t)];
static StaticTask_t s_rx_tcb;
static StackType_t s_tx_stack[UART_TX_TASK_STACK / sizeof(StackType_t)];
static StaticTask_t s_tx_tcb;

/* RingBuf（原始 RX 字节） */
static uint8_t s_ringbuf_storage[RINGBUF_SIZE];
static StaticRingbuffer_t s_ringbuf_struct;
static RingbufHandle_t s_ringbuf = NULL;

/* UART 事件队列 */
static QueueHandle_t s_uart_event_queue = NULL;

/* TX 队列（编码后的帧） */
typedef struct {
    uint8_t data[PKT_MAX_FRAME_SIZE];
    uint8_t len;
} uart_tx_msg_t;

static uart_tx_msg_t s_tx_queue_storage[TX_QUEUE_DEPTH];
static StaticQueue_t s_tx_queue_struct;
static QueueHandle_t s_tx_queue = NULL;

/* 协议解析器 */
static pkt_parser_t s_parser;

/* 帧接收回调 */
static uart_frame_rx_cb_t s_rx_cb = NULL;
static void* s_rx_cb_ctx = NULL;

/* ==================== 任务实现 ==================== */

static void uart_rx_task(void* arg)
{
    static uint8_t tmp_buf[UART_TMP_BUF_SIZE];
    uart_event_t event;

    while (1)
    {
        if (xQueueReceive(s_uart_event_queue, &event, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        switch (event.type)
        {
            case UART_DATA:
            {
                /* 从 UART FIFO 读取数据 */
                const int len = uart_read_bytes(
                    UART_PORT, tmp_buf, sizeof(tmp_buf), pdMS_TO_TICKS(100));
                if (len <= 0)
                {
                    break;
                }

                /* 写入 RingBuf */
                if (xRingbufferSend(s_ringbuf, tmp_buf, (size_t)len,
                                    pdMS_TO_TICKS(10)) != pdTRUE)
                {
                    ESP_LOGW(TAG, "RingBuf full, dropping %d bytes", len);
                    esp_event_post(CMCAR_EVENTS, CMCAR_UART_ERROR, NULL, 0, 0);
                    break;
                }

                /* 从 RingBuf 取出并解析 */
                size_t item_size;
                uint8_t* item = xRingbufferReceive(s_ringbuf, &item_size, 0);
                if (item != NULL && item_size > 0)
                {
                    /* 解析（零 malloc） */
                    pkt_frame_t frame;
                    int frames_found = pkt_parser_feed_buf(
                        &s_parser, item, item_size, &frame, 1);

                    for (int i = 0; i < frames_found; i++)
                    {
                        if (s_rx_cb != NULL)
                        {
                            s_rx_cb(&frame, s_rx_cb_ctx);
                        }
                    }

                    vRingbufferReturnItem(s_ringbuf, item);
                }
                break;
            }

            case UART_FIFO_OVF:
                ESP_LOGE(TAG, "FIFO overflow");
                uart_flush_input(UART_PORT);
                esp_event_post(CMCAR_EVENTS, CMCAR_UART_ERROR, NULL, 0, 0);
                break;

            case UART_BUFFER_FULL:
                ESP_LOGW(TAG, "Ring buffer full");
                uart_flush_input(UART_PORT);
                break;

            case UART_PARITY_ERR:
                ESP_LOGW(TAG, "Parity error");
                break;

            case UART_FRAME_ERR:
                ESP_LOGW(TAG, "Frame error");
                uart_flush_input(UART_PORT);
                break;

            default:
                break;
        }
    }
}

static void uart_tx_task(void* arg)
{
    uart_tx_msg_t msg;

    while (1)
    {
        if (xQueueReceive(s_tx_queue, &msg, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        const int written = uart_write_bytes(UART_PORT, msg.data, msg.len);
        if (written < 0 || (size_t)written != msg.len)
        {
            ESP_LOGE(TAG, "UART write error: wrote %d/%d", written, msg.len);
        }
    }
}

/* ==================== 公共 API ==================== */

esp_err_t uart_service_init(void)
{
    /* 配置 UART */
    const uart_config_t uart_cfg = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    esp_err_t ret = uart_param_config(UART_PORT, &uart_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "param_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "set_pin failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 安装驱动（IRAM 中断） */
    ret = uart_driver_install(UART_PORT, RX_BUF_SIZE, TX_BUF_SIZE,
                              UART_EVENT_QUEUE_SIZE, &s_uart_event_queue,
                              ESP_INTR_FLAG_IRAM);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 创建静态 RingBuf */
    s_ringbuf = xRingbufferCreateStatic(RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF,
                                        s_ringbuf_storage, &s_ringbuf_struct);
    if (s_ringbuf == NULL)
    {
        ESP_LOGE(TAG, "Failed to create RingBuf");
        uart_driver_delete(UART_PORT);
        return ESP_ERR_NO_MEM;
    }

    /* 创建静态 TX 队列 */
    s_tx_queue = xQueueCreateStatic(TX_QUEUE_DEPTH, sizeof(uart_tx_msg_t),
                                    (uint8_t*)s_tx_queue_storage,
                                    &s_tx_queue_struct);

    /* 初始化协议解析器 */
    pkt_parser_init(&s_parser);

    ESP_LOGI(TAG, "UART service initialized (port=%d, baud=%d, TX=%d, RX=%d)",
             UART_PORT, UART_BAUD, UART_TX_PIN, UART_RX_PIN);
    return ESP_OK;
}

esp_err_t uart_service_start(void)
{
    /* 创建静态 RX 任务 */
    TaskHandle_t rx_handle = xTaskCreateStatic(
        uart_rx_task, "uart_rx",
        UART_RX_TASK_STACK / sizeof(StackType_t),
        NULL, TASK_PRIO_UART_RX,
        s_rx_stack, &s_rx_tcb);
    if (rx_handle == NULL)
    {
        return ESP_FAIL;
    }

    /* 创建静态 TX 任务 */
    TaskHandle_t tx_handle = xTaskCreateStatic(
        uart_tx_task, "uart_tx",
        UART_TX_TASK_STACK / sizeof(StackType_t),
        NULL, TASK_PRIO_UART_TX,
        s_tx_stack, &s_tx_tcb);
    if (tx_handle == NULL)
    {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "UART tasks started");
    return ESP_OK;
}

esp_err_t uart_service_register_rx_cb(uart_frame_rx_cb_t cb, void* user_ctx)
{
    if (cb == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    s_rx_cb = cb;
    s_rx_cb_ctx = user_ctx;
    return ESP_OK;
}

esp_err_t uart_service_send(const uint8_t* data, size_t len)
{
    if (data == NULL || len == 0 || len > PKT_MAX_FRAME_SIZE)
    {
        return ESP_ERR_INVALID_ARG;
    }

    uart_tx_msg_t msg = { .len = (uint8_t)len };
    memcpy(msg.data, data, len);

    if (xQueueSend(s_tx_queue, &msg, pdMS_TO_TICKS(100)) != pdPASS)
    {
        ESP_LOGW(TAG, "TX queue full, frame dropped");
        return CMCAR_ERR_QUEUE_FULL;
    }

    return ESP_OK;
}

esp_err_t uart_service_stop(void)
{
    uart_driver_delete(UART_PORT);
    if (s_ringbuf != NULL)
    {
        vRingbufferDelete(s_ringbuf);
        s_ringbuf = NULL;
    }
    return ESP_OK;
}
