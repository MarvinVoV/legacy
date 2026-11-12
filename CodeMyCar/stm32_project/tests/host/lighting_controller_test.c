#include "lighting_service.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static LightHints hints;
static LightRenderer renderer;
static LightLocal local;
static LedRgb pixels[6];
static LightView view;
static uint32_t now;
static void step(uint32_t time)
{
    now=time;local.sampledMs=time;local.batteryMs=time;
    Lighting_Render(&renderer,&hints,&local,time,pixels,&view);
    for (unsigned i=0;i<6;++i)
        assert(pixels[i].r<=12 && pixels[i].g<=12 && pixels[i].b<=12);
}
static LightCommand mood(uint32_t event)
{
    return (LightCommand){.session=42,.epoch=1,.event=event,.kind=LIGHT_EXPRESSION,
        .value=LIGHT_HAPPY,.intensity=100,.ttlMs=3000};
}
int main(void)
{
    local.motionValid=true;Lighting_NewSession(&hints,42);step(0);
    assert(view.mode==LIGHT_IDLE && !(view.flags&4));
    LightCommand command=mood(1);
    assert(Lighting_Accept(&hints,&command,0));step(100);
    assert(view.mood==LIGHT_HAPPY && view.moodRemainingMs==2900);
    const uint32_t phase=renderer.phaseMs;
    command.event=2;assert(Lighting_Accept(&hints,&command,200));step(200);
    assert(renderer.phaseMs==phase); /* Refresh never restarts animation. */
    assert(!Lighting_Accept(&hints,&command,201));
    command.event=1;assert(!Lighting_Accept(&hints,&command,202));
    command.session=41;assert(!Lighting_Accept(&hints,&command,202));
    command=mood(3);command.epoch=0;assert(!Lighting_Accept(&hints,&command,202));
    step(3200);assert(view.mood==LIGHT_CALM && !(view.flags&4));

    command=mood(3);command.epoch=2;assert(Lighting_Accept(&hints,&command,now));
    LightCommand voice={.kind=LIGHT_VOICE,.session=42,.epoch=2,.event=1,
        .value=LIGHT_VOICE_SPEAK,.intensity=100,.ttlMs=1500};
    assert(Lighting_Accept(&hints,&voice,now));step(3300);assert(view.voice==LIGHT_VOICE_SPEAK);
    step(4700);assert(view.voice==LIGHT_VOICE_IDLE);
    local.speedMmps=80;step(4800);assert(view.mode==LIGHT_FORWARD);
    local.speedMmps=-80;step(4840);assert(view.mode==LIGHT_REVERSE);
    local.steering=-1;step(4880);assert(view.mode==LIGHT_LEFT);
    local.steering=1;step(4920);assert(view.mode==LIGHT_RIGHT);
    local.stopRequested=true;step(4960);
    assert(view.mode==LIGHT_BRAKE && pixels[0].r==12 && !pixels[0].g && !pixels[0].b);
    local.speedMmps=0;step(5000);step(5440);assert(view.mode==LIGHT_IDLE);
    local.fault=true;step(5480);
    assert(view.mode==LIGHT_FAULT);
    for (unsigned i=0;i<6;++i) assert(pixels[i].r>0 && !pixels[i].g && !pixels[i].b);
    local.fault=false;local.stopRequested=false;local.steering=0;

    local.batteryValid=true;local.batteryPercent=20;
    step(6000);step(10999);assert(!(view.flags&1));
    step(11000);assert((view.flags&1) && view.mode==LIGHT_WARNING);
    local.batteryPercent=25;step(12000);step(21999);assert(view.flags&1);
    step(22000);assert(!(view.flags&1));
    /* Invalid/expired SOC is not interpreted as an empty battery. */
    local.batteryValid=false;local.batteryPercent=0;step(30000);step(40000);assert(!(view.flags&1));
    local.sampledMs=0;Lighting_Render(&renderer,&hints,&local,41000,pixels,&view);
    assert(view.mode==LIGHT_WARNING && (view.flags&2));

    Lighting_NewSession(&hints,43);assert(!hints.mood.seen && !hints.voice.seen);
    command=mood(UINT32_MAX);command.session=43;command.epoch=UINT32_MAX;
    assert(Lighting_Accept(&hints,&command,UINT32_MAX-100));
    step(50);assert(view.mood==LIGHT_HAPPY && view.moodRemainingMs==2849);
    command.event=0;assert(Lighting_Accept(&hints,&command,60));
    command.epoch=0;command.event=1;assert(Lighting_Accept(&hints,&command,60));
    command=(LightCommand){.session=43,.epoch=0,.event=2,.kind=LIGHT_CLEAR};
    assert(Lighting_Accept(&hints,&command,70));
    command=mood(1);command.session=43;command.epoch=0;
    assert(!Lighting_Accept(&hints,&command,80)); /* old hint cannot revive after clear */

    uint8_t wire[20]={1,LIGHT_EXPRESSION,LIGHT_HAPPY,100,0xb8,0x0b,0,0,43,0,0,0,0,0,0,0,3,0,0,0};
    assert(Lighting_UnpackCommand(wire,sizeof(wire),&command) && command.ttlMs==3000);
    assert(!Lighting_UnpackCommand(wire,19,&command));
    wire[6]=1;assert(!Lighting_UnpackCommand(wire,20,&command));wire[6]=0;
    wire[2]=LIGHT_MOOD_COUNT;assert(!Lighting_UnpackCommand(wire,20,&command));wire[2]=LIGHT_CALM;
    wire[3]=101;assert(!Lighting_UnpackCommand(wire,20,&command));wire[3]=100;
    wire[4]=0x11;wire[5]=0x27;assert(!Lighting_UnpackCommand(wire,20,&command));
    uint8_t status[LIGHTING_STATUS_SIZE];Lighting_PackStatus(&hints,&view,pixels,80,status);
    assert(status[0]==1 && status[12]==43 && status[16]==80 && status[20]==pixels[0].r);
    puts("lighting: priorities, STOP/fault masking, TTL/epoch/event/session, wrap, battery hysteresis, fade/cap and protocol PASS");
}
