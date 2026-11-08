#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "imu_service.h"
#include "imu_yaw_rate.h"
#include "sh2.h"
#include "sh2_SensorValue.h"

static uint32_t now=1000;
static int configFail;
static int diagConfigFail;
static uint32_t rvPeriod,magPeriod;
static sh2_SensorCallback_t* callback;
static sh2_EventCallback_t* eventCallback;
static void* eventCookie;
static void* cookie;
static sh2_SensorValue_t value;
static int inject;
static ImuService* reading;
static unsigned reads;
sh2_Hal_t sh2_hal_stm32;
uint32_t HAL_GetTick(void) { return now; }
int sh2_open(sh2_Hal_t* h,sh2_EventCallback_t* cb,void* c)
{(void)h;eventCallback=cb;eventCookie=c;return 0;}
void sh2_close(void) {}
int sh2_setSensorCallback(sh2_SensorCallback_t* cb,void* c)
{callback=cb;cookie=c;return 0;}
int sh2_setSensorConfig(sh2_SensorId_t id,const sh2_SensorConfig_t* cfg)
{if(id==SH2_ROTATION_VECTOR)rvPeriod=cfg->reportInterval_us;
 if(id==SH2_MAGNETIC_FIELD_CALIBRATED)magPeriod=cfg->reportInterval_us;
 return configFail || (diagConfigFail && (id==SH2_ROTATION_VECTOR || id==SH2_MAGNETIC_FIELD_CALIBRATED)) ? -1 : 0;}
