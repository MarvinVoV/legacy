//
// Created by marvin on 2025/3/9.
//

#ifndef MQTT_UTILS_H
#define MQTT_UTILS_H
#include <stdbool.h>

bool mqtt_topic_match(const char *subscribed, const char *actual);
#endif //MQTT_UTILS_H
