#include "uart_manager.h"

#include <string.h>

#include "app_events.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "queue_manager.h"
#include "task_priorities.h"
#include "pkt_protocol_buf.h"
#include "mqtt_logger.h"

static const char* TAG = "UARTManager";

// 波特率
#define BAUD_RATE (115200)
// UART事件队列大小
#define UART_EVENT_QUEUE_SIZE (10)
// 累积缓冲区大小
#define ACCUMULATE_BUF_SIZE (RING_BUFFER_SIZE + 512)

// GPIO Define
#define UART_RX_PIN GPIO_NUM_5
#define UART_TX_PIN GPIO_NUM_4
#define UART_PORT UART_NUM_1

// Task Constants
#define UART_EVENT_TASK_STACK_SIZE (4096)
#define RING_BUFFER_TASK_STACK_SIZE (4096)
#define MQTT_MESSAGE_TASK_STACK_SIZE (4096)

// UART事件队列
static QueueHandle_t uart_event_queue;
// 原始数据环形缓冲区
static RingbufHandle_t raw_rb_handle;
// 协议解析处理器
static protocol_receiver receiver;

static void handle_uart_event_task(void* pvParameters);
static void handle_ring_buffer_task(void* pvParameters);
static void handle_mqtt_message_task(void* pvParameters);

static void send_frame_to_mqtt_queue(uint8_t type, const uint8_t* frame_data, uint16_t frame_len);


/**
 * @brief 初始化UART模块
 */
