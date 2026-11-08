/* Independent bicycle-yaw plant: actuator lag, transport delay, 100Hz
 * quaternion holds and quantization. Model comparison is not ground truth. */
#include "heading_control.h"
#include <math.h>
#include <assert.h>
#include <stdio.h>
static uint32_t tick;
uint32_t HAL_GetTick(void) {return tick;}
typedef struct {float peak_yaw,peak_y,final_y;} Result;
static Result simulate(float v,float tau,float bias,int baseline)
{
 HeadingControl h;HeadingControl_Init(&h,0,0);
 if(baseline){h.kp=.3f;h.kpGoal=.3f;h.kd=0;}
 float yaw=0,k=0,y=0,measurement=0,queue[7]={0};Result r={0};
 int steps=(int)(2/fabsf(v)/.005f);
 for(int i=0;i<steps;++i){
  tick+=5;
  if(!(i%2))measurement=roundf(yaw*100)/100; /* sensor100Hz */
  queue[i%7]=measurement;float delayed=queue[(i+1)%7];
  float command=HeadingControl_HoldHeading(&h,delayed,0,v,0);
  assert(isfinite(command)&&fabsf(command)<=h.maxStraightCorr+.0001f);
  k+=(command-k)*.005f/tau;
  yaw+=-v*(k+bias)*57.2957795f*.005f;
  y+=v*sinf(yaw*.017453293f)*.005f;
  r.peak_yaw=fmaxf(r.peak_yaw,fabsf(yaw));r.peak_y=fmaxf(r.peak_y,fabsf(y));
 }
 r.final_y=y;return r;
}
int main(void){
 for(int sign=-1;sign<=1;sign+=2)for(int speed=1;speed<=3;speed+=2)
 for(int lag=15;lag<=25;lag+=10){
  float v=sign*speed*.1f,tau=lag*.01f;
  Result old=simulate(v,tau,.08f,1),candidate=simulate(v,tau,.08f,0);
  printf("v=%+.1f lag=%.2f old/new peakYaw=%.4f/%.4f peakYmm=%.4f/%.4f finalYmm=%.4f/%.4f\n",v,tau,old.peak_yaw,candidate.peak_yaw,old.peak_y*1000,candidate.peak_y*1000,old.final_y*1000,candidate.final_y*1000);
  /* Added damping should improve angular transient without increasing
   * accumulated lateral error in this independent delayed plant. */
  assert(candidate.peak_yaw<old.peak_yaw*.8f);
  assert(candidate.peak_y<=old.peak_y+.0002f);
 }
 HeadingControl h;HeadingControl_Init(&h,0,0);
 HeadingControl_HoldHeading(&h,0,0,.1f,0);tick+=5;
 float out=HeadingControl_HoldHeading(&h,.1f,0,.1f,0);
 assert(out>.06f && h.filteredYawRateDps>0);
 HeadingControl copy=h;tick+=5;
 HeadingControl_HoldHeading(&h,.1f,0,.1f,0);
 HeadingControl_HoldHeading(&copy,.1f,0,.1f,20);
 assert(fabsf(h.filteredYawRateDps-copy.filteredYawRateDps)<.00001f);
 tick+=5;HeadingControl_HoldHeading(&h,.1f,0,-.1f,0);
 assert(h.filteredYawRateDps==0 && h.lastCorr<0);
 HeadingControl_Disable(&h);tick+=5;
 assert(HeadingControl_HoldHeading(&h,30,0,.1f,30)==0);
 HeadingControl_Disable(&h);tick+=5;
 HeadingControl_HoldHeading(&h,179.99f,0,.1f,179.99f);tick+=5;
 HeadingControl_HoldHeading(&h,-179.99f,0,.1f,179.99f);
 assert(fabsf(h.filteredYawRateDps)<1);
 tick+=500;HeadingControl_HoldHeading(&h,-179.98f,0,.1f,179.99f);
 assert(h.filteredYawRateDps==0);
 HeadingControl_Disable(&h);h.maxKappa=.01f;
 HeadingControl_HoldHeading(&h,0,0,.1f,0);
 for(int i=0;i<1000;++i){tick+=5;out=HeadingControl_HoldHeading(&h,.1f,0,.1f,0);assert(fabsf(out)<=.01001f);}
 assert(fabsf(h.integral)<.001f);
 HeadingControl_Reset(&h,0);assert(!h.yawRateTracking && h.filteredYawRateDps==0);
 puts("heading delayed-plant and derivative lifecycle: PASS");
}
