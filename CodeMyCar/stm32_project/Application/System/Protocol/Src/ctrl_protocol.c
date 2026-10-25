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
 *   - 非空、长度 >= sizeof(ControlCmd)（13 字节显式宽度布局）
 *   - fields 至少含 CTRL_FIELD_MOTION
 *   - mode 在合法枚举范围 [0, MOTION_MIXED_STEER]
 *   - MIXED 模式下 driveCtrlType 合法
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

    /* 运动模式合法性 */
    if (c->motion.mode > (uint8_t)MOTION_MIXED_STEER)
    {
        return ERR_MOTION_INVALID_MODE;
    }

    /* MIXED 模式下驱动控制类型合法性 */
    if (c->motion.mode == (uint8_t)MOTION_MIXED_STEER)
    {
        const MixedCtrlParam* m = &c->motion.params.mixedCtrl;
        if (m->driveCtrlType > (uint8_t)DRIVE_CTRL_DUTY_CYCLE)
        {
            return ERR_MOTION_INVALID_MODE;
        }
    }

    return ERR_SUCCESS;
}
