/* Production LUT -> smoothing service -> real driver -> mocked PWM boundary.
 * No mechanical model: submitted PWM does not prove measured wheel angle. */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "servo_service.h"
#include "steering_lut.h"
static uint32_t tick;
static uint16_t pulse;
static bool fail_write;
uint32_t HAL_GetTick(void) { return tick; }
osMutexId_t osMutexNew(const void* a) { (void)a; return (void*)1; }
osStatus_t osMutexAcquire(osMutexId_t m,uint32_t t) { (void)m;(void)t;return osOK; }
osStatus_t osMutexRelease(osMutexId_t m) { (void)m;return osOK; }
void ServoTask_init(void* p) { (void)p; }
int ServoHAL_init(HAL_ServoConfig* p) { pulse=p->safetyPulse;return 0; }
int ServoHAL_setPulse(HAL_ServoConfig* p,uint16_t us) {
    assert(us>=p->minPulse && us<=p->maxPulse);
    if(fail_write)return -1;
    pulse=us;return 0;
}
int ServoHAL_emergencyStop(HAL_ServoConfig* p) { pulse=p->safetyPulse;return 0; }
int main(void) {
    HAL_ServoConfig hw={.minPulse=500,.maxPulse=2500,.safetyPulse=STEERING_SERVO_STRAIGHT_US};
    ServoDriver driver={.hw=&hw,.spec={.minAngle=0,.maxAngle=180,.resolutionDeg=.1f}};
    SteerConfig cfg={.minAngleDeg=0,.maxAngleDeg=180,.deadZoneDeg=STEER_CONTROL_DEADZONE_DEG,
        .maxSpeedDegPerSec=90,.accelerationDegPerSec2=180,.updateIntervalMs=20};
    SteerInstance inst={0};assert(SteerService_init(&inst,&cfg,&driver)==0);
    float raw;assert(steering_lut_curvature_to_raw(0,.141f,&raw));
    assert(SteerService_setAngleImmediate(&inst,90-raw)==0);
    assert(pulse==STEERING_SERVO_STRAIGHT_US);
    /* Includes the correction range in the actual reverse logs, both signs,
     * followed by sign reversals. A common LUT neutral must stay unchanged. */
    const float k[]={-.06f,-.12f,-.17f,.06f,.12f,.17f,-.02f,.02f,0};
    for(unsigned j=0;j<sizeof(k)/sizeof(k[0]);j++) {
        float expected;assert(steering_lut_curvature_to_pulse(k[j],.141f,&expected));
        assert(steering_lut_curvature_to_raw(k[j],.141f,&raw));
        int settled=-1;
        for(int i=0;i<400;i++) {
            tick+=5;assert(SteerService_setAngleSmoothly(&inst,90-raw)==0);
            if(i%4==3)assert(SteerService_update(&inst)==0);
            if(fabsf((float)pulse-expected)<=3 && settled<0)settled=(i+1)*5;
        }
        printf("k=%+.3f expected=%.2fus actual=%uus settle<=3us=%dms\n",k[j],expected,pulse,settled);
        assert(fabsf((float)pulse-expected)<=3);
        assert(settled>=0 && settled<=500);
        if(fabsf(k[j])>=.06f)assert(k[j]<0 ? pulse>STEERING_SERVO_STRAIGHT_US : pulse<STEERING_SERVO_STRAIGHT_US);
    }
    assert(steering_lut_curvature_to_raw(0,.141f,&raw));
    assert(SteerService_setAngleImmediate(&inst,90-raw)==0);
    assert(pulse==STEERING_SERVO_STRAIGHT_US);
    for(int j=0;j<2;j++) {
        const float trim=j==0?6:14;
        float trimmed;
        assert(steering_lut_trim_raw(raw,trim,&trimmed));
        for(int i=0;i<400;i++) {
            tick+=5;assert(SteerService_setAngleSmoothly(&inst,90-trimmed)==0);
            if(i%4==3)assert(SteerService_update(&inst)==0);
        }
        assert(fabsf(pulse-(STEERING_SERVO_STRAIGHT_US+trim))<=3);
        assert(SteerService_setAngleImmediate(&inst,90-raw)==0);
        assert(pulse==STEERING_SERVO_STRAIGHT_US);
    }
    const float previous=inst.state.commandedAngleDeg;
    fail_write=true;
    assert(SteerService_setAngleImmediate(&inst,previous+1)!=0);
    assert(inst.state.commandedAngleDeg==previous);
    assert(pulse==STEERING_SERVO_STRAIGHT_US);
    fail_write=false;
    assert(SteerService_setAngleImmediate(&inst,90-raw)==0);
    assert(pulse==STEERING_SERVO_STRAIGHT_US);
    puts("steering_pipeline: real driver and smoothing passed; no mechanical accuracy claim");
}
