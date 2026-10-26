/**
 * @file bridge_codec.c
 * @brief JSON ↔ Binary 转换实现（见 docs/cmd-mapping.md）
 *
 * 下行用 cJSON 解析 JSON；上行用 snprintf 构建 JSON。
 * ESP32-C6 无硬件 FPU：sensor 浮点字段以定点整数（×1000）编码，避免 %f。
 */

#include "bridge_codec.h"

#include <string.h>
#include <stdio.h>

#include "cJSON.h"
#include "pkt_codec.h"

/* ==================== STM32 二进制布局（与 Application/System/Protocol 镜像） ==================== */

/* sensor_data_t（#pragma pack(1)）：timestamp(u32) | type(u8) | union(24) */
#define SENSOR_TYPE_IMU       1
#define SENSOR_TYPE_INFRARED  2
#define SENSOR_TYPE_ULTRASONIC 3
#define SENSOR_HEADER_SIZE    5 /* timestamp(4) + type(1) */

/* log_entry_t（#pragma pack(1)）：timestamp(u32) | module(u16) | level(u8) | message[] */
#define LOG_HEADER_SIZE 7
#define LOG_LEVEL_DEBUG    0x01
#define LOG_LEVEL_INFO     0x02
#define LOG_LEVEL_WARNING  0x04
#define LOG_LEVEL_ERROR    0x08
#define LOG_LEVEL_CRITICAL 0x10

#define CTRL_FIELD_MOTION 0x01

/* ==================== 辅助 ==================== */

static void put_u16le(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put_i16le(uint8_t* p, int16_t v)
{
    put_u16le(p, (uint16_t)v);
}

/** 从 cJSON 对象取可选整数，缺失返回 def */
static int get_int(const cJSON* obj, const char* key, int def)
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item != NULL && cJSON_IsNumber(item))
    {
        return item->valueint;
    }
    return def;
}

/* ==================== 下行：ping/pong 链路自检 ==================== */

bool bridge_json_is_ping(const char* json, size_t len, uint8_t* out_seq)
{
    if (json == NULL || out_seq == NULL)
    {
        return false;
    }
    cJSON* root = cJSON_ParseWithLength(json, len);
    if (root == NULL)
    {
        return false;
    }
    bool is_ping = false;
    const cJSON* cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (cmd != NULL && cJSON_IsString(cmd) && strcmp(cmd->valuestring, "ping") == 0)
    {
        *out_seq = (uint8_t)(get_int(root, "seq", 0) & 0xFF);
        is_ping = true;
    }
    cJSON_Delete(root);
    return is_ping;
}

int bridge_pong_to_json(uint8_t seq, unsigned long long uptime_s,
                        unsigned long heap, char* out, size_t out_size)
{
    if (out == NULL || out_size == 0)
    {
        return 0;
    }
    const int n = snprintf(out, out_size,
                           "{\"seq\":%u,\"pong\":true,\"uptime_s\":%llu,\"heap\":%lu}",
                           (unsigned)seq, uptime_s, heap);
    if (n <= 0 || (size_t)n >= out_size)
    {
        return 0;
    }
    return n;
}

/* ==================== 下行：JSON → ControlCmd ==================== */

