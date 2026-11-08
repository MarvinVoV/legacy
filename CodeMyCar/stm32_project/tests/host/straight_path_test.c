#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "straight_path.h"
#include "heading_control.h"

static uint32_t tick;
uint32_t HAL_GetTick(void) { return tick; }

static void near(float a, float b, float tol) { assert(fabsf(a-b)<tol); }

static float simulate(bool outer, int direction, float speed)
{
    StraightPath p;
    HeadingControl hc;
    assert(straight_path_start(&p,170, direction*2));
    HeadingControl_Init(&hc,0,0);
    float yaw=170, kActual=0, y=0, peak=0;
    const float dt=.005f, v=direction*speed;
    const int steps=(int)(2.0f/(speed*dt));
    for(int i=0;i<steps;i++) {
        const float remaining=fmaxf(0,2-i*speed*dt);
        const float ref=straight_path_reference(&p,v,remaining,dt);
        tick+=5;
        const float rate=-v*(kActual+.10f)*57.29578f;
        const float k=HeadingControl_HoldHeading(&hc,yaw,rate,v,outer?ref:170);
        kActual+=(k-kActual)*dt/.15f;
        yaw+=-v*(kActual+.10f)*57.29578f*dt;
        y+=v*sinf((yaw-170)*.017453293f)*dt;
        assert(straight_path_update(&p,v*dt,yaw));
        peak=fmaxf(peak,fabsf(y));
        assert(fabsf(p.headingOffsetDeg)<=1.0001f);
    }
    printf("path sim en=%d direction=%d speed=%.1f end_mm=%.3f peak_mm=%.3f\n",
           outer,direction,(double)speed,(double)(y*1000),(double)(peak*1000));
    return fabsf(y);
}

int main(void)
{
    StraightPath p;
    assert(!straight_path_start(NULL,0,1));
    assert(!straight_path_start(&p,NAN,1) && !p.active);
    assert(!straight_path_start(&p,0,0) && !p.active);
    assert(straight_path_start(&p,170,2));
    assert(straight_path_update(&p,1,170));
    near(p.forwardM,1,.00001f);near(p.leftM,0,.00001f);
    assert(straight_path_update(&p,1,190)); /* continuous yaw crosses 180 */
    near(p.leftM,sinf(10*.017453293f),.00001f);
    const float previous=p.leftM;
    assert(!straight_path_update(&p,NAN,170));near(p.leftM,previous,.00001f);
    assert(!straight_path_update(&p,.01f,INFINITY));
    const float ref=straight_path_reference(&p,.3f,1,.005f);
    assert(ref<170 && p.headingOffsetDeg>=-.003751f); /* left -> right heading */
    straight_path_stop(&p);
    assert(!p.active && p.headingOffsetDeg==0);
    assert(!straight_path_update(&p,.01f,170));
    assert(straight_path_start(&p,-170,-2));
    near(p.leftM,0,.00001f);near(p.maxAbsLeftM,0,.00001f);
    p.leftM=.01f;
    assert(straight_path_reference(&p,-.3f,1,.005f)>-170); /* reverse sign */
    for(int i=0;i<500;i++) straight_path_reference(&p,-.3f,1,.005f);
    assert(p.headingOffsetDeg<=1.0001f);
    for(int i=0;i<500;i++) straight_path_reference(&p,-.3f,0,.005f);
    near(p.headingOffsetDeg,0,.00001f);
    assert(straight_path_reference(&p,NAN,1,.005f)==p.originYawDeg);
    assert(straight_path_reference(&p,-.3f,1,10)==p.originYawDeg);
    assert(straight_path_reference(&p,.3f,1,.005f)==p.originYawDeg); /* wrong-way transient */
    p.leftM=NAN;
    assert(straight_path_reference(&p,-.3f,1,.005f)==p.originYawDeg);
    for(int dir=-1;dir<=1;dir+=2) for(int speed=1;speed<=3;speed+=2) {
        const float baseline=simulate(false,dir,speed*.1f);
        const float controlled=simulate(true,dir,speed*.1f);
        assert(controlled<baseline*.75f && controlled<.005f);
    }
    puts("straight_path: all tests passed");
}
