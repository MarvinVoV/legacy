//
// Created by marvin on 2025/3/9.
//
#include <string.h>
#include <stdbool.h>

#define MAX_TOPIC_LAYERS 8     // 最大主题层级数
#define MAX_LAYER_LENGTH 64    // 单层最大长度

// 分割主题字符串到层级数组，返回层级数
static int split_topic(const char *topic, char layers[][MAX_LAYER_LENGTH]) {
    int count = 0;
    const char *start = topic;
    const char *end;

    while (*start != '\0' && count < MAX_TOPIC_LAYERS) {
        // 跳过连续的'/'
        while (*start == '/') start++;
        if (*start == '\0') break;

        // 查找下一个分隔符
        end = strchr(start, '/');
        if (!end) end = start + strlen(start);

        // 提取当前层级
        size_t len = end - start;
        if (len >= MAX_LAYER_LENGTH) return -1; // 层级过长

        strncpy(layers[count], start, len);
        layers[count][len] = '\0';
        count++;

        start = end; // 继续处理下一层级
    }
    return count;
}

// MQTT 主题匹配函数
bool mqtt_topic_match(const char *subscribed, const char *actual) {
    char sub_layers[MAX_TOPIC_LAYERS][MAX_LAYER_LENGTH];
    char act_layers[MAX_TOPIC_LAYERS][MAX_LAYER_LENGTH];

    int sub_cnt = split_topic(subscribed, sub_layers);
    int act_cnt = split_topic(actual, act_layers);

    if (sub_cnt < 0 || act_cnt < 0) return false;

    int s = 0, a = 0;
    while (s < sub_cnt && a < act_cnt) {
        // 处理通配符 #
        if (strcmp(sub_layers[s], "#") == 0) {
            return (s == sub_cnt - 1); // # 必须是最后一层
        }

        // 处理通配符 +
        if (strcmp(sub_layers[s], "+") == 0 ||
            strcmp(sub_layers[s], act_layers[a]) == 0) {
            s++;
            a++;
        } else {
            return false;
        }
    }

    // 检查剩余层级
    if (s == sub_cnt && a == act_cnt) return true;       // 完全匹配
    if (s == sub_cnt - 1 && strcmp(sub_layers[s], "#") == 0) return true;
    return false;
}