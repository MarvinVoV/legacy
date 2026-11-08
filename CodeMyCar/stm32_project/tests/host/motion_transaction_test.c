#include <assert.h>
#include <stdio.h>
#include "motion_service.h"
#include "imu_service.h"
#include "safety_service.h"
#include "error_code.h"

static uint32_t now=1000;
static unsigned depth;
static bool mutexFail, injectStop, inhibited=true, brakeFail;
static bool measurementFail, measurementStale;
static bool imuAvailable;
static float imuYaw, leftRevs, rightRevs;
static float measuredLeftRpm, measuredRightRpm;
static float submittedRpm[2];
static int targetFail=-1;
static bool steerFail;
static uint32_t imuReference=7;
static MotionContext* current;
static SafetyChannel front,rear;
static float lastAngle;
static unsigned driveSubmits;
uint32_t HAL_GetTick(void){return now;}
osMutexId_t osMutexNew(const void* a)
{assert(((const osMutexAttr_t*)a)->attr_bits & osMutexRecursive);return (void*)1;}
osStatus_t osMutexAcquire(osMutexId_t m,uint32_t t)
{(void)m;(void)t;if(mutexFail)return 1;depth++;return osOK;}
osStatus_t osMutexRelease(osMutexId_t m)
{(void)m;assert(depth>0);depth--;return osOK;}
void MotionTask_init(MotionContext* c){(void)c;}
void MotorService_SetInhibit(MotorService* s,bool v){(void)s;inhibited=v;}
int MotorService_emergencyStop(MotorService* s,MotorID i)
{(void)s;(void)i;assert(inhibited);return 0;}
int MotorService_setTargetRPM(MotorService* s,MotorID i,float v)
{(void)s;submittedRpm[i]=v;assert(depth>0);if(!inhibited)driveSubmits++;
 return targetFail==(int)i?ERR_HW_INIT_FAIL:0;}
int MotorService_brakeAll(MotorService* s)
{MotorService_SetInhibit(s,true);return brakeFail?ERR_MOTOR_MUTEX_FAIL:0;}
bool MotorService_getStateEx(MotorService* s,MotorID i,MotorState* o)
{(void)s;*o=(MotorState){.lastUpdate=measurementStale?now-201:now};
 o->position.revolutions=i==MOTOR_LEFT_WHEEL?leftRevs:rightRevs;
 o->velocity.rpm=i==MOTOR_LEFT_WHEEL?measuredLeftRpm:measuredRightRpm;
 o->velocity.mps=o->velocity.rpm*2*3.14159265f*0.0325f/60;return !measurementFail;}
int SteerService_setAngleImmediate(SteerInstance* s,float a)
{(void)s;assert(depth>0);lastAngle=a;return 0;}
int SteerService_setAngleSmoothly(SteerInstance* s,float a)
{
    (void)s;lastAngle=a;assert(depth>0);
    if(injectStop) {
        injectStop=false;mutexFail=true;
        assert(MotionService_EmergencyStop(current,55)==ERR_MOTION_MUTEX_TIMEOUT);
        mutexFail=false;assert(inhibited && current->stopPending);
    }
    return steerFail?ERR_HW_INIT_FAIL:0;
}
SteerState SteerService_getState(const SteerInstance* s)
{(void)s;return (SteerState){0};}
ImuService* ImuTask_GetService(void){return NULL;}
bool ImuService_GetSnapshot(ImuService* s,ImuSnapshot* o)
{(void)s;*o=(ImuSnapshot){.valid=imuAvailable,.yawDeg=imuYaw,
 .referenceId=imuReference,.lastUpdateMs=now,.lastGyroMs=now};return imuAvailable;}
