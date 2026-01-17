//
// Created by Marvin on 2025/1/15.
//
#include <stdlib.h> // 用于动态内存分配
#include <stdbool.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include "ring_buffer.h"

// 内部函数前置声明
// 获取环形缓冲区的内容，并以字符串形式输出 注：若为不可打印的字符，以'.'代替
static void getFullBufferContentAsString(const RingBuffer_t *rb, char *dataBuffer, uint16_t dataBufferSize);

// 初始化环形缓冲区
bool RingBuffer_Init(RingBuffer_t *rb, uint16_t capacity) {
    // 动态分配缓冲区 注意calloc和malloc的区别
    rb->buffer = (uint8_t *) calloc(capacity, sizeof(uint8_t));
    if (rb->buffer == NULL) {
        return false; // 内存分配失败
    }
    rb->capacity = capacity;
    rb->head = 0;
    rb->tail = 0;
    rb->length = 0;
    return true;
}

// 释放环形缓冲区
void RingBuffer_Free(RingBuffer_t *rb) {
    if (rb && rb->buffer) {
        free(rb->buffer);
        rb->buffer = NULL;
        rb->capacity = 0;
        rb->head = 0;
        rb->tail = 0;
    }
}

// 向环形缓冲区写入数据
bool RingBuffer_Write(RingBuffer_t *rb, const uint8_t *data, uint16_t length) {
    if (RingBuffer_GetFreeSpace(rb) < length) {
        return false; // 缓冲区空间不足
    }

    // 计算目标地址，避免多次取模
    if (rb->head + length > rb->capacity) {
        // 数据跨越缓冲区边界，分段复制
        uint16_t firstPartLength = rb->capacity - rb->head;
        memcpy(&rb->buffer[rb->head], data, firstPartLength);
        memcpy(&rb->buffer[0], data + firstPartLength,
               length - firstPartLength);
    } else {
        // 数据未跨越边界，直接复制
        memcpy(&rb->buffer[rb->head], data, length);
    }

    rb->head = (rb->head + length) % rb->capacity;
    rb->length += length; // 更新当前数据长度
    return true;
}

// 从环形缓冲区读取数据
uint16_t RingBuffer_Read(RingBuffer_t *rb, uint8_t *data, uint16_t length) {
    if (length > rb->length) {
        length = rb->length;
    }
    if (rb->tail + length > rb->capacity) {
        // 数据跨越缓冲区边界，分段复制
        uint16_t firstPartLength = rb->capacity - rb->tail;
        memcpy(data, &rb->buffer[rb->tail], firstPartLength);
        memcpy(data + firstPartLength, rb->buffer, length - firstPartLength);
    } else {
        // 数据未跨越边界，直接复制
        memcpy(data, &rb->buffer[rb->tail], length);
    }
    rb->tail = (rb->tail + length) % rb->capacity;
    rb->length -= length;
    return length;
}

// 获取环形缓冲区的空闲空间
uint16_t RingBuffer_GetFreeSpace(RingBuffer_t *rb) {
    return rb->capacity - rb->length;
}

// 获取环形缓冲区的已用空间
uint16_t RingBuffer_GetUsedSpace(RingBuffer_t *rb) {
    return rb->length;
}

// 检查环形缓冲区是否为空
bool RingBuffer_IsEmpty(RingBuffer_t *rb) {
    return rb->length == 0;
}

// 检查环形缓冲区是否已满
bool RingBuffer_IsFull(RingBuffer_t *rb) {
    return rb->length == rb->capacity;
}

void RingBuffer_digest(RingBuffer_t *rb, char *digestBuffer,
                       uint16_t digestBufferSize, char *dataBuffer, uint16_t dataBufferSize) {
    if (digestBuffer == NULL || digestBufferSize == 0) {
        return; // 或者考虑抛出异常、记录错误等处理方式
    }
    if (dataBuffer == NULL || dataBufferSize == 0) {
        return;
    }
    memset(digestBuffer, 0, digestBufferSize); // 将digest数组内容置空
    memset(dataBuffer, 0, dataBufferSize); // 将digest数组内容置空

    if (rb == NULL) {
        snprintf(digestBuffer, digestBufferSize,
                 "\nInvalid RingBuffer pointer.");
    } else {
        getFullBufferContentAsString(rb, dataBuffer, dataBufferSize);
        snprintf(digestBuffer, digestBufferSize,
                 "\nCapacity: %d, Head: %d, Tail: %d, Length: %d, Full Content: %s\n",
                 rb->capacity, rb->head, rb->tail, rb->length, dataBuffer);
    }

    digestBuffer[digestBufferSize - 1] = '\0'; // 确保字符串以null结尾
}

/**
 * 读取RingBuffer中的整个缓冲区内容，并以字符串形式输出；
 * 注：若为不可打印的字符，以'.'代替
 * @param rb RingBuffer Pointer
 * @param dataBuffer 数据缓冲区，需要确保足够的容量
 * @param dataBufferSize dataBuffer的大小
 */
static void getFullBufferContentAsString(const RingBuffer_t *rb, char *dataBuffer,
                                         uint16_t dataBufferSize) {
    if (dataBufferSize < rb->capacity + 1)
        return; // 需要足够的空间来存放所有字符加上终止符

    for (int i = 0; i < rb->capacity; ++i) {
        // 判断是否为可打印字符，如果不是则用'.'代替
        dataBuffer[i] = isprint(rb->buffer[i]) ? rb->buffer[i] : '.';
    }
    dataBuffer[rb->capacity] = '\0'; // 确保字符串以null字符结尾
}

void RingBuffer_ReadAsString(const RingBuffer_t *rb, char *output, uint16_t outputSize) {
    if (outputSize == 0) return; // 确保有空间至少放一个终止符
    output[0] = '\0'; // 默认情况下为空字符串

    if (rb->length > 0) {
        int lengthToCopy = rb->length;
        if (rb->head >= rb->tail) {
            // 数据未环绕
            strncpy(output, (const char *)(rb->buffer + rb->tail), lengthToCopy);
        } else {
            // 数据环绕
            int firstPartLength = rb->capacity - rb->tail;
            strncpy(output, (const char *)(rb->buffer + rb->tail), firstPartLength);
            strncat(output, (const char *)(rb->buffer), rb->head);
            lengthToCopy = firstPartLength + rb->head;
        }

        // 确保输出以 null 结尾
        if (lengthToCopy < outputSize) {
            output[lengthToCopy] = '\0';
        } else {
            output[outputSize - 1] = '\0'; // 安全截断
        }
    }
}
