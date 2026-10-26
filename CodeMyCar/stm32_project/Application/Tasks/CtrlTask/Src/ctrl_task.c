/*
 * ctrl_task.c
 *
 *  Created on: Feb 6, 2025
 *      Author: marvin
 */
#include "ctrl_task.h"

#include <stdbool.h>

#include "cmd_process.h"
#include "error_code.h"
#include "log_manager.h"
#include "pkt_protocol_buf.h"
#include "queue_manager.h"
#include "seq_dedup.h"
#include "uart_handle.h"
#include "system_init.h"

// 累积缓冲区大小
#define ACCUMULATE_BUF_SIZE ( 5 * PROTOCOL_MAX_DATA_LEN)

static osThreadId_t rawDataReadTaskHandle = NULL;
// 协议解析处理器
static protocol_receiver receiver;

/* mode 4 去重窗口：bridge 重传时只重发 ACK，不重复执行 goal */
static seq_dedup_t s_goal_seqs;

static void dispatcher(uint8_t type, uint8_t seq, uint8_t flags,
                       const uint8_t* cmd_payload, uint16_t total_len);

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
    if (receiver.callback == NULL)
    {
        LOG_ERROR(LOG_MODULE_SYSTEM, "Create protocol receiver error.");
    }

    seq_dedup_init(&s_goal_seqs);
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

static uint8_t ack_status_from_error(const int32_t error_code)
{
    switch (error_code)
    {
        case ERR_SUCCESS:               return 0;
        case ERR_MOTION_INVALID_MODE:   return 1;
        case ERR_MOTION_INVALID_PARAM:
        case ERR_CMD_INVALID_PARAM:     return 2;
        case ERR_MOTION_GEOMETRY:       return 3;
        case ERR_MOTION_BUSY:           return 4;
        default:                        return 5;
    }
}

static void send_ack(const uint8_t seq, const uint8_t status)
{
    const uint8_t payload = status;
    UART_Send_Frame(PROTOCOL_TYPE_ACK, seq, FRAME_FLAG_ACK_RSP, &payload, 1);
}

static void dispatcher(uint8_t type, uint8_t seq, uint8_t flags,
                       const uint8_t* cmd_payload, const uint16_t total_len)
{
    const bool ack_req = (flags & FRAME_FLAG_ACK_REQ) != 0;

    /* 非控制帧：只需确认收到 */
    if (type != PROTOCOL_TYPE_CONTROL)
    {
        if (ack_req)
        {
            send_ack(seq, 0);
        }
        return;
    }

    int32_t error_code = validateControlCmd(cmd_payload, total_len);
    bool    duplicate  = false;

    if (error_code == ERR_SUCCESS)
    {
        const ControlCmd*    cmd            = (const ControlCmd*)cmd_payload;
        const SystemContext* system_context = System_GetContext();

        if (cmd->motion.mode == MOTION_GOAL && seq_dedup_seen(&s_goal_seqs, seq))
        {
            duplicate = true; /* 重传：只重发 ACK，不重复执行 */
        }
        else if (system_context != NULL && system_context->cmdContext != NULL)
        {
            error_code = CmdProcessor_ProcessCommand(system_context->cmdContext, cmd, seq);
            if (error_code == ERR_SUCCESS && cmd->motion.mode == MOTION_GOAL)
            {
                seq_dedup_record(&s_goal_seqs, seq);
            }
        }
        else
        {
            error_code = ERR_CMD_EXECUTION_FAIL;
        }
    }

    if (error_code != ERR_SUCCESS && !duplicate)
    {
        LOG_ERROR(LOG_MODULE_SYSTEM, "ctrl_cmd_t failed: err=0x%08X", (unsigned)error_code);
    }

    if (ack_req)
    {
        /* ACK 语义：帧已收到 + 命令是否被接受（0 = 接受） */
        send_ack(seq, duplicate ? 0 : ack_status_from_error(error_code));
    }
}

static void process_raw_data(const uint8_t* raw_data, const uint16_t len)
{
    // 通过累积缓冲区接收并解析数据，将解析的帧数据发送到消息队列
    protocol_receiver_append(&receiver, raw_data, len);
}