int sh2_getCalConfig(uint8_t* sensors) { *sensors=SH2_CAL_ACCEL|SH2_CAL_GYRO;return 0; }
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
    uint32_t previousReference=snap.referenceId;
    sh2_AsyncEvent_t reset={.eventId=SH2_RESET};eventCallback(eventCookie,&reset);
    ImuService_GetSnapshot(&s,&snap);
    assert(!snap.valid && snap.referenceId!=previousReference); /* no I2C-batch wait */
    report(&s,SH2_GAME_ROTATION_VECTOR,1);report(&s,SH2_GYROSCOPE_CALIBRATED,0);
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
    /* Independent numerical derivative: integrate quaternion by body omega,
     * then compare decoded yaw change to the analytic yaw-rate function. */
    float worstRateError=0;
    for(int ri=-1;ri<=1;ri++) for(int pi=-1;pi<=1;pi++) for(int yi=-1;yi<=1;yi++) {
        const float rr=ri*1.04719755f,pp=pi*.69813170f,yy=yi*3.1414f;
        const float cr=cosf(rr/2),sr=sinf(rr/2),cp=cosf(pp/2),sp=sinf(pp/2),cy=cosf(yy/2),sy=sinf(yy/2);
        const float q[4]={cr*cp*cy+sr*sp*sy,sr*cp*cy-cr*sp*sy,cr*sp*cy+sr*cp*sy,cr*cp*sy-sr*sp*cy};
        const float p=.4f,gq=-.2f,r=.7f,dt=.001f;
        const float dq[4]={-.5f*(q[1]*p+q[2]*gq+q[3]*r),.5f*(q[0]*p+q[2]*r-q[3]*gq),
            .5f*(q[0]*gq+q[3]*p-q[1]*r),.5f*(q[0]*r+q[1]*gq-q[2]*p)};
        float next[4],nn=0,analytic;
        for(int i=0;i<4;i++){next[i]=q[i]+dt*dq[i];nn+=next[i]*next[i];}
        for(int i=0;i<4;i++)next[i]/=sqrtf(nn);
        const float yaw0=atan2f(2*(q[0]*q[3]+q[1]*q[2]),1-2*(q[2]*q[2]+q[3]*q[3]));
        const float yaw1=atan2f(2*(next[0]*next[3]+next[1]*next[2]),1-2*(next[2]*next[2]+next[3]*next[3]));
        const float numeric=remainderf(yaw1-yaw0,6.283185307f)/dt;
        assert(imu_euler_yaw_rate(q[0],q[1],q[2],q[3],gq,r,&analytic));
        worstRateError=fmaxf(worstRateError,fabsf(numeric-analytic));
        assert(fabsf(numeric-analytic)<.003f);
    }
    printf("Euler yaw-rate quaternion derivative grid: max error %.6f rad/s\n",worstRateError);
    /* Known world-yaw rotation at roll30/pitch20: gyro Z underreads yaw. */
    const float roll=.5235987756f,pitch=.3490658504f;
    const float w=cosf(roll/2)*cosf(pitch/2), x=sinf(roll/2)*cosf(pitch/2);
    const float y=cosf(roll/2)*sinf(pitch/2), z=-sinf(roll/2)*sinf(pitch/2);
    float rate=99;
    assert(imu_euler_yaw_rate(1,0,0,0,.7f,.3f,&rate) && fabsf(rate-.3f)<1e-6f);
    for(int sign=-1;sign<=1;sign+=2) {
        const float gy=sign*sinf(roll)*cosf(pitch),gz=sign*cosf(roll)*cosf(pitch);
        assert(imu_euler_yaw_rate(w,x,y,z,gy,gz,&rate) && fabsf(rate-sign)<1e-5f);
        assert(imu_euler_yaw_rate(1.1f*w,1.1f*x,1.1f*y,1.1f*z,gy,gz,&rate) && fabsf(rate-sign)<1e-5f);
    }
    value=(sh2_SensorValue_t){.sensorId=SH2_GAME_ROTATION_VECTOR};
    value.un.gameRotationVector.real=w;value.un.gameRotationVector.i=x;
    value.un.gameRotationVector.j=y;value.un.gameRotationVector.k=z;
    inject=1;ImuService_Update(&s);
    value=(sh2_SensorValue_t){.sensorId=SH2_GYROSCOPE_CALIBRATED};
    value.un.gyroscope.y=sinf(roll)*cosf(pitch);value.un.gyroscope.z=cosf(roll)*cosf(pitch);
    inject=1;ImuService_Update(&s);ImuService_GetSnapshot(&s,&snap);
    assert(snap.valid && fabsf(snap.yawRateDps-57.29578f)<.001f);
    const float half=sqrtf(.5f);rate=99;
    assert(!imu_euler_yaw_rate(half,0,half,0,0,1,&rate) && rate==99);
    assert(!imu_euler_yaw_rate(NAN,0,0,0,0,1,&rate));
    assert(!imu_euler_yaw_rate(1,0,0,0,NAN,1,&rate));
    assert(!imu_euler_yaw_rate(0,0,0,0,0,1,&rate));
    assert(!imu_euler_yaw_rate(1,0,0,0,0,1,NULL));
    value=(sh2_SensorValue_t){.sensorId=SH2_GAME_ROTATION_VECTOR};
    value.un.gameRotationVector.real=half;value.un.gameRotationVector.j=half;
    inject=1;ImuService_Update(&s);ImuService_GetSnapshot(&s,&snap);assert(!snap.valid && !s.yawRateValid);
    report(&s,SH2_GAME_ROTATION_VECTOR,1);report(&s,SH2_GYROSCOPE_CALIBRATED,0);
    ImuService_GetSnapshot(&s,&snap);assert(snap.valid && snap.yawRateDps==0);
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
#if CMCAR_IMU_MAG_DIAGNOSTICS
    assert(s.magDiagnosticsEnabled && s.diagCalReadOk);
    assert(rvPeriod==40000 && magPeriod==20000);
    const float controlYaw=s.yawDeg;
    value=(sh2_SensorValue_t){.sensorId=SH2_ROTATION_VECTOR,.status=3};
    value.un.rotationVector.real=sqrtf(.5f);value.un.rotationVector.k=sqrtf(.5f);
    value.un.rotationVector.accuracy=.02f;inject=1;ImuService_Update(&s);
    assert(s.diagRvValid && fabsf(s.diagRvYawDeg-90)<.001f);
    assert(s.yawDeg==controlYaw); /* magnetic yaw never enters GRV control */
    value.un.rotationVector.real=NAN;inject=1;ImuService_Update(&s);assert(!s.diagRvValid);
    value=(sh2_SensorValue_t){.sensorId=SH2_MAGNETIC_FIELD_CALIBRATED,.status=3};
    value.un.magneticField.x=30;value.un.magneticField.y=40;
    inject=1;ImuService_Update(&s);assert(s.diagMagValid && s.diagMagX==30);
    value.un.magneticField.x=NAN;inject=1;ImuService_Update(&s);assert(!s.diagMagValid);
    diagConfigFail=1;ImuService diagFailed={0};
    assert(ImuService_Init(&diagFailed,0)==0 && diagFailed.inited && !diagFailed.magDiagnosticsEnabled);
    diagConfigFail=0;
#endif
    ImuService_Close(&s);ImuService_GetSnapshot(&s,&snap);assert(!snap.valid);
    assert(ImuService_Init(&s,0)==0);
    ImuService_GetSnapshot(&s,&snap);assert(snap.referenceId!=reference);
    puts("imu_service: all tests passed");
}
