#include "display.h"
#include "display_config.h"
#include "stm32h7xx_hal.h"
#include <assert.h>
#include <stdio.h>
CoreDebug_Type display_test_debug;
DWT_Type display_test_dwt;
uint32_t SystemCoreClock=250000000;
static uint16_t fb[240*240];
static unsigned calls,rows,fail_call,cs=1;
static uint8_t *seen[240];static uint16_t sizes[240];
HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef *s) { (void)s;return HAL_OK; }
void HAL_GPIO_Init(void *p,GPIO_InitTypeDef *i) { (void)p;(void)i; }
void HAL_GPIO_WritePin(void *p,uint32_t pin,uint32_t value) {
    (void)p;if (pin==DISPLAY_CS_PIN) cs=value;
}
HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *s,uint8_t *data,uint16_t size,uint32_t timeout) {
    (void)s;(void)timeout;assert(cs==0);calls++;
    if (size>1) { assert(rows<240);seen[rows]=data;sizes[rows++]=size; }
    return fail_call==calls ? HAL_ERROR:HAL_OK;
}
static void reset(void) {calls=rows=fail_call=0;cs=1;}
int main(void) {
    reset();assert(!Display_BlitBuffer(NULL,0,0,1,1));
    assert(!Display_BlitBuffer(fb,240,0,1,1));assert(!Display_BlitBuffer(fb,0,240,1,1));
    assert(!Display_BlitBuffer(fb,65535,65535,65535,65535));
    assert(!Display_BlitBuffer(fb,0,0,0,1));assert(calls==0);
    assert(Display_BlitBuffer(fb,239,239,65535,65535));
    assert(rows==1 && sizes[0]==2 && seen[0]==(uint8_t*)&fb[240*240-1] && cs==1);
    reset();assert(Display_BlitBuffer(fb,23,61,3,2));
    assert(rows==2 && sizes[0]==6 && sizes[1]==6);
    assert(seen[0]==(uint8_t*)&fb[61*240+23] && seen[1]==(uint8_t*)&fb[62*240+23]);
    reset();fail_call=12;assert(!Display_BlitBuffer(fb,0,0,20,20));
    assert(rows==1 && calls==12 && cs==1); /* early failure releases bus, no stale rows */
    reset();assert(Display_BlitBuffer(fb,0,0,240,240));assert(rows==240 && cs==1);
    puts("display SPI: clipping, framebuffer stride, bounds and failure/CS release passed");
}