int bridge_json_to_control_cmd(const char* json, size_t len,
                               uint8_t* out, uint8_t* out_seq, bool* out_ack)
{
    if (json == NULL || out == NULL || out_seq == NULL || out_ack == NULL)
    {
        return 0;
    }

    cJSON* root = cJSON_ParseWithLength(json, len);
    if (root == NULL)
    {
        return 0;
    }

    int result = 0;
    do
    {
        /* cmd 必须是 "motion" */
        const cJSON* cmd_item = cJSON_GetObjectItemCaseSensitive(root, "cmd");
        if (cmd_item != NULL && cJSON_IsString(cmd_item) &&
            strcmp(cmd_item->valuestring, "motion") != 0)
        {
            break; /* 非 motion 命令，不支持 */
        }

        /* seq → 帧头 seq（用于 ACK） */
        *out_seq = (uint8_t)(get_int(root, "seq", 0) & 0xFF);
        *out_ack = true; /* 运动命令默认请求 ACK */

        const cJSON* motion = cJSON_GetObjectItemCaseSensitive(root, "motion");
        if (motion == NULL || !cJSON_IsObject(motion))
        {
            break;
        }

        const int mode = get_int(motion, "mode", -1);
        if (mode < BRIDGE_MODE_EMERGENCY_STOP || mode >= BRIDGE_MODE_MAX)
        {
            break; /* 只支持 0-3 */
        }

        const cJSON* params = cJSON_GetObjectItemCaseSensitive(motion, "params");

        /* 填充 9 字节 ControlCmd */
        memset(out, 0, BRIDGE_CONTROL_CMD_SIZE);
        out[0] = 0;                  /* ctrlId */
        out[1] = CTRL_FIELD_MOTION;  /* fields */
        out[2] = (uint8_t)mode;      /* mode */
        /* out[3..8] = params union（6 字节，按 mode 填） */

        switch (mode)
        {
            case BRIDGE_MODE_EMERGENCY_STOP:
                /* 无参数 */
                break;

            case BRIDGE_MODE_DIRECT: /* DirectCtrlParam: leftRpm, rightRpm, steerAngle */
                if (params == NULL) { break; }
                put_i16le(&out[3], (int16_t)get_int(params, "left_rpm", 0));
                put_i16le(&out[5], (int16_t)get_int(params, "right_rpm", 0));
                put_i16le(&out[7], (int16_t)get_int(params, "steer_angle", 0));
                break;

            case BRIDGE_MODE_DIFFERENTIAL: /* DiffCtrlParam: linearVel, angularVel */
                if (params == NULL) { break; }
                put_i16le(&out[3], (int16_t)get_int(params, "linear_vel", 0));
                put_i16le(&out[5], (int16_t)get_int(params, "angular_vel", 0));
                break;

            case BRIDGE_MODE_DRIVE: /* DriveCtrlParam: linearVel, steerAngle */
                if (params == NULL) { break; }
                put_i16le(&out[3], (int16_t)get_int(params, "linear_vel", 0));
                put_i16le(&out[5], (int16_t)get_int(params, "steer_angle", 0));
                break;

            default:
                break;
        }

        result = BRIDGE_CONTROL_CMD_SIZE;
    } while (0);

    cJSON_Delete(root);
    return result;
}

/* ==================== 上行：二进制 → JSON ==================== */

static const char* log_level_str(uint8_t level)
{
    if (level & LOG_LEVEL_CRITICAL) return "CRITICAL";
    if (level & LOG_LEVEL_ERROR)    return "ERROR";
    if (level & LOG_LEVEL_WARNING)  return "WARNING";
    if (level & LOG_LEVEL_INFO)     return "INFO";
    if (level & LOG_LEVEL_DEBUG)    return "DEBUG";
    return "INFO";
}

static const char* log_module_str(uint16_t module)
{
    if (module & 0x0001) return "system";
    if (module & 0x0002) return "motor";
    if (module & 0x0004) return "sensor";
    if (module & 0x0008) return "network";
    if (module & 0x0010) return "driver";
    if (module & 0x0020) return "servo";
    if (module & 0x0040) return "motion";
    if (module & 0x0080) return "cmd";
    return "unknown";
}

