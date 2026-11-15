/* Executes production uart_service/ack_manager with deterministic FreeRTOS
 * queue and mutex doubles. Interleavings are injected between dequeue/write
 * and retry-copy/enqueue, rather than duplicating the epoch algorithm. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mock_idf.h"
#include "../../components/uart_service/src/uart_service.c"
#include "ack_manager.h"
#include "../../components/bridge/src/bridge_receipts.h"

static unsigned writes;
static uint8_t written[32];
QueueHandle_t xQueueCreateStatic(unsigned n,unsigned size,uint8_t* data,StaticQueue_t* q)
{ *q=(StaticQueue_t){.depth=n,.size=size,.storage=data}; return q; }
int xQueueReceive(QueueHandle_t q,void* out,uint32_t wait)
{
    (void)wait;
    if(!q->count) return 0;
    memcpy(out,q->storage,q->size);
    memmove(q->storage,q->storage+q->size,--q->count*q->size);
    return pdTRUE;
}
int xQueueSend(QueueHandle_t q,const void* in,uint32_t wait)
{
    assert(wait==0);
    if(q->count==q->depth) return 0;
    memcpy(q->storage+q->count++*q->size,in,q->size); return pdPASS;
}
int xQueueSendToFront(QueueHandle_t q,const void* in,uint32_t wait)
{
    assert(wait==0);
    if(q->count==q->depth) return 0;
    memmove(q->storage+q->size,q->storage,q->count++*q->size);
    memcpy(q->storage,in,q->size); return pdPASS;
}
int xQueueReset(QueueHandle_t q) { q->count=0; return pdPASS; }
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* s)
{ s->held=false; return s; }
int xSemaphoreTake(SemaphoreHandle_t s,uint32_t wait)
{ (void)wait; if(s->held) return 0; s->held=true; return pdTRUE; }
int xSemaphoreGive(SemaphoreHandle_t s)
{ assert(s->held); s->held=false; return pdTRUE; }
int uart_write_bytes(int port,const void* data,size_t n)
{ (void)port; assert(s_tx_gate->held); assert(writes<32); written[writes++]=*(const uint8_t*)data; return (int)n; }
static void drain(void)
{ uart_tx_msg_t m; while(xQueueReceive(s_tx_queue,&m,0)) uart_tx_write(&m); }
static unsigned retryCount;
static bool retryPriority;
static void retry(const uint8_t* frame,size_t len,bool priority,void* ctx)
{ (void)ctx;assert(len==1 && *frame==0);retryCount++;retryPriority=priority; }

static uint8_t input[RX_BUF_SIZE];
static size_t available, cursor;
static unsigned parsed;

int uart_read_bytes(int port, void *out, size_t want, uint32_t ticks)
{
    assert(port == UART_PORT && ticks == 0);
    size_t n = available - cursor;
    if (n > want) n = want;
    memcpy(out, input + cursor, n);
    cursor += n;
    return (int)n;
}

static void observe(const pkt_frame_t *frame, void *ctx)
{
    (void)ctx;
    assert(frame->type == PKT_TYPE_ACK && frame->seq == 17);
    parsed++;
}

static void test_rx_and_receipts(void)
{
    pkt_frame_t frame = {.type=PKT_TYPE_ACK,.seq=17,.payload_len=1};
    uint8_t wire[PKT_MAX_FRAME_SIZE];
    size_t n;
    assert(pkt_encode(&frame, wire, sizeof(wire), &n) == ESP_OK);
    s_rx_cb = observe;
    pkt_parser_init(&s_parser);
    for (unsigned i=0; i<50; i++) memcpy(input+i*n, wire, n);
    available = 3; drain_rx(); assert(parsed == 0);
    available = n; drain_rx(); assert(parsed == 1); /* no second UART event needed */
    available = 50*n; drain_rx(); assert(parsed == 50);
    drain_rx(); assert(parsed == 50); /* stale notification */

    bridge_receipts_t receipts;
    bridge_receipts_init(&receipts);
    ack_echo_t echo = {0};
    for (unsigned i=0;i<16;i++) {
        echo.seq=i;
        assert(bridge_receipts_put(&receipts,false,&echo));
    }
    assert(!bridge_receipts_put(&receipts,false,&echo));
    echo.seq=99; assert(bridge_receipts_put(&receipts,true,&echo));
    assert(bridge_receipts_take(&receipts,&echo) && echo.seq==99);
    for (unsigned i=0;i<16;i++) assert(bridge_receipts_take(&receipts,&echo) && echo.seq==i);
}

