#include "imu_i2c_transport.h"
#include "semphr.h"
#include <assert.h>
#include <stdio.h>

static I2C_TypeDef instance={.ISR=0x8000};
I2C_HandleTypeDef hi2c1={.Instance=&instance,.State=0x20};
static uint32_t now;
static bool latchedBusy;
uint32_t HAL_GetTick(void) { return now; }
void vTaskDelay(TickType_t ticks) { now+=ticks; }
unsigned HAL_GPIO_ReadPin(void *port,uint16_t pin)
{ assert(port==GPIOB); return pin==GPIO_PIN_6; }
static int mode, pending, receive, resets, enabled=0;
extern volatile uint32_t g_imuI2cCompletions, g_imuI2cTimeouts, g_imuI2cErrors;
void HAL_I2C_MasterRxCpltCallback(I2C_HandleTypeDef*);
void HAL_I2C_MasterTxCpltCallback(I2C_HandleTypeDef*);
void HAL_I2C_ErrorCallback(I2C_HandleTypeDef*);

SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage)
{ storage->count=0; return storage; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem,TickType_t timeout)
{
    if (timeout && pending && mode < 2) {
        pending=0;
        if (mode==1) HAL_I2C_ErrorCallback(&hi2c1);
        else if (receive) HAL_I2C_MasterRxCpltCallback(&hi2c1);
        else HAL_I2C_MasterTxCpltCallback(&hi2c1);
    }
    if (!sem->count) return pdFALSE;
    --sem->count;
    return pdTRUE;
}
void xSemaphoreGiveFromISR(SemaphoreHandle_t sem,BaseType_t *wake)
{ assert(sem->count==0); sem->count=1; *wake=pdTRUE; }
static HAL_StatusTypeDef start(uint8_t *buffer)
{
    assert(!pending && enabled==3);
    if (mode==3 || latchedBusy) return HAL_BUSY;
    pending=1;hi2c1.pBuffPtr=buffer;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_I2C_Master_Receive_IT(I2C_HandleTypeDef *bus,uint16_t a,uint8_t *b,uint16_t n)
{ (void)a;(void)n;assert(bus==&hi2c1); receive=1;return start(b); }
HAL_StatusTypeDef HAL_I2C_Master_Transmit_IT(I2C_HandleTypeDef *bus,uint16_t a,uint8_t *b,uint16_t n)
{ (void)a;(void)n;assert(bus==&hi2c1); receive=0;return start(b); }
HAL_StatusTypeDef HAL_I2C_DeInit(I2C_HandleTypeDef *bus)
{ assert(bus==&hi2c1 && !enabled);pending=0;latchedBusy=false;++resets;return HAL_OK; }
HAL_StatusTypeDef HAL_I2C_Init(I2C_HandleTypeDef *bus)
{ assert(bus==&hi2c1 && !bus->pBuffPtr && !enabled);return mode==4 ? HAL_ERROR:HAL_OK; }
void HAL_I2C_EV_IRQHandler(I2C_HandleTypeDef *b) { (void)b; }
void HAL_I2C_ER_IRQHandler(I2C_HandleTypeDef *b) { (void)b; }
void HAL_NVIC_SetPriority(int irq,unsigned pre,unsigned sub)
{ (void)irq;assert(pre>=5 && !sub); }
void HAL_NVIC_ClearPendingIRQ(int irq) { (void)irq; }
void HAL_NVIC_EnableIRQ(int irq) { enabled|=irq; }
void HAL_NVIC_DisableIRQ(int irq) { enabled&=~irq; }

int main(void)
{
    uint8_t buffer[32];
    assert(ImuI2c_Transfer(1,0x94,NULL,32,20)==HAL_ERROR);
    assert(ImuI2c_Transfer(1,0x94,buffer,0,20)==HAL_ERROR);
    assert(ImuI2c_Transfer(1,0x94,buffer,32,0)==HAL_ERROR);
    assert(ImuI2c_Transfer(1,0x94,buffer,32,20)==HAL_OK);
    assert(ImuI2c_Transfer(0,0x94,buffer,32,20)==HAL_OK);
    mode=1;
    assert(ImuI2c_Transfer(1,0x94,buffer,32,20)==HAL_ERROR);
    mode=2;
    assert(ImuI2c_Transfer(1,0x94,buffer,32,20)==HAL_TIMEOUT);
    assert(!pending && !hi2c1.pBuffPtr && resets==1 && enabled==3);
    /* Late callback after timeout must not leave a stale semaphore signal. */
    HAL_I2C_MasterRxCpltCallback(&hi2c1);
    mode=0;
    assert(ImuI2c_Transfer(1,0x94,buffer,32,20)==HAL_OK);
    /* A latched startup BUSY is cleared without reissuing the uncertain write. */
    now+=250; latchedBusy=true;
    assert(ImuI2c_Transfer(0,0x94,buffer,32,20)==HAL_BUSY);
    assert(!latchedBusy && resets==2 && !hi2c1.pBuffPtr);
    assert(g_imuI2cLastIsr==0x8000 && g_imuI2cLastState==0x20);
    assert(g_imuI2cLastPins==1 && g_imuI2cBusy==1);
    HAL_I2C_MasterTxCpltCallback(&hi2c1); /* late IRQ must not signal next call */
    assert(ImuI2c_Transfer(0,0x94,buffer,32,20)==HAL_OK);
    /* Permanent BUSY: rate limit reset attempts and yield, never spin/retry. */
    mode=3;
    for (unsigned i=0;i<20;i++)
        assert(ImuI2c_Transfer(1,0x94,buffer,32,20)==HAL_BUSY);
    assert(resets==2 && now>=271);
    now+=250;
    assert(ImuI2c_Transfer(1,0x94,buffer,32,20)==HAL_BUSY);
    assert(resets==3);
    mode=4;
    assert(ImuI2c_Transfer(1,0x94,buffer,32,20)==HAL_ERROR);
    assert(!pending && !hi2c1.pBuffPtr && resets==4);
    assert(g_imuI2cRecoveryFailures==1);
    mode=0;
    assert(ImuI2c_Transfer(1,0x94,buffer,32,20)==HAL_OK);
    assert(g_imuI2cCompletions==5 && g_imuI2cTimeouts==2);
    assert(g_imuI2cBusy==22 && g_imuI2cErrors==23);
    puts("IMU I2C: RX/TX, ISR completion, error, timeout quiescence, late IRQ and recovery PASS");
}
