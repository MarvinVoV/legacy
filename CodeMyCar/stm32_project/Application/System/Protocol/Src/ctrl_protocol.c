/*
 * ctrl_protocol.c
 *
 *  Created on: Feb 6, 2025
 *      Author: marvin
 */
#include "ctrl_protocol.h"

#include <stdbool.h>
#include <string.h>

#include "error_code.h"

/**
 * @brief 检查是否包含指定控制字段
 */
bool isCtrlFieldSet(const uint8_t fields, const CtrlField field)
{
    return (fields & field) == field;
}

bool isCtrlFieldsSet(const uint8_t fields, const uint8_t checkFields)
{
    return (fields & checkFields) == checkFields;
}

/**
 * @brief 验证控制指令完整性
 *
 * 检查项：
 *   - 非空、长度 >= sizeof(ControlCmd)（9 字节显式宽度布局）
 *   - fields 至少含 CTRL_FIELD_MOTION
 *   - mode 在合法枚举范围 [0, MOTION_GOAL]
 *   - 各模式参数范围
 *
 * @param cmd 控制指令字节流
 * @param len 字节流长度
 * @return ERR_SUCCESS 或错误码
 */
int validateControlCmd(const uint8_t* cmd, uint16_t len)
{
    if (cmd == NULL || len < sizeof(ControlCmd))
    {
        return ERR_CMD_INVALID_PARAM;
    }

    const ControlCmd* c = (const ControlCmd*)cmd;

    /* 必须包含运动控制字段 */
    if (!isCtrlFieldSet(c->fields, CTRL_FIELD_MOTION))
    {
        return ERR_CMD_INVALID_PARAM;
    }

    /* 运动模式合法性：仅 0-3 */
    if (c->motion.mode >= (uint8_t)MOTION_MODE_MAX)
    {
        return ERR_MOTION_INVALID_MODE;
    }

    /* 按模式校验参数范围（与 pyserver/ESP32 契约一致） */
    switch (c->motion.mode)
    {
        case MOTION_EMERGENCY_STOP:
            break;

        case MOTION_DIRECT_CONTROL:
        {
            const DirectCtrlParam* d = &c->motion.params.directCtrl;
            if (d->leftRpm < -300 || d->leftRpm > 300 ||
                d->rightRpm < -300 || d->rightRpm > 300 ||
                d->steerAngle < -450 || d->steerAngle > 450)
            {
                return ERR_MOTION_INVALID_PARAM;
            }
            break;
        }

        case MOTION_DIFFERENTIAL:
        {
            const DiffCtrlParam* d = &c->motion.params.diffCtrl;
            if (d->linearVel < -1000 || d->linearVel > 1000 ||
                d->angularVel < -127 || d->angularVel > 127)
            {
                return ERR_MOTION_INVALID_PARAM;
            }
            break;
        }

        case MOTION_DRIVE:
        {
            const DriveCtrlParam* d = &c->motion.params.driveCtrl;
            if (d->linearVel < -1000 || d->linearVel > 1000 ||
                d->steerAngle < -450 || d->steerAngle > 450)
            {
                return ERR_MOTION_INVALID_PARAM;
            }
            break;
        }

        case MOTION_GOAL:
        {
            const GoalCtrlParam* g = &c->motion.params.goalCtrl;
            if (g->turnDdeg < -3600 || g->turnDdeg > 3600 ||
                g->distanceMm < -5000 || g->distanceMm > 5000 ||
                g->speedMmps < 1 || g->speedMmps > 600)
            {
                return ERR_MOTION_INVALID_PARAM;
            }
            break;
        }

        default:
            return ERR_MOTION_INVALID_MODE;
    }

    return ERR_SUCCESS;
}
