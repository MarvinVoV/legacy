//
// Created by Marvin on 2025/1/15.
//

#ifndef RINGBUFFER_H
#define RINGBUFFER_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
// 定义环形缓冲区大小（可根据需要调整）
#define RING_BUFFER_SIZE 1024

// 环形缓冲区结构体
typedef struct {
    uint8_t *buffer; // 动态分配的缓冲区指针
    uint16_t capacity; // 缓冲区总容量
    volatile uint16_t head; // 写索引
    volatile uint16_t tail; // 读索引
    volatile uint16_t length; // 当前存储的数据长度
} RingBuffer_t;

/**
 * 初始化环形缓冲区
 * @param rb 环形缓冲区指针
 * @param capacity 缓冲区容量
 * @return 是否初始化成功
 * @note 该函数会分配内存并初始化环形缓冲区
 */
bool RingBuffer_Init(RingBuffer_t *rb, uint16_t capacity);

/**
 * 释放环形缓冲区
 * @param rb 环形缓冲区指针
 */
void RingBuffer_Free(RingBuffer_t *rb);

/**
 * 写入数据到环形缓冲区
 * @param rb 环形缓冲区指针
 * @param data 要写入的数据指针
 * @param length 要写入的数据长度
 * @return 是否写入成功
 */
bool RingBuffer_Write(RingBuffer_t *rb, const uint8_t *data, uint16_t length);

/**
 * 从环形缓冲区读取数据
 * @param rb 环形缓冲区指针
 * @param data 要读取的数据指针
 * @param length 要读取的数据长度
 * @return 实际读取的数据长度
 */
uint16_t RingBuffer_Read(RingBuffer_t *rb, uint8_t *data, uint16_t length);

/**
 * 获取环形缓冲区当前可用空间
 * @param rb 环形缓冲区指针
 * @return 可用空间大小
 */
uint16_t RingBuffer_GetFreeSpace(RingBuffer_t *rb);

/**
 * 获取环形缓冲区当前已用空间
 * @param rb 环形缓冲区指针
 * @return 已用空间大小
 * */
uint16_t RingBuffer_GetUsedSpace(RingBuffer_t *rb);

/**
 * 检查环形缓冲区是否为空
 * @param rb 环形缓冲区指针
 * @return 是否为空
 */
bool RingBuffer_IsEmpty(RingBuffer_t *rb);

/**
 * 检查环形缓冲区是否为满
 * @param rb 环形缓冲区指针
 * @return 是否为满
 */
bool RingBuffer_IsFull(RingBuffer_t *rb);


/**
 * 获取环形缓冲区的整个缓冲区内容，并以字符串形式输出
 * @param rb 环形缓冲区指针
 * @param digestBuffer 用于存储输出的字符串的缓冲区指针
 * @param digestBufferSize digestBuffer的大小
 * @param dataBuffer 用于存储环形缓冲区的缓冲区指针
 * @param dataBufferSize dataBuffer的大小
 * @note digestBufferSize和dataBufferSize必须大于等于RING_BUFFER_SIZE
 * @note digestBuffer和dataBuffer不能重叠
 * */
void RingBuffer_digest(RingBuffer_t *rb, char *digestBuffer, uint16_t digestBufferSize, char *dataBuffer,
                       uint16_t dataBufferSize);


/**
 * 获取环形缓冲区的整个缓冲区内容
 * @param rb 环形缓冲区指针
 * @param output 用于存储输出的字符串的缓冲区指针
 * @param outputSize output的大小
 * @note outputSize必须大于等于RING_BUFFER_SIZE
 * */
void RingBuffer_ReadAsString(const RingBuffer_t *rb, char *output, uint16_t outputSize);


#endif //RINGBUFFER_H
