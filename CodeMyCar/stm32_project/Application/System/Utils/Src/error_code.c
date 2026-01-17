/*
 * error_code.c
 *
 *  Created on: Apr 7, 2025
 *      Author: marvin
 */


#include "error_code.h"

const char* GetErrorDescription(uint32_t errCode) {
    switch(errCode) {
        case ERR_SUCCESS:        return "Operation succeeded";
        case ERR_INVALID_PARAM: return "Invalid parameter detected";
        case ERR_MOTOR_OVERHEAT:
            return "Motor temperature exceeds safety threshold (Current temp: %.1f℃)";
        // 补充其他错误描述...
        default: return "Unknown error";
    }
}
