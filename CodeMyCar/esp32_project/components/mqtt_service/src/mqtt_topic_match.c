/**
 * @file mqtt_topic_match.c
 * @brief MQTT topic 通配符匹配（支持 + 和 #）
 */

#include <string.h>

/**
 * @brief 匹配 MQTT topic（支持 + 单级通配、# 多级通配）
 * @param pattern 订阅模式（如 "cmd/esp32/+/exec"）
 * @param topic   实际 topic（如 "cmd/esp32/DEV_1/exec"）
 * @return true 匹配成功
 */
bool mqtt_topic_match(const char* pattern, const char* topic)
{
    while (*pattern && *topic)
    {
        if (*pattern == '#')
        {
            return true; /* # 匹配所有剩余 */
        }

        if (*pattern == '+')
        {
            /* + 匹配一个层级 */
            while (*topic && *topic != '/')
            {
                topic++;
            }
            pattern++;
            continue;
        }

        if (*pattern != *topic)
        {
            return false;
        }

        pattern++;
        topic++;
    }

    /* 处理尾部 # */
    while (*pattern == '/' || *pattern == '#')
    {
        if (*pattern == '#')
        {
            return true;
        }
        pattern++;
    }

    return (*pattern == '\0' && *topic == '\0');
}
