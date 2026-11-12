#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "host.h"
#include "battery_service.h"
#include "power_config.h"
#include "power_task.h"

static jmp_buf finished;
static void (*entry)(void *);
static uint32_t tick, gpio=GPIO_PIN_10|GPIO_PIN_11;
static uint16_t config;
static bool absent, stuck;
static unsigned initCount, probeCount, recoveryClocks;
static uint8_t frame[29];
unsigned fakeMux;

uint32_t HAL_RCC_GetPCLK1Freq(void) { return 125000000u; }
HAL_StatusTypeDef HAL_I2C_DeInit(I2C_HandleTypeDef *b) { assert(b->Instance==I2C2); return HAL_OK; }
HAL_StatusTypeDef HAL_I2C_Init(I2C_HandleTypeDef *b) {
    uint32_t timing=b->Init.Timing;
    uint32_t ns=8u*((timing>>28)+1u); /* 125MHz PCLK1 */
    assert((((timing>>20)&15u)+1u)*ns>=1100u); /* t_rise + t_setup */
    assert(((timing>>16)&15u)*ns>=400u); /* data hold margin */
    assert(((timing&255u)+1u)*ns>=4700u); /* standard-mode low */
    assert((((timing>>8)&255u)+1u)*ns>=4000u); /* standard-mode high */
    ++initCount; return HAL_OK;
}
HAL_StatusTypeDef HAL_I2CEx_ConfigAnalogFilter(I2C_HandleTypeDef *b,int f) { (void)b;(void)f;return HAL_OK; }
HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *b,uint16_t addr,uint32_t tries,uint32_t timeout) {
    (void)b; assert(tries==1 && timeout==POWER_I2C_TIMEOUT_MS); ++probeCount;
    return !absent && addr==(0x44u<<1) ? HAL_OK : HAL_ERROR;
}
HAL_StatusTypeDef HAL_I2C_Mem_Read(void *b,uint16_t addr,uint16_t reg,uint16_t size,uint8_t *out,uint16_t len,uint32_t timeout) {
    (void)b;assert(addr==(0x44u<<1) && size==1 && len==2 && timeout==POWER_I2C_TIMEOUT_MS);
    uint16_t value=reg==0?config:reg==2?((3000u<<3)|2u):1200u;
    out[0]=(uint8_t)(value>>8);out[1]=(uint8_t)value;return HAL_OK;
}
HAL_StatusTypeDef HAL_I2C_Mem_Write(void *b,uint16_t addr,uint16_t reg,uint16_t size,uint8_t *data,uint16_t len,uint32_t timeout) {
    (void)b; assert(addr==(0x44u<<1) && reg==0 && size==1 && len==2 && timeout==POWER_I2C_TIMEOUT_MS);
    config=(uint16_t)((data[0]<<8)|data[1]); return HAL_OK;
}
uint32_t HAL_I2C_GetError(I2C_HandleTypeDef *b) { (void)b;return 0; }
void HAL_GPIO_Init(void *p,GPIO_InitTypeDef *g) { assert(p==GPIOB && g->Mode==GPIO_MODE_OUTPUT_OD); }
void HAL_GPIO_WritePin(void *p,uint32_t pins,int value) {
    assert(p==GPIOB);
    if (value) gpio|=pins; else gpio&=~pins;
    if ((pins&GPIO_PIN_10) && value) ++recoveryClocks;
}
int HAL_GPIO_ReadPin(void *p,uint32_t pin) { assert(p==GPIOB);return stuck?0:!!(gpio&pin); }
uint32_t HAL_GetTick(void) { return tick; }
uint32_t osKernelGetTickCount(void) { return tick; }
void osDelay(uint32_t n) { tick+=n; }
void osDelayUntil(uint32_t n) { (void)n;longjmp(finished,1); }
void *osThreadNew(void (*fn)(void *),void *arg,const osThreadAttr_t *attrs) {
    (void)arg;assert(attrs->priority==osPriorityBelowNormal && attrs->stack_size==1536);entry=fn;return (void *)1;
}
int UART_Send_Frame(uint8_t type,uint8_t seq,uint8_t flags,const uint8_t *data,uint16_t len) {
    (void)seq;assert(type==PROTOCOL_TYPE_SENSOR && flags==0 && len==29);memcpy(frame,data,len);return 0;
}
static void run_once(void) {
    tick=1000;initCount=probeCount=recoveryClocks=0;memset(frame,0,sizeof(frame));
    PowerTask_Init();assert(entry);
    if (!setjmp(finished)) entry(NULL);
}
int main(void) {
    run_once();
    assert(initCount==1 && probeCount==16 && frame[4]==5 && frame[24]==0x44);
    assert(frame[5]==0xe0 && frame[6]==0x2e && frame[23]==0);
    assert(frame[22]==(CMCAR_POWER_SHUNT_MILLIOHM ? 7 : 5));
    assert(frame[25]==CMCAR_POWER_SHUNT_MILLIOHM && frame[26]==0);
    assert(frame[13]==(CMCAR_POWER_SHUNT_MILLIOHM ? 120 : 0));
    assert(config==0x3e63); /* real adapter runs a triggered conversion */
    absent=true;run_once();assert(frame[22]==0 && frame[23]==1);
    absent=false;fakeMux=1;run_once();assert(initCount==0 && probeCount==0 && frame[22]==0);
    fakeMux=0;stuck=true;run_once();assert(initCount==0 && recoveryClocks<=11 && frame[22]==0);
    puts("power task: PCLK1 init, all-address probe, BE transport, real wire frame, absent device and bounded stuck-bus recovery passed");
}