int main(void)
{
    test_rx_and_receipts();
    s_tx_queue=xQueueCreateStatic(TX_QUEUE_DEPTH,sizeof(uart_tx_msg_t),
        (uint8_t*)s_tx_queue_storage,&s_tx_queue_struct);
    s_tx_gate=xSemaphoreCreateMutexStatic(&s_tx_gate_storage);
    atomic_init(&s_tx_epoch,1u);
    const uint8_t move=1, stop=0, session=2;
    uint32_t old=uart_service_tx_epoch();
    /* Saturated queue must still accept STOP and discard every old move. */
    for(unsigned i=0;i<TX_QUEUE_DEPTH;i++) assert(uart_service_send(&move,1)==ESP_OK);
    assert(uart_service_send(&move,1)==CMCAR_ERR_QUEUE_FULL);
    assert(uart_service_send_stop(&stop,1)==ESP_OK);
    drain(); assert(writes==1 && written[0]==stop);
    /* TX already popped old move, STOP overtakes before driver write. */
    assert(uart_service_send(&move,1)==ESP_OK);
    uart_tx_msg_t popped;
    assert(xQueueReceive(s_tx_queue,&popped,0));
    assert(uart_service_send_stop(&stop,1)==ESP_OK);
    uart_tx_write(&popped); drain();
    assert(writes==2 && written[1]==stop);
    /* Retry copied under ACK lock before STOP cannot reenter afterward. */
    assert(uart_service_send_at_epoch(&move,1,old)==ESP_ERR_INVALID_STATE);
    drain(); assert(writes==2);
    /* Already written bytes stay before STOP; legitimate post-STOP move works. */
    assert(uart_service_send(&move,1)==ESP_OK); drain();
    assert(uart_service_send_stop(&stop,1)==ESP_OK);
    assert(uart_service_send(&move,1)==ESP_OK); drain();
    assert(writes==5 && written[2]==move && written[3]==stop && written[4]==move);
    /* SESSION priority does not cancel a legitimate motion queue. */
    old=uart_service_tx_epoch();
    assert(uart_service_send(&move,1)==ESP_OK);
    assert(uart_service_send_priority(&session,1)==ESP_OK); drain();
    assert(uart_service_tx_epoch()==old);
    assert(writes==7 && written[5]==session && written[6]==move);
    /* Failed gate acquisition reports failure, does not fabricate STOP success. */
    s_tx_gate->held=true;
    assert(uart_service_send_stop(&stop,1)==ESP_ERR_TIMEOUT);
    assert(uart_service_tx_epoch()==old);
    s_tx_gate->held=false;
    ack_manager_t ack;
    ackmgr_init(&ack);
    assert(ackmgr_add(&ack,11,&move,1,0));
    assert(ackmgr_add_priority(&ack,12,&stop,1,0));
    ackmgr_cancel_motion(&ack);
    assert(ackmgr_is_free(&ack,11) && !ackmgr_is_free(&ack,12));
    assert(ackmgr_scan(&ack,201,200,3,retry,NULL)==0);
    assert(retryCount==1 && retryPriority);
    ackmgr_cancel_motion(NULL);
    puts("STOP barrier: queue-full, popped-frame, stale-retry, ordering, new-motion, SESSION, failure and ACK cancellation passed");
}
