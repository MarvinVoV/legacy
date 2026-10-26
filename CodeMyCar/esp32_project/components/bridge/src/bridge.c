/**
 * @file bridge.c
 * @brief 桥接编排层（UART↔MQTT）
 *
 * 职责：
 *   下行：MQTT JSON ControlCommand → bridge_codec 转 13 字节 ControlCmd →
 *         pkt_encode（ACK_REQ）→ UART；pending 表负责超时重传。
 *   上行：UART 帧（SENSOR/LOG 二进制）→ bridge_codec 转 JSON → MQTT 发布。
 *   ACK：收到 TYPE=ACK 帧即从 pending 表清除对应 seq。
 *   状态：INIT → CONNECTING → ONLINE → DEGRADED。
 */

#include "bridge.h"

#include <string.h>

#include "bridge_codec.h"
#include "ack_manager.h"
#include "cmcar_events.h"
#include "cmcar_topics.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_service.h"
#include "pkt_codec.h"
#include "task_priorities.h"
#include "uart_service.h"

static const char* TAG = "bridge";

/* ==================== 系统状态 ==================== */

typedef enum
{
    SYS_STATE_INIT,
    SYS_STATE_CONNECTING,
    SYS_STATE_ONLINE,
    SYS_STATE_DEGRADED,
} sys_state_t;

static sys_state_t s_state = SYS_STATE_INIT;

/* ==================== 上行帧队列（uart_rx → uplink task） ==================== */

#define FRAME_QUEUE_DEPTH 8

typedef struct
{
    pkt_frame_t frame;
    uint32_t timestamp;
    bool is_ack_echo;   /* true：本条是 ACK 回执（ack_seq 有效），非上行帧 */
    uint8_t ack_seq;   /* 被确认的 seq */
} bridge_frame_msg_t;

static bridge_frame_msg_t s_frame_queue_storage[FRAME_QUEUE_DEPTH];
static StaticQueue_t s_frame_queue_struct;
static QueueHandle_t s_frame_queue = NULL;

/* 上行任务（静态分配） */
#define BRIDGE_UP_STACK 3072
static StackType_t s_uplink_stack[BRIDGE_UP_STACK / sizeof(StackType_t)];
static StaticTask_t s_uplink_tcb;

/* ==================== 下行命令队列（mqtt_cb → downlink task） ==================== */
/* on_mqtt_data 跑在 esp_mqtt client 任务上，不能阻塞在 UART TX；故只投队列即返回，
   译码/组帧/发送由独立 downlink 任务完成。命令 JSON 远小于 256 字节。 */

#define DOWNLINK_QUEUE_DEPTH 4
#define DOWNLINK_PAYLOAD_MAX 256
#define BRIDGE_DOWN_STACK 3072

typedef struct
{
    char payload[DOWNLINK_PAYLOAD_MAX];
    size_t len;
} downlink_msg_t;

static downlink_msg_t s_downlink_storage[DOWNLINK_QUEUE_DEPTH];
static StaticQueue_t s_downlink_queue_struct;
static QueueHandle_t s_downlink_queue = NULL;
static StackType_t s_downlink_stack[BRIDGE_DOWN_STACK / sizeof(StackType_t)];
static StaticTask_t s_downlink_tcb;

/* ACK 重传扫描定时器（esp_timer，从 uplink 任务剥离，避免 publish 阻塞拖累重传时序） */
static esp_timer_handle_t s_retransmit_timer = NULL;

/* 状态上报定时器（周期发 CMCAR_TOPIC_STATUS） */
static esp_timer_handle_t s_status_timer = NULL;
#define BRIDGE_STATUS_PERIOD_MS 30000

/* ==================== 下行 ACK pending 表（ack_manager 组件 + 调用方加锁） ==================== */

#define BRIDGE_ACK_TIMEOUT_MS 200
#define BRIDGE_ACK_MAX_RETRIES 3
#define BRIDGE_RETRANSMIT_PERIOD_MS 50
#define BRIDGE_FIELD_MOTION         0x01

static ack_manager_t s_ackmgr;
static SemaphoreHandle_t s_ack_mutex = NULL;

/* 运行指标（best-effort，多上下文自增，允许轻微竞态） */
static volatile uint32_t s_uplink_dropped = 0;   /* 上行帧队列满丢弃 */
static volatile uint32_t s_downlink_dropped = 0;  /* 下行命令队列满丢弃 */
static volatile uint32_t s_ack_lost = 0;         /* ACK 超时耗尽重试 */
static volatile uint32_t s_pub_failed = 0;        /* MQTT publish 失败 */
static volatile uint32_t s_last_stm32_frame_ms = 0; /* 最近一次有效 UART 帧时间 */