static uint32_t rd_u32le(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16le(const uint8_t* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

int bridge_sensor_to_json(const uint8_t* payload, uint8_t len,
                          char* out, size_t out_size)
{
    if (payload == NULL || out == NULL || len < SENSOR_HEADER_SIZE)
    {
        return 0;
    }

    const uint32_t ts = rd_u32le(payload);
    const uint8_t type = payload[4];
    const uint8_t* data = &payload[5];
    const uint8_t data_len = (uint8_t)(len - SENSOR_HEADER_SIZE);

    int n = 0;
    switch (type)
    {
        case SENSOR_TYPE_IMU:
        {
            if (data_len < 24) { return 0; } /* 6 floats */
            float f[6];
            memcpy(f, data, 24);
            /* 浮点 → 定点整数（×1000），避免 C6 无 FPU 下的 %f 开销 */
            int32_t a0 = (int32_t)(f[0] * 1000.0f), a1 = (int32_t)(f[1] * 1000.0f),
                    a2 = (int32_t)(f[2] * 1000.0f), g0 = (int32_t)(f[3] * 1000.0f),
                    g1 = (int32_t)(f[4] * 1000.0f), g2 = (int32_t)(f[5] * 1000.0f);
            n = snprintf(out, out_size,
                         "{\"type\":\"imu\",\"timestamp\":%lu,"
                         "\"data\":{\"accel\":[%ld,%ld,%ld],\"gyro\":[%ld,%ld,%ld],"
                         "\"units\":{\"accel\":\"0.001 m/s2\",\"gyro\":\"0.001 rad/s\"}}}",
                         (unsigned long)ts,
                         (long)a0, (long)a1, (long)a2,
                         (long)g0, (long)g1, (long)g2);
            break;
        }
        case SENSOR_TYPE_INFRARED:
        case SENSOR_TYPE_ULTRASONIC:
        {
            if (data_len < 2) { return 0; }
            const uint16_t distance = rd_u16le(data);
            n = snprintf(out, out_size,
                         "{\"type\":\"%s\",\"timestamp\":%lu,\"data\":{\"distance\":%u}}",
                         (type == SENSOR_TYPE_INFRARED) ? "infrared" : "ultrasonic",
                         (unsigned long)ts, distance);
            break;
        }
        default:
            n = snprintf(out, out_size,
                         "{\"type\":\"unknown\",\"timestamp\":%lu,\"data\":{}}",
                         (unsigned long)ts);
            break;
    }

    if (n < 0 || (size_t)n >= out_size)
    {
        return 0; /* 截断 */
    }
    return n;
}

int bridge_log_to_json(const uint8_t* payload, uint8_t len,
                       char* out, size_t out_size)
{
    if (payload == NULL || out == NULL || out_size == 0 || len < LOG_HEADER_SIZE)
    {
        return 0;
    }

    const uint32_t ts = rd_u32le(payload);
    const uint16_t module = rd_u16le(&payload[4]);
    const uint8_t level = payload[6];
    const char* msg = (const char*)&payload[7];
    const size_t msg_len = (size_t)len - LOG_HEADER_SIZE;

    int off = snprintf(out, out_size,
                       "{\"level\":\"%s\",\"tag\":\"%s\",\"message\":\"",
                       log_level_str(level), log_module_str(module));
    if (off < 0)
    {
        return 0;
    }
    if ((size_t)off >= out_size)
    {
        off = (int)out_size - 1; /* out 极小，前缀就截断 */
    }

    /* 逐字符正规 JSON 转义（" → \"、\ → \\、控制字符 → 空格）。
       预留尾部 ","timestamp":<u32>}` 约 24 字节，超长则截断 message 但仍发合法 JSON。 */
    const int tail_reserve = 24;
    int cap = (int)out_size - tail_reserve;
    if (cap < off)
    {
        cap = off; /* 已无空间容纳 message */
    }
    for (size_t i = 0; i < msg_len && off < cap; i++)
    {
        const unsigned char c = (unsigned char)msg[i];
        if (c == '"' || c == '\\')
        {
            if (off + 2 <= cap)
            {
                out[off++] = '\\';
                out[off++] = (char)c;
            }
            else
            {
                break;
            }
        }
        else if (c < 0x20)
        {
            out[off++] = ' ';
        }
        else
        {
            out[off++] = (char)c;
        }
    }

    const int tail = snprintf(out + off, out_size - (size_t)off,
                              "\",\"timestamp\":%lu}", (unsigned long)ts);
    if (tail < 0)
    {
        return 0;
    }
    off += tail;
    if ((size_t)off >= out_size)
    {
        off = (int)out_size - 1;
    }
    out[off] = '\0';
    return off;
}
