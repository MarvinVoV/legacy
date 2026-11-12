#include "lighting_service.h"
#include <string.h>

static uint16_t read16(const uint8_t *p) { return (uint16_t)(p[0]|((uint16_t)p[1]<<8)); }
static uint32_t read32(const uint8_t *p) { return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static void write16(uint8_t *p,uint16_t v) { p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8); }
static void write32(uint8_t *p,uint32_t v) { for (unsigned i=0;i<4;++i) p[i]=(uint8_t)(v>>(8u*i)); }
static bool newer(uint32_t a,uint32_t b) { const uint32_t delta=a-b;return delta && delta<0x80000000u; }

static bool valid(const LightCommand *c)
{
    if (!c || !c->session || c->intensity>100u) return false;
    switch (c->kind) {
    case LIGHT_EXPRESSION: return c->value<LIGHT_MOOD_COUNT && c->ttlMs>=100u && c->ttlMs<=10000u;
    case LIGHT_VOICE: return c->value<LIGHT_VOICE_COUNT && c->ttlMs>=100u && c->ttlMs<=1500u;
    case LIGHT_LEVEL: return c->value<=100u && c->ttlMs>=100u && c->ttlMs<=300u;
    case LIGHT_CLEAR: return !c->value && !c->intensity && !c->ttlMs;
    default: return false;
    }
}

bool Lighting_UnpackCommand(const uint8_t *p,size_t size,LightCommand *out)
{
    if (!p || !out || size!=LIGHTING_COMMAND_SIZE || p[0]!=1u || p[6] || p[7]) return false;
    LightCommand c={.kind=p[1],.value=p[2],.intensity=p[3],.ttlMs=read16(p+4),
                    .session=read32(p+8),.epoch=read32(p+12),.event=read32(p+16)};
    if (!valid(&c)) return false;
    *out=c;return true;
}

void Lighting_NewSession(LightHints *h,uint32_t session)
{
    if (!h || !session || h->session==session) return;
    const uint16_t accepted=h->accepted,rejected=h->rejected;
    *h=(LightHints){.session=session,.accepted=accepted,.rejected=rejected};
}

bool Lighting_Accept(LightHints *h,const LightCommand *c,uint32_t now)
{
    if (!h) return false;
    if (!valid(c) || c->session!=h->session ||
        (h->epochKnown && c->epoch!=h->epoch && !newer(c->epoch,h->epoch))) goto reject;
    if (!h->epochKnown || c->epoch!=h->epoch) {
        h->epoch=c->epoch;h->epochKnown=true;
        h->mood=(LightHint){0};h->voice=(LightHint){0};h->level=(LightHint){0};
    }
    if (c->kind==LIGHT_CLEAR) {
        if ((h->mood.seen && !newer(c->event,h->mood.event)) ||
            (h->voice.seen && !newer(c->event,h->voice.event)) ||
            (h->level.seen && !newer(c->event,h->level.event))) goto reject;
        h->mood=h->voice=h->level=(LightHint){.event=c->event,.seen=true};
    } else {
        LightHint *slot=c->kind==LIGHT_EXPRESSION ? &h->mood : c->kind==LIGHT_VOICE ? &h->voice : &h->level;
        if (slot->seen && !newer(c->event,slot->event)) goto reject;
        *slot=(LightHint){.value=c->value,.intensity=c->intensity,.ttlMs=c->ttlMs,
                         .receivedMs=now,.event=c->event,.seen=true};
    }
    ++h->accepted;return true;
reject:
    ++h->rejected;return false;
}

