#pragma once

#include "ack_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef struct {
    uint8_t seq;
    uint8_t status;
} ack_echo_t;

/* STOP receipts have reserved capacity even when regular ACKs or telemetry fill up.
 * Both lanes retain FIFO order. A full lane is accounted by the caller. */
typedef struct {
    StaticQueue_t stop_control, normal_control;
    ack_echo_t stop_storage[ACK_MGR_MAX_PENDING];
    ack_echo_t normal_storage[16];
    QueueHandle_t stop, normal;
} bridge_receipts_t;

static inline void bridge_receipts_init(bridge_receipts_t *r)
{
    r->stop = xQueueCreateStatic(ACK_MGR_MAX_PENDING, sizeof(ack_echo_t),
                                 (uint8_t *)r->stop_storage, &r->stop_control);
    r->normal = xQueueCreateStatic(sizeof(r->normal_storage) / sizeof(ack_echo_t), sizeof(ack_echo_t),
                                   (uint8_t *)r->normal_storage, &r->normal_control);
}

static inline bool bridge_receipts_put(bridge_receipts_t *r, bool stop, const ack_echo_t *echo)
{
    return xQueueSend(stop ? r->stop : r->normal, echo, 0) == pdPASS;
}

static inline bool bridge_receipts_take(bridge_receipts_t *r, ack_echo_t *echo)
{
    return xQueueReceive(r->stop, echo, 0) == pdTRUE || xQueueReceive(r->normal, echo, 0) == pdTRUE;
}
