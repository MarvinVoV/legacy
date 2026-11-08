#include <assert.h>
#include <stdio.h>
#include "motor_hal.h"

uint32_t HAL_GetTick(void) { return 0; }
int main(void)
{
    TIM_HandleTypeDef pwm = {.Init.Period=1000}, encoder = {0};
    GPIO_TypeDef gpio = {0};
    HAL_MotorConfig h = {.pwm={&pwm,1}, .encoder={&encoder},
        .gpio={&gpio,1,&gpio,2}};
    assert(HAL_Motor_Init(&h)==HAL_OK);
    HAL_Motor_SetDuty(&h,50);
    assert(pwm.compare==500);
    HAL_Motor_SetInhibit(&h,true);
    assert(pwm.compare==0 && gpio.pins==3);
    /* Simulate a pre-STOP control calculation submitting late. */
    for(int i=0;i<20;i++) {
        HAL_Motor_SetDuty(&h,(i%2)?70:-70);
        assert(pwm.compare==0 && gpio.pins==3);
    }
    HAL_Motor_SetInhibit(&h,false);
    HAL_Motor_SetDuty(&h,30);
    assert(pwm.compare==300);
    puts("motor_inhibit: all tests passed");
}
