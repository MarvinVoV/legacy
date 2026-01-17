/*
 * ctrl_task.c
 *
 *  Created on: Feb 6, 2025
 *      Author: marvin
 */
#include "ctrl_task.h"

#include "cmd_process.h"
#include "error_code.h"
#include "log_manager.h"
#include "pkt_protocol_buf.h"
#include "queue_manager.h"
#include "uart_handle.h"
#include "system_init.h"

// 累积缓冲区大小
#define ACCUMULATE_BUF_SIZE ( 5 * PROTOCOL_MAX_DATA_LEN)

static osThreadId_t rawDataReadTaskHandle = NULL;
// 协议解析处理器
static protocol_receiver receiver;

static void dispatcher(uint8_t type, const uint8_t* cmd_payload, uint16_t total_len);

static void process_raw_data(const uint8_t* raw_data, uint16_t len);


void CtrlTask_Init()
{
    const osThreadAttr_t rawDataReadTaskAttrs = {
        .name = "RawDataReadTask",
        .stack_size = 512 * 4,
        .priority = (osPriority_t)osPriorityNormal
    };
    rawDataReadTaskHandle = osThreadNew(CtrlTask_doTask, NULL, &rawDataReadTaskAttrs);
    if (rawDataReadTaskHandle == NULL)
    {
        LOG_ERROR(LOG_MODULE_SYSTEM, "Create CtrlTask thread error.");
    }

    // 初始化协议接收器
    protocol_receiver_init(&receiver, ACCUMULATE_BUF_SIZE, dispatcher);
    if (receiver.buffer == NULL)
    {
        LOG_ERROR(LOG_MODULE_SYSTEM, "Create protocol receiver error.");
    }
}

void CtrlTask_doTask(void* args)
{
    while (1)
    {
        if (xSemaphoreTake(uart_dma_buffer.semaphore, osWaitForever) == pdTRUE)
        {
            if (uart_dma_buffer.length > 0)
            {
                // 获取非活动缓冲区的索引和长度
                const uint8_t  inactive_buffer = uart_dma_buffer.current ^ 1;
                uint8_t*       data            = uart_dma_buffer.rx_buffer[inactive_buffer];
                const uint16_t len             = uart_dma_buffer.length;
                if (len == 0) continue;

                // 无效化 Cache（H7 的 Cache 行大小为 32 字节）
                SCB_InvalidateDCache_by_Addr(data, len);
                // 处理数据
                process_raw_data(data, len);
            }
        }
    }
}

static void dispatcher(uint8_t type, const uint8_t* cmd_payload, const uint16_t total_len)
{
    const int32_t error_code = validateControlCmd(cmd_payload, total_len);
    if (error_code != ERR_SUCCESS)
    {
        LOG_ERROR(LOG_MODULE_SYSTEM, "ctrl_cmd_t validation failed with error code: %d", error_code);
        return;
    }

    const ControlCmd* cmd = (ControlCmd*)cmd_payload;

    const SystemContext* system_context = System_GetContext();
    if (system_context != NULL)
    {
        CmdProcessorContext* cmd_context = system_context->cmdContext;
        if (cmd_context != NULL)
        {
            CmdProcessor_ProcessCommand(cmd_context, cmd);
        }
    }
}

static void process_raw_data(const uint8_t* raw_data, const uint16_t len)
{
    // 通过累积缓冲区接收并解析数据，将解析的帧数据发送到消息队列
    protocol_receiver_append(&receiver, raw_data, len);
}
