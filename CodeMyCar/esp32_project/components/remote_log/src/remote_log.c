/**
 * @file remote_log.c
 * @brief 远程日志实现（静态 buffer pool + 静态队列 + 静态任务）
 */

#include "remote_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cmcar_topics.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_service.h"
#include "task_priorities.h"

static const char* TAG = "rlog";

#define LOG_MSG_MAX_LEN 256
#define LOG_QUEUE_DEPTH 16
#define LOG_TASK_STACK  2048

/* 日志消息（固定大小，值传递） */
typedef struct {
    char buf[LOG_MSG_MAX_LEN];
    uint16_t len;
    uint8_t level;
} log_msg_t;

/* 静态资源 */
static log_msg_t s_log_queue_storage[LOG_QUEUE_DEPTH];
static StaticQueue_t s_log_queue_struct;
static QueueHandle_t s_log_queue = NULL;

static StackType_t s_log_stack[LOG_TASK_STACK / sizeof(StackType_t)];
static StaticTask_t s_log_tcb;

/* 与 bridge_log_to_json 一致的 level 字符串，使 ESP32 自身日志也走 pyserver
   LogEntry.model_validate_json 路径（JSON-only 契约），而非纯文本回退分支 */
static const char* s_level_str[] = { "ERROR", "WARNING", "INFO", "DEBUG" };

/* 日志任务：出队 → MQTT 发布 */
static void log_task(void* arg)
{
    log_msg_t msg;

    while (1)
    {
        if (xQueueReceive(s_log_queue, &msg, portMAX_DELAY) == pdTRUE)
        {
            if (mqtt_service_is_connected())
            {
                mqtt_service_publish(CMCAR_TOPIC_LOG, msg.buf,
                                     msg.len, 0);
            }
        }
    }
}

esp_err_t remote_log_init(void)
{
    s_log_queue = xQueueCreateStatic(LOG_QUEUE_DEPTH, sizeof(log_msg_t),
                                     (uint8_t*)s_log_queue_storage,
                                     &s_log_queue_struct);
    if (s_log_queue == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Remote log initialized");
    return ESP_OK;
}

esp_err_t remote_log_start(void)
{
    TaskHandle_t handle = xTaskCreateStatic(
        log_task, "remote_log",
        LOG_TASK_STACK / sizeof(StackType_t),
        NULL, TASK_PRIO_REMOTE_LOG,
        s_log_stack, &s_log_tcb);

    return (handle != NULL) ? ESP_OK : ESP_FAIL;
}

void remote_log_write(remote_log_level_t level, const char* tag,
                      const char* fmt, ...)
{
    if (s_log_queue == NULL)
    {
        return;
    }

    /* 日志级别过滤 */
#ifdef CONFIG_CMCAR_REMOTE_LOG_LEVEL
    if (level > CONFIG_CMCAR_REMOTE_LOG_LEVEL)
    {
        return;
    }
#endif

    const uint8_t lvl = (uint8_t)level & 0x03;

    /* 先把用户消息格式化到临时缓冲 */
    char text[LOG_MSG_MAX_LEN];
    va_list args;
    va_start(args, fmt);
    int tlen = vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    if (tlen < 0)
    {
        return;
    }
    if ((size_t)tlen >= sizeof(text))
    {
        tlen = (int)sizeof(text) - 1;
    }

    const int64_t ts_ms = esp_timer_get_time() / 1000;

    log_msg_t msg = { .level = lvl };

    /* 组装 JSON LogEntry：{"level":"..","tag":"..","message":"<escaped>","timestamp":N}
       超长则截断 message（仍发，不丢弃） */
    int off = snprintf(msg.buf, LOG_MSG_MAX_LEN,
                       "{\"level\":\"%s\",\"tag\":\"%s\",\"message\":\"",
                       s_level_str[lvl], tag ? tag : "");
    if (off < 0)
    {
        return;
    }

    /* 预留尾部空间：","timestamp":<int64>}` 约 32 字节 */
    const int tail_reserve = 32;
    const int escape_cap = LOG_MSG_MAX_LEN - tail_reserve;
    for (int i = 0; i < tlen && off < escape_cap; i++)
    {
        const char c = text[i];
        if (c == '"' || c == '\\')
        {
            if (off + 2 < escape_cap)
            {
                msg.buf[off++] = '\\';
                msg.buf[off++] = c;
            }
            else
            {
                break;
            }
        }
        else if ((unsigned char)c < 0x20)
        {
            msg.buf[off++] = ' '; /* 控制字符→空格，保证 JSON 合法可读 */
        }
        else
        {
            msg.buf[off++] = c;
        }
    }

    int tail = snprintf(msg.buf + off, LOG_MSG_MAX_LEN - off,
                        "\",\"timestamp\":%lld}", (long long)ts_ms);
    if (tail < 0)
    {
        return;
    }
    off += tail;
    if (off > LOG_MSG_MAX_LEN - 1)
    {
        off = LOG_MSG_MAX_LEN - 1;
    }
    msg.buf[off] = '\0';
    msg.len = (uint16_t)off;

    /* 入队（非阻塞，满则丢弃） */
    if (xQueueSend(s_log_queue, &msg, 0) != pdPASS)
    {
        /* 队列满，静默丢弃 */
    }
}