static const char* state_name(sys_state_t s)
{
    switch (s)
    {
        case SYS_STATE_INIT: return "INIT";
        case SYS_STATE_CONNECTING: return "CONNECTING";
        case SYS_STATE_ONLINE: return "ONLINE";
        case SYS_STATE_DEGRADED: return "DEGRADED";
        default: return "UNKNOWN";
    }
}

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

bool bridge_stm32_connected(void)
{
    const uint32_t last = s_last_stm32_frame_ms;
    return (last != 0U) && ((now_ms() - last) < BRIDGE_STM32_TIMEOUT_MS);
}

/* resend 回调（注入 uart_service_send）：ackmgr_scan 在持有 s_ack_mutex 时调用；
   uart_service_send 内部 xQueueSend 非阻塞，不会自死锁 */
static void ack_resend_cb(const uint8_t* frame, size_t len, void* ctx)
{
    (void)ctx;
    uart_service_send(frame, len);
}

/** 存入已编码帧用于 ACK 跟踪；失败（dup seq / 表满）时调用方退化为不请求 ACK 直发 */
static bool pending_add(uint8_t seq, const uint8_t* frame, size_t frame_len)
{
    bool ok = false;
    if (xSemaphoreTake(s_ack_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        ok = ackmgr_add(&s_ackmgr, seq, frame, frame_len, now_ms());
        xSemaphoreGive(s_ack_mutex);
    }
    return ok;
}

/** 收到 ACK：清除对应 seq 的在途槽位 */
static void pending_clear(uint8_t seq)
{
    if (xSemaphoreTake(s_ack_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        ackmgr_clear(&s_ackmgr, seq);
        xSemaphoreGive(s_ack_mutex);
    }
}

/** 周期扫描：超时重传，达上限则标记丢失 */
static void pending_retransmit_scan(void)
{
    if (xSemaphoreTake(s_ack_mutex, 0) != pdTRUE)
    {
        return; /* 非阻塞：拿不到锁就下个周期再扫 */
    }
    const uint32_t lost = ackmgr_scan(&s_ackmgr, now_ms(),
                                      BRIDGE_ACK_TIMEOUT_MS,
                                      BRIDGE_ACK_MAX_RETRIES,
                                      ack_resend_cb, NULL);
    if (lost > 0)
    {
        s_ack_lost += lost;
        ESP_LOGW(TAG, "%lu ACK(s) lost after %d retries",
                 (unsigned long)lost, BRIDGE_ACK_MAX_RETRIES);
    }
    xSemaphoreGive(s_ack_mutex);
}

/* ==================== 下行：MQTT JSON → UART ==================== */

/** 直接回复 ping（不经过 UART/STM32），用于 MQTT 链路自检 */
static void publish_pong(uint8_t seq)
{
    char json[96];
    const unsigned long long uptime_s =
        (unsigned long long)(esp_timer_get_time() / 1000000LL);
    const int n = bridge_pong_to_json(
        seq, uptime_s, (unsigned long)esp_get_free_heap_size(), json, sizeof(json));
    if (n <= 0)
    {
        ESP_LOGW(TAG, "pong json build failed");
        return;
    }
    const esp_err_t err = mqtt_service_publish_timed(
        CMCAR_TOPIC_PONG, json, (size_t)n, 0, 200);
    if (err != ESP_OK)
    {
        ESP_LOGD(TAG, "pong publish skipped: %s", esp_err_to_name(err));
    }
}

/** 译码 + 组帧 + 发送（在 downlink 任务上下文执行，不阻塞 MQTT client 任务） */
static void process_downlink(const char* payload, size_t payload_len)
{
    ESP_LOGI(TAG, "CMD downlink len=%zu", payload_len);

    /* ping：MQTT 链路自检，直接回 pong，不进入 UART/STM32 路径 */
    uint8_t ping_seq = 0;
    if (bridge_json_is_ping(payload, payload_len, &ping_seq))
    {
        publish_pong(ping_seq);
        return;
    }

    /* JSON → 9 字节 ControlCmd */
    uint8_t cmd_payload[BRIDGE_CONTROL_CMD_SIZE];
    uint8_t client_seq = 0;
    bool ack_req = false;
    const int cmd_len = bridge_json_to_control_cmd(
        payload, payload_len, cmd_payload, &client_seq, &ack_req);
    if (cmd_len <= 0)
    {
        ESP_LOGW(TAG, "Invalid/unhandled command JSON");
        return;
    }

    /* 组帧：优先使用客户端 seq，便于服务端把 ACK 关联回请求；
       若该 seq 已在途，再分配本地 seq 避免碰撞 */
    uint8_t frame_seq = client_seq;
    if (ack_req && !ackmgr_is_free(&s_ackmgr, client_seq))
    {
        frame_seq = ackmgr_alloc_seq(&s_ackmgr);
    }
    uint8_t flags = ack_req ? PKT_FLAG_ACK_REQ : 0;
    pkt_frame_t frame = {
        .type = PKT_TYPE_CONTROL,
        .seq = frame_seq,
        .flags = flags,
        .payload_len = (uint8_t)cmd_len,
    };
    memcpy(frame.payload, cmd_payload, cmd_len);

    uint8_t out_buf[PKT_MAX_FRAME_SIZE];
    size_t out_len = 0;
    if (pkt_encode(&frame, out_buf, sizeof(out_buf), &out_len) != ESP_OK)
    {
        ESP_LOGW(TAG, "pkt_encode failed");
        return;
    }

    /* ACK 表满或 seq 回绕碰撞：退化为不请求 ACK 直发（不重传，但至少送达一次） */
    if (ack_req && !pending_add(frame_seq, out_buf, out_len))
    {
        ESP_LOGW(TAG, "Pending ACK table full/collision; sending without ACK");
        frame.flags = 0;
        if (pkt_encode(&frame, out_buf, sizeof(out_buf), &out_len) != ESP_OK)
        {
            ESP_LOGW(TAG, "pkt_encode(plain) failed");
            return;
        }
    }

    uart_service_send(out_buf, out_len);
}

/* MQTT 下行回调（esp_mqtt client 任务上下文）：仅投队列即返回，不阻塞 client 任务 */
static void on_mqtt_data(const char* topic, size_t topic_len,
                         const uint8_t* payload, size_t payload_len,
                         void* user_ctx)
{
    (void)user_ctx;
    (void)topic;
    (void)topic_len;

    if (payload_len > DOWNLINK_PAYLOAD_MAX)
    {
        ESP_LOGW(TAG, "CMD too large (%zu > %d), dropping", payload_len,
                 DOWNLINK_PAYLOAD_MAX);
        return;
    }

    downlink_msg_t msg;
    msg.len = payload_len;
    memcpy(msg.payload, payload, payload_len);
    if (xQueueSend(s_downlink_queue, &msg, 0) != pdPASS)
    {
        ESP_LOGW(TAG, "Downlink queue full, dropping cmd");
        s_downlink_dropped++;
    }
}

/** 下行任务：downlink_queue → process_downlink */
static void bridge_downlink_task(void* arg)
{
    (void)arg;
    downlink_msg_t msg;
    while (1)
    {
        if (xQueueReceive(s_downlink_queue, &msg, portMAX_DELAY) == pdTRUE)
        {
            process_downlink(msg.payload, msg.len);
        }
    }
}

/* ==================== 上行：UART 帧 → MQTT JSON ==================== */

/* 帧接收回调（uart_rx 任务上下文）：ACK 帧就地清除，其余入上行队列 */
static void on_uart_frame(const pkt_frame_t* frame, void* user_ctx)
{
    (void)user_ctx;

    /* 任意有效帧都说明 STM32 在线（用于心跳的 stm32_connected） */
    s_last_stm32_frame_ms = now_ms();

    /* ACK 响应帧：清除 pending，并向 uplink 队列投递一条 ACK 回执
       （由 uplink 任务异步发往 CMCAR_TOPIC_ACK，不在 uart_rx 里阻塞 publish） */
    if (frame->type == PKT_TYPE_ACK && (frame->flags & PKT_FLAG_ACK_RSP))
    {
        /* v2：ACK 的 seq 在帧头 SEQ 字段（STM32 的 ACK payload 为空） */
        const uint8_t ack_seq = frame->seq;
        pending_clear(ack_seq);
        bridge_frame_msg_t echo = {
            .timestamp = now_ms(),
            .is_ack_echo = true,
            .ack_seq = ack_seq,
        };
        if (xQueueSend(s_frame_queue, &echo, 0) != pdPASS)
        {
            ESP_LOGW(TAG, "ACK echo queue full, dropping ack seq=%u", ack_seq);
        }
        return;
    }

    bridge_frame_msg_t msg = {
        .frame = *frame,
        .timestamp = now_ms(),
    };
    if (xQueueSend(s_frame_queue, &msg, 0) != pdPASS)
    {
        ESP_LOGW(TAG, "Frame queue full, dropping type=%d", frame->type);
        s_uplink_dropped++;
    }
}

/** 把 ACK 回执发往 CMCAR_TOPIC_ACK（端到端命令确认：pyserver 据此标记命令已送达 STM32） */
static void publish_ack_echo(uint8_t seq)
{
    if (!mqtt_service_is_connected())
    {
        return;
    }
    char json[40];
    const int n = snprintf(json, sizeof(json),
                           "{\"seq\":%u,\"ack\":true}", (unsigned)seq);
    if (n > 0 && (size_t)n < sizeof(json))
    {
        const esp_err_t err = mqtt_service_publish_timed(
            CMCAR_TOPIC_ACK, json, (size_t)n, 0, 200);
        if (err != ESP_OK)
        {
            ESP_LOGD(TAG, "ACK echo publish skipped: %s", esp_err_to_name(err));
        }
    }
}

/** 将一帧上行数据转 JSON 并发布 */
static void publish_uplink_frame(const pkt_frame_t* frame)
{
    if (!mqtt_service_is_connected())
    {
        return;
    }

    char json[320];
    int n = 0;
    const char* topic = NULL;

    switch (frame->type)
    {
        case PKT_TYPE_SENSOR:
            n = bridge_sensor_to_json(frame->payload, frame->payload_len,
                                      json, sizeof(json));
            topic = CMCAR_TOPIC_SENSOR;
            break;
        case PKT_TYPE_LOG:
            n = bridge_log_to_json(frame->payload, frame->payload_len,
                                   json, sizeof(json));
            topic = CMCAR_TOPIC_LOG;
            break;
        default:
            return; /* HEARTBEAT 由 health_monitor 直发；其余忽略 */
    }

    if (n > 0)
    {
        const esp_err_t err = mqtt_service_publish(topic, json, (size_t)n, 0);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "Publish failed: %s", esp_err_to_name(err));
            s_pub_failed++;
        }
    }
    else
    {
        ESP_LOGW(TAG, "Uplink conversion failed for type=%d", frame->type);
    }
}