static uint16_t remaining(const LightHint *hint,uint32_t now)
{
    const uint32_t age=now-hint->receivedMs;
    return hint->seen && age<hint->ttlMs ? (uint16_t)(hint->ttlMs-age) : 0u;
}
static unsigned triangle(uint32_t time,uint32_t period)
{
    const uint32_t phase=time%period,half=period/2u;
    return (phase<half ? phase : period-phase)*255u/half;
}
static LedRgb scale(LedRgb color,unsigned value)
{
    return (LedRgb){(uint8_t)(color.r*value/255u),(uint8_t)(color.g*value/255u),(uint8_t)(color.b*value/255u)};
}
static void battery_update(LightRenderer *r,const LightLocal *local,uint32_t now)
{
    if (!local->batteryValid || now-local->batteryMs>2000u || local->batteryPercent>100u) {
        r->batteryTimer=false;r->lowBattery=false;return;
    }
    const bool recover=r->lowBattery;
    const bool condition=recover ? local->batteryPercent>=25u : local->batteryPercent<=20u;
    if (!condition) { r->batteryTimer=false;return; }
    if (!r->batteryTimer || r->recoveringBattery!=recover) {
        r->batteryTimer=true;r->recoveringBattery=recover;r->batterySinceMs=now;
    }
    if (now-r->batterySinceMs >= (recover ? 10000u : 5000u)) {
        r->lowBattery=!recover;r->batteryTimer=false;
    }
}