void Sonar_GetSafetySnapshot(SafetyChannel* f,SafetyChannel* r){*f=front;*r=rear;}
int main(void)
{
    MotionContext c;MotorService motor={0};SteerInstance steer={0};
    ChassisConfig cfg={.wheelbaseMM=141,.trackWidthMM=167,.wheelRadiusMM=32.5f,
        .maxSteerAngleDeg=28.5f,.minTurnRadiusMM=260,.maxLinearVelocityMPS=1,
        .maxAngularVelRad=8.2f,.steerCenterAngleDeg=90};
    current=&c;safety_channel_init(&front,true,0,12,40);
    safety_channel_init(&rear,true,0,12,40);
    for(int i=0;i<5;i++){safety_channel_update(&front,100,now);safety_channel_update(&rear,100,now);}
    assert(MotionContext_Init(&c,&motor,&steer,&cfg)==0);
    measurementStale=true;now+=2000;
    MotionService_Run(&c);
    assert(!c.faultLatched && !c.motorStateValid);
    assert(MotionService_SetDriveParams(&c,0.1f,0,0)==ERR_MOTION_EMERGENCY_ACTIVE);
    measurementStale=false;
    for(int i=0;i<5;i++){safety_channel_update(&front,100,now);safety_channel_update(&rear,100,now);}
    MotionService_Run(&c); /* initial fresh zero-speed measurements */
    assert(MotionService_SetDifferentialParams(&c,0,0.5f,0)==ERR_MOTION_GEOMETRY);
    assert(MotionService_GetCurrentMode(&c)==MOTION_EMERGENCY_STOP);
    assert(MotionService_SetDriveParams(&c,0.1f,0,1)==0);
    injectStop=true;MotionService_Run(&c);
    assert(inhibited && driveSubmits==0 && depth==0);
    MotionService_Run(&c);assert(c.faultLatched && inhibited && depth==0);
    assert(MotionService_ClearFault(&c,2)==0);
    assert(MotionService_SetDriveParams(&c,0.1f,0,3)==0);
    MotionService_Run(&c);assert(!inhibited && driveSubmits==2);
    assert(MotionService_EmergencyStop(&c,4)==0);
    MotionService_Run(&c);assert(inhibited && depth==0);
    /* Old 260mm minimum radius is outside the final curvature envelope. */
    assert(MotionService_StartGoal(&c,900,409,50,5)==ERR_MOTION_GEOMETRY);
    assert(MotionService_StartGoal(&c,900,420,50,6)==ERR_MOTION_SENSOR_UNAVAILABLE);
    assert(MotionService_GetCurrentMode(&c)==MOTION_EMERGENCY_STOP);
    imuAvailable=true;
    assert(MotionService_StartGoal(&c,900,420,50,6)==0);
    MotionService_Run(&c);assert(!c.faultLatched && isfinite(lastAngle));
    safety_channel_update(&front,5,now);brakeFail=true;
    MotionService_Run(&c);assert(inhibited && c.faultLatched && depth==0);
    assert(MotionService_ShouldReportFast(&c));
    now+=1100;
    assert(!MotionService_ShouldReportFast(&c));
    c.state.drive.leftRpm=0.2f;
    assert(MotionService_ShouldReportFast(&c));
    brakeFail=false;
    MotionService_Run(&c);
    assert(MotionService_ClearFault(&c,7)==0);
    assert(MotionService_SetDriveParams(&c,0.1f,0,8)==0);
    MotionService_Run(&c);
    measurementFail=true;now+=100;
    MotionService_Run(&c);assert(!c.faultLatched);
    now+=101;MotionService_Run(&c);
    assert(c.faultLatched && inhibited && c.faultCode==MOTION_ERR_ENCODER);
    assert(MotionService_ClearFault(&c,9)==ERR_MOTION_CLEAR_FAILED);
    measurementFail=false;MotionService_Run(&c);
    assert(MotionService_ClearFault(&c,10)==0);
    assert(MotionService_SetDriveParams(&c,0.1f,0,11)==0);
    measurementStale=true;MotionService_Run(&c);
    assert(c.faultLatched && inhibited && c.faultCode==MOTION_ERR_ENCODER);
    measurementStale=false;MotionService_Run(&c);
    assert(MotionService_ClearFault(&c,12)==0);
    for(int i=0;i<5;i++){safety_channel_update(&front,100,now);safety_channel_update(&rear,100,now);}
    /* Encoders claim 40deg, IMU says 10deg: planner uses the physical yaw. */
    assert(MotionService_StartGoal(&c,900,500,50,13)==0 && c.goalUsesImu);
    const float circ=2*3.14159265f*32.5f;
    leftRevs=(100+0.5f*167*40*3.14159265f/180)/circ;
    rightRevs=(100-0.5f*167*40*3.14159265f/180)/circ;
    imuYaw=-10;MotionService_Run(&c);
    assert(fabsf(c.goal.headingTravelDeg-10)<0.001f);
    assert(fabsf(c.goalEncoderHeadingDeg-40)<0.001f);
    assert(!c.goal.finished);
    /* Matching distance + IMU heading completes even with encoder disagreement. */
    leftRevs+=400/circ;rightRevs+=400/circ;imuYaw=-90;
    MotionService_Run(&c);assert(c.goal.finished && !c.goal.error && inhibited);
    motion_status_t status;MotionService_GetStatus(&c,&status);
    assert(status.state==MOTION_STATE_DONE && (status.flags&MOTION_STATUS_FLAG_GOAL_IMU));
    assert(status.goalTurnMdeg==90000);
    assert(MotionService_EmergencyStop(&c,14)==0);
    /* Source loss and reference changes must not fall back or report success. */
    assert(MotionService_StartGoal(&c,900,500,50,15)==0);
    imuAvailable=false;MotionService_Run(&c);
    assert(c.faultLatched && c.faultCode==MOTION_ERR_IMU && inhibited);
    assert(MotionService_ClearFault(&c,16)==0);
    imuAvailable=true;
    assert(MotionService_StartGoal(&c,900,500,50,17)==0);
    imuReference++;MotionService_Run(&c);
    assert(c.faultLatched && c.faultCode==MOTION_ERR_IMU && inhibited);
    assert(MotionService_ClearFault(&c,18)==0);
    /* Continuous yaw must preserve turns across +/-180 and over 180 degrees. */
    imuYaw=170;
    assert(MotionService_StartGoal(&c,2700,2000,50,19)==0);
    imuYaw=-100;MotionService_Run(&c);
    assert(fabsf(c.goal.headingTravelDeg-270)<0.001f);
    assert(MotionService_EmergencyStop(&c,20)==0);
    /* Explicit straight encoder fallback does not switch source mid-goal. */
    imuAvailable=false;
    assert(MotionService_StartGoal(&c,0,100,50,21)==0 && !c.goalUsesImu);
    imuAvailable=true;imuYaw=50;MotionService_Run(&c);
    assert(c.goal.headingTravelDeg==0 && !c.goalUsesImu);
    assert(MotionService_EmergencyStop(&c,22)==0);
    c.goal.startMs=UINT32_MAX-5u;c.goal.finishMs=10u;
    MotionService_GetStatus(&c,&status);
    assert(status.goalDurationMs==16u);
    /* At low speed the intentional wheel difference is <3RPM. Old sync
     * heuristics could reverse that turn under a 20RPM measurement error. */
    imuAvailable=false;measuredLeftRpm=30;measuredRightRpm=10;
    assert(MotionService_SetDriveParams(&c,0.04f,0.5f,23)==0);
    MotionService_Run(&c);
    assert(c.syncBias==0 && submittedRpm[0]>submittedRpm[1]);
    assert(MotionService_SetDriveParams(&c,0.04f,0,24)==0);
    MotionService_Run(&c);assert(c.syncBias>0);
    assert(MotionService_SetDriveParams(&c,0.04f,-0.5f,25)==0);
    MotionService_Run(&c);
    assert(c.syncBias==0 && c.syncIntegral==0 && submittedRpm[0]<submittedRpm[1]);
    measuredLeftRpm=measuredRightRpm=0;
    assert(MotionService_EmergencyStop(&c,26)==0);
    MotionService_Run(&c);
    assert(MotionService_StartGoal(&c,0,100,50,27)==0);
    MotionService_AbortGoal(&c,MOTION_STATE_ABORTED);
    assert(c.goal.aborted && !c.goal.active && inhibited && depth==0);
    assert(MotionService_SetDriveParams(&c,0.04f,0,28)==0);
    targetFail=1;MotionService_Run(&c);
    assert(c.faultLatched && inhibited && c.faultCode==MOTION_ERR_INTERNAL);
    targetFail=-1;assert(MotionService_ClearFault(&c,29)==0);
    assert(MotionService_SetDriveParams(&c,0.04f,0,30)==0);
    steerFail=true;MotionService_Run(&c);
    assert(c.faultLatched && inhibited && c.faultCode==MOTION_ERR_INTERNAL);
    steerFail=false;assert(MotionService_ClearFault(&c,31)==0);
    mutexFail=true;MotionService_LatchFault(&c,MOTION_ERR_TASK_STALE);
    mutexFail=false;assert(c.stopPending && inhibited);
    MotionService_Run(&c);assert(c.faultLatched && inhibited && depth==0);
    assert(MotionService_ClearFault(&c,32)==0);
    assert(MotionService_SetDriveParams(&c,0.1f,0,33)==0);
    imuAvailable=true;imuYaw=0;measuredLeftRpm=measuredRightRpm=30;
    MotionService_Run(&c);
    const float straightAngle=lastAngle;
    imuReference++;imuYaw=20;
    MotionService_Run(&c);
    assert(fabsf(lastAngle-straightAngle)<0.001f);
    puts("motion_transaction: all tests passed");
}