/* ACK 重传扫描定时器回调（esp_timer 任务上下文）：
   pending_retransmit_scan 用非阻塞 mutex + uart_service_send(xQueueSend 非阻塞)，满足
   timer 回调不阻塞的约束。从 uplink 任务剥离，避免 publish 阻塞拖累重传时序。 */
static void on_retransmit_timer(void* arg)
{
    (void)arg;
    pending_retransmit_scan();
}

/* 上行任务：frame_queue → JSON → MQTT（重传扫描由 esp_timer 负责，不在此） */
static void bridge_uplink_task(void* arg)
{
    (void)arg;
    bridge_frame_msg_t msg;

    while (1)
    {
        if (xQueueReceive(s_frame_queue, &msg, portMAX_DELAY) == pdTRUE)
        {
            if (msg.is_ack_echo)
            {
                publish_ack_echo(msg.ack_seq);
            }
            else
            {
                publish_uplink_frame(&msg.frame);
            }
        }
    }
}

/* ==================== 状态指标 ==================== */

int bridge_get_status(char* out, size_t n)
{
    if (out == NULL || n == 0)
    {
        return 0;
    }
    const int len = snprintf(
        out, n,
        "{\"state\":\"%s\",\"up_drop\":%lu,\"dn_drop\":%lu,\"ack_lost\":%lu,\"pub_fail\":%lu}",
        state_name(s_state),
        (unsigned long)s_uplink_dropped,
        (unsigned long)s_downlink_dropped,
        (unsigned long)s_ack_lost,
        (unsigned long)s_pub_failed);
    if (len < 0 || (size_t)len >= n)
    {
        return 0; /* 截断则不上报（下次再来） */
    }
    return len;
}