void Lighting_Render(LightRenderer *r,const LightHints *h,const LightLocal *local,
                     uint32_t now,LedRgb out[WS2812_LED_COUNT],LightView *view)
{
    if (!r || !h || !local || !out || !view) return;
    const bool fresh=local->motionValid && now-local->sampledMs<=250u;
    const bool moving=fresh && (local->speedMmps>15 || local->speedMmps<-15);
    const bool braking=fresh && (local->braking || (local->stopRequested && r->wasMoving));
    if (r->ready && ((braking && !r->wasBraking) || (r->wasMoving && !moving))) {
        r->brakeMs=now;r->brakeActive=true;
    }
    if (r->brakeActive && now-r->brakeMs>=400u) r->brakeActive=false;
    r->wasMoving=moving;r->wasBraking=braking;
    battery_update(r,local,now);

    const uint16_t moodTtl=remaining(&h->mood,now);
    const uint8_t mood=moodTtl ? h->mood.value : LIGHT_CALM;
    const bool voiceActive=remaining(&h->voice,now)!=0u;
    const uint8_t voice=voiceActive ? h->voice.value : LIGHT_VOICE_IDLE;
    const unsigned strength=moodTtl ? h->mood.intensity : 100u;
    const uint8_t key=(uint8_t)(voice*LIGHT_MOOD_COUNT+mood);
    if (!r->ready || key!=r->baseKey) { r->phaseMs=now;r->baseKey=key; }
    const uint32_t phase=now-r->phaseMs;
    static const LedRgb palette[LIGHT_MOOD_COUNT]={
        {0,240,150},{255,190,35},{30,190,255},{255,135,170},{85,110,180}};
    unsigned breath=100u+triangle(phase,mood==LIGHT_HAPPY ? 2200u : 4000u)*155u/255u;
    LedRgb base=scale(palette[mood],breath*strength/100u);
    LedRgb target[WS2812_LED_COUNT];
    for (unsigned i=0;i<WS2812_LED_COUNT;++i) target[i]=base;
    if (voice!=LIGHT_VOICE_IDLE) {
        for (unsigned i=0;i<WS2812_LED_COUNT;++i) {
            unsigned level=80u+triangle(phase+((i<3u ? i : 5u-i)*180u),1800u)*175u/255u;
            LedRgb color={30,140,255};
            if (voice==LIGHT_VOICE_WAKE) {
                color=(LedRgb){255,210,65};
                const unsigned distance=i<3u ? 2u-i : i-3u;
                level=(phase<600u && phase<distance*130u) ? 0u : 210u;
            } else if (voice==LIGHT_VOICE_THINK) {
                color=(LedRgb){180,75,255};level=((phase/220u)%6u==i) ? 255u : 70u;
            } else if (voice==LIGHT_VOICE_SPEAK) {
                color=(LedRgb){255,225,190};
                level=remaining(&h->level,now) ? 65u+h->level.value*190u/100u : 140u;
            }
            target[i]=scale(color,level*h->voice.intensity/100u);
        }
    }
    LightMode mode=LIGHT_IDLE;
    if (moving) {
        mode=local->speedMmps>0 ? LIGHT_FORWARD : LIGHT_REVERSE;
        target[0]=target[5]=local->speedMmps>0 ? (LedRgb){15,210,45}
                                              : scale((LedRgb){255,220,175},100u+triangle(now,1800u)*155u/255u);
    }
    if (fresh && local->steering && !local->stopRequested) {
        mode=local->steering<0 ? LIGHT_LEFT : LIGHT_RIGHT;
        const unsigned slot=(now/170u)%4u;
        for (unsigned distance=0;distance<3;++distance) {
            const unsigned pixel=local->steering<0 ? 2u-distance : 3u+distance;
            target[pixel]=slot<3u && distance<=slot ? (LedRgb){255,155,0} : (LedRgb){0};
        }
    }
    const bool warning=!fresh || r->lowBattery;
    if (warning && !moving && mode==LIGHT_IDLE) {
        mode=LIGHT_WARNING;
        target[0]=target[5]=scale((LedRgb){255,160,0},100u+triangle(now,3000u)*155u/255u);
    }
    if (r->brakeActive || braking) {
        mode=LIGHT_BRAKE;target[0]=target[5]=(LedRgb){255,0,0};
    }
    if (local->fault) {
        mode=LIGHT_FAULT;
        for (unsigned i=0;i<WS2812_LED_COUNT;++i)
            target[i]=scale((LedRgb){255,0,0},150u+triangle(now,2400u)*105u/255u);
    }

    uint32_t dt=r->ready ? now-r->lastMs : 0u;
    if (dt>250u) dt=250u;
    for (unsigned i=0;i<WS2812_LED_COUNT;++i) {
        const uint8_t colors[3]={target[i].r,target[i].g,target[i].b};
        uint8_t final[3];
        for (unsigned j=0;j<3;++j) {
            const int32_t desired=(int32_t)colors[j]*256;
            if (!r->ready || mode==LIGHT_FAULT || (mode==LIGHT_BRAKE && (i==0u || i==5u)))
                r->smooth[i][j]=(uint16_t)desired;
            else r->smooth[i][j]=(uint16_t)((int32_t)r->smooth[i][j]+
                         (desired-r->smooth[i][j])*(int32_t)dt/(int32_t)(120u+dt));
            const uint32_t linear=r->smooth[i][j]/256u;
            /* Approximate gamma 2, with a single final current/brightness cap. */
            final[j]=(uint8_t)((linear*linear*LIGHTING_MAX_CHANNEL+32512u)/65025u);
        }
        out[i]=(LedRgb){final[0],final[1],final[2]};
    }
    r->ready=true;r->lastMs=now;
    *view=(LightView){.mode=(uint8_t)mode,.mood=mood,.voice=voice,.intensity=(uint8_t)strength,
        .moodRemainingMs=moodTtl,.flags=(uint8_t)((r->lowBattery?1u:0u)|(!fresh?2u:0u)|
                                               (moodTtl?4u:0u)|(voiceActive?8u:0u))};
}

void Lighting_PackStatus(const LightHints *h,const LightView *v,const LedRgb p[WS2812_LED_COUNT],
                         uint32_t now,uint8_t out[LIGHTING_STATUS_SIZE])
{
    memset(out,0,LIGHTING_STATUS_SIZE);
    out[0]=1;out[1]=v->mode;out[2]=v->mood;out[3]=v->voice;out[4]=v->flags;out[5]=v->intensity;
    write16(out+6,v->moodRemainingMs);write16(out+8,h->accepted);write16(out+10,h->rejected);
    write32(out+12,h->session);write32(out+16,now);write32(out+38,h->epoch);
    for (unsigned i=0;i<WS2812_LED_COUNT;++i) { out[20+i*3]=p[i].r;out[21+i*3]=p[i].g;out[22+i*3]=p[i].b; }
}
