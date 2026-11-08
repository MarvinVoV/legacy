#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "imu_service.h"
#include "sh2.h"
#include "sh2_SensorValue.h"

static uint32_t now=1000;
static int configFail;
static sh2_SensorCallback_t* callback;
static void* cookie;
static sh2_SensorValue_t value;
static int inject;
static ImuService* reading;
static unsigned reads;
sh2_Hal_t sh2_hal_stm32;
uint32_t HAL_GetTick(void) { return now; }
int sh2_open(sh2_Hal_t* h,sh2_EventCallback_t* cb,void* c)
{(void)h;(void)cb;(void)c;return 0;}
void sh2_close(void) {}
int sh2_setSensorCallback(sh2_SensorCallback_t* cb,void* c)
{callback=cb;cookie=c;return 0;}
int sh2_setSensorConfig(sh2_SensorId_t id,const sh2_SensorConfig_t* cfg)
{(void)id;(void)cfg;return configFail?-1:0;}
int sh2_decodeSensorEvent(sh2_SensorValue_t* out,const sh2_SensorEvent_t* e)
{(void)e;*out=value;return 0;}
void sh2_service(void)
{
    if(reading) { ImuSnapshot s;assert(ImuService_GetSnapshot(reading,&s));reads++; }
    if(inject) { inject=0; sh2_SensorEvent_t e={0};callback(cookie,&e); }
}
static void report(ImuService* s,sh2_SensorId_t id,float z)
{
    value=(sh2_SensorValue_t){.sensorId=id};
    if(id==SH2_GAME_ROTATION_VECTOR) value.un.gameRotationVector.real=z;
    else value.un.gyroscope.z=z;
    inject=1;ImuService_Update(s);
}
int main(void)
{
    ImuService s={0};ImuSnapshot snap;
    configFail=1;assert(ImuService_Init(&s,0)!=0);assert(!s.inited);
    configFail=0;assert(ImuService_Init(&s,0)==0);
    report(&s,SH2_GAME_ROTATION_VECTOR,1);
    ImuService_GetSnapshot(&s,&snap);assert(!snap.valid);
    report(&s,SH2_GYROSCOPE_CALIBRATED,0);
    ImuService_GetSnapshot(&s,&snap);assert(snap.valid);
    uint32_t reference=snap.referenceId;
    ImuService_ResetYawReference(&s);ImuService_Update(&s);
    ImuService_GetSnapshot(&s,&snap);assert(snap.referenceId!=reference);
    reference=snap.referenceId;
    reading=&s;ImuService_Update(&s);reading=0;assert(reads==8);
    report(&s,SH2_GAME_ROTATION_VECTOR,NAN);
    ImuService_GetSnapshot(&s,&snap);assert(!snap.valid);
    report(&s,SH2_GAME_ROTATION_VECTOR,1);
    report(&s,SH2_GYROSCOPE_CALIBRATED,NAN);
    ImuService_GetSnapshot(&s,&snap);assert(!snap.valid);
    report(&s,SH2_GYROSCOPE_CALIBRATED,0);
    now+=201;report(&s,SH2_GAME_ROTATION_VECTOR,1);
    ImuService_GetSnapshot(&s,&snap);assert(!snap.valid); /* gyro-only stale */
    report(&s,SH2_GYROSCOPE_CALIBRATED,0);
    ImuService_GetSnapshot(&s,&snap);assert(snap.valid);
    /* Diagnose report age across the 32-bit microsecond wrap. This records
     * SH-2 reconstructed timestamps, not an independently measured latency. */
    now=4294968u;
    value=(sh2_SensorValue_t){.sensorId=SH2_GAME_ROTATION_VECTOR,
        .timestamp=(uint64_t)now*1000u-120000u};
    value.un.gameRotationVector.real=1;inject=1;ImuService_Update(&s);
    assert(s.grvRxAgeUs==120000 && s.grvMaxRxAgeUs>=120000);
    value=(sh2_SensorValue_t){.sensorId=SH2_GYROSCOPE_CALIBRATED,
        .timestamp=(uint64_t)now*1000u-5000u};
    inject=1;ImuService_Update(&s);
    assert(s.gyroRxAgeUs==5000);
    assert((int32_t)(s.grvSampleUs-s.gyroSampleUs)==-115000);
    value.timestamp=(uint64_t)now*1000u+1000u;
    inject=1;ImuService_Update(&s);assert(s.gyroRxAgeUs==-1000);
    ImuService_Close(&s);ImuService_GetSnapshot(&s,&snap);assert(!snap.valid);
    assert(ImuService_Init(&s,0)==0);
    ImuService_GetSnapshot(&s,&snap);assert(snap.referenceId!=reference);
    puts("imu_service: all tests passed");
}