/** 周期上报 status JSON 到 CMCAR_TOPIC_STATUS（超时发布，不阻塞 timer 任务） */
static void on_status_timer(void* arg)
{
    (void)arg;
    if (!mqtt_service_is_connected())
    {
        return;
    }
    char json[128];
    const int n = bridge_get_status(json, sizeof(json));
    if (n > 0)
    {
        (void)mqtt_service_publish_timed(CMCAR_TOPIC_STATUS, json, (size_t)n, 0, 1000);
    }
}

/* ==================== 系统事件处理 ==================== */

static void on_system_event(void* arg, esp_event_base_t base,
                            int32_t id, void* data)
{
    (void)arg;
    (void)base;
    (void)data;
    switch ((cmcar_event_id_t)id)
    {
        case CMCAR_WIFI_GOT_IP:
            ESP_LOGI(TAG, "WiFi got IP → starting MQTT");
            s_state = SYS_STATE_CONNECTING;
            mqtt_service_start();
            break;

        case CMCAR_MQTT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected → ONLINE");
            s_state = SYS_STATE_ONLINE;
            break;

        case CMCAR_WIFI_DISCONNECTED:
        case CMCAR_MQTT_DISCONNECTED:
            if (s_state == SYS_STATE_ONLINE)
            {
                ESP_LOGW(TAG, "Connection lost → DEGRADED");
                s_state = SYS_STATE_DEGRADED;
            }
            break;

        default:
            break;
    }
}