void uart_init()
{
    const uart_config_t uart_config = {
        .baud_rate = BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE // 明确禁用硬件流控
    };

    // 安装UART驱动，启用DMA
    const esp_err_t err =
        uart_driver_install(UART_PORT,             // UART端口号
                            UART_BUF_SIZE * 2,     // RX缓冲区大小 最好是UART_FIFO_LEN(128）的整数倍
                            UART_BUF_SIZE,         // TX缓冲区大小
                            UART_EVENT_QUEUE_SIZE, // 事件队列的大小
                            &uart_event_queue,     // 事件队列的句柄
                            ESP_INTR_FLAG_IRAM     // 中断分配标志
        );
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to install UART driver: %s", esp_err_to_name(err));
        return;
    }

    // 初始化 UART 参数
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_config));
    // 设置 UART 引脚
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN,
        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    // 创建RAW DATA环形缓冲区
    raw_rb_handle = xRingbufferCreate(RING_BUFFER_SIZE,      // 缓冲区大小
                                      RINGBUF_TYPE_BYTEBUF); // 类型
    if (raw_rb_handle == NULL)
    {
        ESP_LOGE(TAG, "failed to create raw_rb_handle");
        uart_driver_delete(UART_PORT);
        return;
    }

    // 创建接收UART事件的任务
    if (xTaskCreate(handle_uart_event_task, "handle_uart_event_task",
                    UART_EVENT_TASK_STACK_SIZE, NULL, TASK_PRIORITY_UART_EVENT,
                    NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create UART event task");
        uart_driver_delete(UART_PORT);
        vRingbufferDelete(raw_rb_handle);
        return;
    }

    // 创建发送UART消息的任务
    if (xTaskCreate(handle_ring_buffer_task, "handle_ring_buffer_task",
                    RING_BUFFER_TASK_STACK_SIZE, NULL,
                    TASK_PRIORITY_UART_BUFFER_PROCESS, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create handle_ring_buffer_task");
        uart_driver_delete(UART_PORT);
        vRingbufferDelete(raw_rb_handle);
        return;
    }

    // 创建UART发送数据队列
    if (!queue_manager_init(UART))
    {
        ESP_LOGE(TAG, "Failed to create UART send queue");
        uart_driver_delete(UART_PORT);
        vRingbufferDelete(raw_rb_handle);
        return;
    }

    // 创建接收MQTT指令消息队列的任务
    if (xTaskCreate(handle_mqtt_message_task, "handle_mqtt_message_task",
                    MQTT_MESSAGE_TASK_STACK_SIZE, NULL,
                    TASK_PRIORITY_UART_HANDLE_MQTT_MSG, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create handle_mqtt_message_task");
    }

    protocol_receiver_init(&receiver, ACCUMULATE_BUF_SIZE, send_frame_to_mqtt_queue);
    if (receiver.buffer == NULL)
    {
        ESP_LOGE(TAG, "Failed to create protocol receiver");
        uart_driver_delete(UART_PORT);
        vRingbufferDelete(raw_rb_handle);
    }
    app_events_set_bits(UART_INIT_COMPLETE);
}

/**
 * @brief 释放UART模块相关资源
 */
void uart_deinit()
{
    // 释放缓冲区
    if (raw_rb_handle)
    {
        vRingbufferDelete(raw_rb_handle);
    }

    // 删除UART驱动
    uart_driver_delete(UART_PORT);

    // 释放协议接收器
    protocol_receiver_destroy(&receiver);
}

/**
 * @brief 处理UART事件回调函数
 * @param pvParameters 任务参数
 */
static void handle_uart_event_task(void* pvParameters)
{
    uart_event_t event;
    while (1)
    {
        // 等待UART事件
        if (xQueueReceive(uart_event_queue, &event, portMAX_DELAY))
        {
            switch (event.type)
            {
                case UART_DATA:
                    {
                        /*
                         * 读取新数据到临时缓冲区，考虑到协议数据包最大长度和接收缓冲区大小一致，理论不会超，采用简单的截取方案
                         */
                        static uint8_t tmp_buf[UART_BUF_SIZE];
                        size_t buffered_size;
                        ESP_ERROR_CHECK(uart_get_buffered_data_len(UART_PORT, &buffered_size));
                        const int len = uart_read_bytes(UART_PORT, tmp_buf, buffered_size, (100 / portTICK_PERIOD_MS));
                        if (len <= 0)
                        {
                            continue;
                        }

                        // 仅将原始数据存入缓冲区
                        if (xRingbufferSend(raw_rb_handle, tmp_buf, len, portMAX_DELAY) != pdTRUE)
                        {
                            LOG_E(MQTT_LOG_ERROR, TAG, "Failed to send raw data to ringbuffer, retrying...");

                            vTaskDelay(10 / portTICK_PERIOD_MS);
                            if (xRingbufferSend(raw_rb_handle, tmp_buf, len, portMAX_DELAY) != pdTRUE)
                            {
                                LOG_E(MQTT_LOG_ERROR, TAG, "Retry failed, dropping data...");
                            }
                        }
                        break;
                    }
                case UART_FIFO_OVF:
                    {
                        LOG_E(MQTT_LOG_ERROR, TAG, "hw fifo overflow");
                        // If fifo overflow happened, you should consider adding flow control
                        // for your application. The ISR has already reset the rx FIFO, As an
                        // example, we directly flush the rx buffer here in order to read more
                        // data.
                        uart_flush_input(UART_PORT);
                        // xQueueReset(uart_event_queue);
                        break;
                    }
                case UART_BUFFER_FULL:
                    {
                        // Event of UART ring buffer full
                        LOG_E(MQTT_LOG_ERROR, TAG, "ring buffer full");
                        // If buffer full happened, you should consider increasing your buffer
                        // size As an example, we directly flush the rx buffer here in order
                        // to read more data.
                        uart_flush_input(UART_PORT);
                        // xQueueReset(uart_event_queue);
                        break;
                    }
                case UART_BREAK:
                    {
                        // Event of UART RX break detected
                        LOG_E(MQTT_LOG_ERROR, TAG, "uart rx break");
                        uart_flush_input(UART_PORT);
                        // xQueueReset(uart_event_queue);
                        break;
                    }
                case UART_PARITY_ERR:
                    {
                        LOG_E(MQTT_LOG_ERROR, TAG, "uart parity error");
                        break;
                    }
                case UART_FRAME_ERR:
                    {
                        LOG_E(MQTT_LOG_ERROR, TAG, "uart frame error");
                        uart_flush_input(UART_PORT);
                        break;
                    }
                case UART_PATTERN_DET:
                    {
                        LOG_E(MQTT_LOG_ERROR, TAG, "uart pattern detected");
                        break;
                    }
                default:
                    {
                        LOG_E(MQTT_LOG_ERROR, TAG, "uart event type: %d", event.type);
                        break;
                    }
            }
        }
    }
    vTaskDelete(NULL);
}

/**
 * 轮询环形缓冲区中的数据，解析并将数据包发送到消息队列
 * @param pvParameters  任务参数
 */
static void handle_ring_buffer_task(void* pvParameters)
{
    while (1)
    {
        size_t item_size;
        uint8_t* raw_data = (uint8_t*)xRingbufferReceive(raw_rb_handle, &item_size, portMAX_DELAY);
        if (raw_data == NULL)
        {
            // ESP_LOGE(TAG, "Ringbuffer receive returned NULL");
            LOG_E(MQTT_LOG_ERROR, TAG, "Ringbuffer receive returned NULL");
            continue;
        }
        if (item_size == 0)
        {
            // ESP_LOGE(TAG, "Invalid zero-length data");
            LOG_E(MQTT_LOG_ERROR, TAG, "Invalid zero-length data");
            vRingbufferReturnItem(raw_rb_handle, raw_data);
            continue;
        }

        // 通过累积缓冲区接收并解析数据，将解析的帧数据发送到消息队列
        protocol_receiver_append(&receiver, raw_data, item_size);

        // 释放原始数据
        vRingbufferReturnItem(raw_rb_handle, raw_data);

        // 适度降低CPU占用
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    protocol_receiver_destroy(&receiver);
    vTaskDelete(NULL);
}

/**
 * 轮询MQTT数据队列，通过UART发送出去
 * @param pvParameters 任务参数
 */
static void handle_mqtt_message_task(void* pvParameters)
{
    QueueHandle_t mqtt_data_queue = NULL;

    // Wait up to 5 seconds for queue initialization
    const TickType_t max_queue_wait = pdMS_TO_TICKS(5000);
    int retry_count = 0;

    while ((mqtt_data_queue = queue_manager_get_queue(MQTT)) == NULL)
    {
        const int max_retries = 5;
        if (retry_count++ >= max_retries)
        {
            ESP_LOGE(TAG, "Failed to get MQTT queue after %d attempts", max_retries);
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGW(TAG, "MQTT send queue not available, retrying (%d/%d)",
                 retry_count, max_retries);
        vTaskDelay(max_queue_wait / max_retries);
    }

    ESP_LOGD(TAG, "MQTT send queue available");

    const TickType_t queue_timeout = pdMS_TO_TICKS(1000); // 1s超时
    while (1)
    {
        QueueMessage_t mqtt_msg = {0};
        const BaseType_t receive_status =
            xQueueReceive(mqtt_data_queue, &mqtt_msg, queue_timeout);

        if (receive_status != pdPASS)
        {
            if (receive_status == errQUEUE_EMPTY)
            {
                ESP_LOGD(TAG, "MQTT queue empty after 1 second timeout");
            }
            else
            {
                ESP_LOGE(TAG, "Failed to receive message from MQTT queue (error: %d)",
                         receive_status);
            }
            continue;
        }

        // 验证数据有效性
        if (mqtt_msg.data == NULL || mqtt_msg.len == 0)
        {
            ESP_LOGE(TAG, "Invalid MQTT message (null data or zero length)");
            free_queue_message(&mqtt_msg);
            continue;
        }

        // 直接转发协议帧数据
        if (!uart_send_data(mqtt_msg.data, mqtt_msg.len))
        {
            ESP_LOGE(TAG, "Failed to send MQTT data via UART");
        }
        // 释放内存
        free_queue_message(&mqtt_msg);
    }
}

/**
 * 将接收并解析后的UART数据发送到MQTT队列
 * @param type
 * @param frame_data 帧数据
 * @param frame_len 帧长度
 */
static void send_frame_to_mqtt_queue(const uint8_t type, const uint8_t* frame_data, const uint16_t frame_len)
{
    if (frame_data == NULL || frame_len == 0)
    {
        ESP_LOGE(TAG, "Invalid UART frame data");
        return;
    }
    const QueueMessage_t msg = {
        .type = UART,
        .protocol_type = type,
        .data = malloc(frame_len),
        .len = frame_len,
    };
    if (msg.data == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for UART send queue data");
        return;
    }
    memcpy(msg.data, frame_data, frame_len);

    // 发送数据
    QueueHandle_t uart_data_queue = queue_manager_get_queue(UART);
    if (uart_data_queue == NULL)
    {
        ESP_LOGE(TAG, "UART data queue has not been initialized");
        free(msg.data);
        return;
    }
    if (xQueueSend(uart_data_queue, &msg, portMAX_DELAY) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to send message to queue");
        free(msg.data);
    }
}

/**
 * @brief 向UART发送数据
 * @param data 数据指针
 * @param len 数据长度
 * @return 是否发送成功
 */
bool uart_send_data(const uint8_t* data, const size_t len)
{
    const int bytes_written = uart_write_bytes(UART_PORT, data, len);
    if (bytes_written < 0)
    {
        ESP_LOGE(TAG, "UART write error: %s", esp_err_to_name(bytes_written));
        return false;
    }
    return bytes_written == len;
}
