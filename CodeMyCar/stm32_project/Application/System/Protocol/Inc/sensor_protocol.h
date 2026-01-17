/*
 * sensor_protocol.h
 *
 *  Created on: Feb 6, 2025
 *      Author: marvin
 */

#ifndef SYSTEM_PROTOCOL_INC_SENSOR_PROTOCOL_H_
#define SYSTEM_PROTOCOL_INC_SENSOR_PROTOCOL_H_
#include <stdint.h>

#pragma pack(push, 1)
typedef enum
{
    SENSOR_IMU = 0x01,
    SENSOR_INFRARED,
    SENSOR_ULTRASONIC
} sensor_type_t;

typedef struct
{
    uint32_t timestamp;
    uint8_t type; // @see sensor_type_t
    union
    {
        struct
        {
            // IMU数据
            float accel[3];
            float gyro[3];
        } imu;

        struct
        {
            // 红外传感器
            uint16_t distance;
        } infrared;

        // ...其他传感器结构
    } data;
} sensor_data_t;
#pragma pack(pop)

#endif /* SYSTEM_PROTOCOL_INC_SENSOR_PROTOCOL_H_ */