/* ==================== 生命周期 ==================== */

esp_err_t bridge_init(void)
{
    s_frame_queue = xQueueCreateStatic(
        FRAME_QUEUE_DEPTH, sizeof(bridge_frame_msg_t),
        (uint8_t*)s_frame_queue_storage, &s_frame_queue_struct);

    s_downlink_queue = xQueueCreateStatic(
        DOWNLINK_QUEUE_DEPTH, sizeof(downlink_msg_t),
        (uint8_t*)s_downlink_storage, &s_downlink_queue_struct);

    s_ack_mutex = xSemaphoreCreateMutex();
    ackmgr_init(&s_ackmgr);

    /* ACK 重传扫描定时器（周期 50ms） */
    const esp_timer_create_args_t timer_cfg = {
        .callback = on_retransmit_timer,
        .name = "brg_retx",
    };
    esp_err_t tret = esp_timer_create(&timer_cfg, &s_retransmit_timer);
    if (tret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_timer_create(retx) failed: %s", esp_err_to_name(tret));
        return tret;
    }

    /* 状态上报定时器（周期 30s） */
    const esp_timer_create_args_t status_cfg = {
        .callback = on_status_timer,
        .name = "brg_stat",
    };
    tret = esp_timer_create(&status_cfg, &s_status_timer);
    if (tret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_timer_create(stat) failed: %s", esp_err_to_name(tret));
        return tret;
    }

    uart_service_register_rx_cb(on_uart_frame, NULL);
    mqtt_service_register_rx_cb(on_mqtt_data, NULL);

    esp_event_handler_register(CMCAR_EVENTS, CMCAR_WIFI_GOT_IP,
                               &on_system_event, NULL);
    esp_event_handler_register(CMCAR_EVENTS, CMCAR_MQTT_CONNECTED,
                               &on_system_event, NULL);
    esp_event_handler_register(CMCAR_EVENTS, CMCAR_WIFI_DISCONNECTED,
                               &on_system_event, NULL);
    esp_event_handler_register(CMCAR_EVENTS, CMCAR_MQTT_DISCONNECTED,
                               &on_system_event, NULL);

    ESP_LOGI(TAG, "Bridge initialized");
    return ESP_OK;
}

esp_err_t bridge_start(void)
{
    TaskHandle_t up_handle = xTaskCreateStatic(
        bridge_uplink_task, "bridge_up",
        BRIDGE_UP_STACK / sizeof(StackType_t),
        NULL, TASK_PRIO_BRIDGE_UPLINK, s_uplink_stack, &s_uplink_tcb);

    TaskHandle_t dn_handle = xTaskCreateStatic(
        bridge_downlink_task, "bridge_dn",
        BRIDGE_DOWN_STACK / sizeof(StackType_t),
        NULL, TASK_PRIO_BRIDGE_DOWNLINK, s_downlink_stack, &s_downlink_tcb);

    (void)esp_timer_start_periodic(
        s_retransmit_timer, BRIDGE_RETRANSMIT_PERIOD_MS * 1000ULL);
    (void)esp_timer_start_periodic(
        s_status_timer, BRIDGE_STATUS_PERIOD_MS * 1000ULL);

    ESP_LOGI(TAG, "Bridge started");
    return (up_handle != NULL && dn_handle != NULL) ? ESP_OK : ESP_FAIL;
}

esp_err_t bridge_stop(void)
{
    if (s_retransmit_timer != NULL)
    {
        (void)esp_timer_stop(s_retransmit_timer);
    }
    if (s_status_timer != NULL)
    {
        (void)esp_timer_stop(s_status_timer);
    }
    s_state = SYS_STATE_INIT;
    return ESP_OK;
}
