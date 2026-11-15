#include "display_face.h"
#include "visual_phase.h"
#include <math.h>
#include <string.h>

typedef struct { float left, right, tilt, smile, r, g, b; } Mood;
static const Mood moods[LIGHT_MOOD_COUNT] = {
    {38, 38,  0.00f, 0.22f,  62, 225, 245}, /* calm: clear cyan */
    {28, 28,  0.00f, 1.00f, 255, 199,  74}, /* happy: smiling gold crescents */
    {43, 30,  0.00f, 0.05f, 112, 190, 255}, /* curious: asymmetrical */
    {29, 29,  0.08f, 0.55f, 157, 222, 205}, /* comforting: soft mint */
    {30, 30, -0.28f,-0.60f, 111, 144, 226}, /* sad: inner corners raised */
    {36, 36,  0.06f, 0.56f, 255, 154, 64}, /* excited amber */
};
static float clamp(float v, float lo, float hi) {
    return isfinite(v) ? fminf(hi, fmaxf(lo, v)) : lo;
}
static float mix(float a, float b, float k) { return a + (b-a)*k; }
static uint32_t random_next(FaceModel *f) {
    f->rng = f->rng * 1664525u + 1013904223u; return f->rng;
}
static bool reached(uint32_t now, uint32_t due) { return (int32_t)(now-due) >= 0; }
/* All render inputs are finite after Face_Update validates the request. */
static inline float bound(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}
static uint16_t pixel(float r, float g, float b) {
    uint16_t value = (uint16_t)(((uint16_t)bound(r,0,255)>>3)<<11 |
                              ((uint16_t)bound(g,0,255)>>2)<<5 |
                              ((uint16_t)bound(b,0,255)>>3));
    return (uint16_t)((value>>8) | (value<<8));
}
void Face_Init(FaceModel *f, uint32_t now) {
    memset(f,0,sizeof(*f));
    f->last_ms=now; f->next_look_ms=now+1800u; f->next_blink_ms=now+3200u;
    f->rng=0x58494159u; f->height_left=f->height_right=38;
    f->smile=moods[0].smile; f->red=62; f->green=225; f->blue=245;
}
void Face_Update(FaceModel *f, const FaceRequest *request, uint32_t now) {
    const FaceRequest empty={0};
    if (!request) request=&empty;
    const uint8_t mood=request->mood<LIGHT_MOOD_COUNT ? request->mood : LIGHT_CALM;
    const Mood *m=&moods[mood];
    if (f->mood!=mood) { f->mood=mood; f->mood_since_ms=now; }
    if (f->voice!=request->voice) { f->voice=request->voice; f->voice_since_ms=now; }
    uint32_t elapsed=now-f->last_ms; f->last_ms=now;
    if (elapsed>200u) elapsed=200u; /* wake/resume never makes a large jump */
    const float gaze_k=1.0f-expf(-(float)elapsed/110.0f);
    const float mood_k=1.0f-expf(-(float)elapsed/180.0f);
    if (reached(now,f->next_look_ms)) {
        uint32_t n=random_next(f);
        f->target_x=((float)(n%1001u)/500.0f-1.0f)*0.75f;
        f->target_y=((float)((n>>12)%1001u)/500.0f-1.0f)*0.45f;
        f->next_look_ms=now+1600u+random_next(f)%2600u;
    }
    float x=f->target_x, y=f->target_y, lift=0;
    const uint32_t phase=now-f->voice_since_ms;
    switch (f->voice) {
    case LIGHT_VOICE_WAKE: x=0; y=-0.25f; lift=3; break;
    case LIGHT_VOICE_LISTEN: x=0; y=0; lift=1.5f; break;
    case LIGHT_VOICE_THINK: x=0.68f; y=-0.72f; break;
    case LIGHT_VOICE_SPEAK: {
        /* Two gentle beats followed by a rest. Calculated once per frame. */
        const unsigned beat=phase%1200u;
        if (beat<760u) lift=1.6f*(1.0f-cosf((float)(beat%380u)*0.016535f));
        x*=0.35f; y=-0.12f*lift;
        break;
    }
    default: break;
    }
    const uint32_t mood_age=now-f->mood_since_ms;
    if (mood==LIGHT_EXCITED && mood_age<720u) {
        const float bounce=1.0f-cosf((float)(mood_age%360u)*0.0174533f);
        lift+=1.7f*bounce;
        y-=0.22f*bounce;
    }
    float gx=request->manual_gaze ? clamp(request->gaze_x,-1,1) : x;
    float gy=request->manual_gaze ? clamp(request->gaze_y,-1,1) : y;
    f->gaze_x=mix(f->gaze_x,gx,gaze_k); f->gaze_y=mix(f->gaze_y,gy,gaze_k);
    f->height_left=mix(f->height_left,m->left+lift,mood_k);
    f->height_right=mix(f->height_right,m->right+lift,mood_k);
    f->tilt=mix(f->tilt,m->tilt,mood_k); f->smile=mix(f->smile,m->smile,mood_k);
    float r=m->r,g=m->g,b=m->b;
    if (request->manual_color) {
        r=(float)((request->color>>11)&31u)*255.0f/31.0f;
        g=(float)((request->color>>5)&63u)*255.0f/63.0f;
        b=(float)(request->color&31u)*255.0f/31.0f;
    }
    f->red=mix(f->red,r,mood_k); f->green=mix(f->green,g,mood_k); f->blue=mix(f->blue,b,mood_k);
    if (!f->blinking && reached(now,f->next_blink_ms)) {
        f->blinking=true; f->blink_start_ms=now;
    }
    float blink=0;
    if (f->blinking) {
        uint32_t age=now-f->blink_start_ms;
        if (age>=180u) {
            f->blinking=false; f->next_blink_ms=now+2800u+random_next(f)%3100u;
        } else {
            float t=age<65u ? (float)age/65.0f : 1.0f-(float)(age-65u)/115.0f;
            blink=t*t*(3.0f-2.0f*t);
        }
    }
    f->blink=request->manual_open ? 1.0f-clamp(request->openness,0,1) : blink;
}
/* Timer deadlines and RNG changes can leave every visible pixel unchanged. */
bool Face_SameAppearance(const FaceModel *a, const FaceModel *b) {
    return a->gaze_x == b->gaze_x && a->gaze_y == b->gaze_y && a->blink == b->blink &&
           a->height_left == b->height_left && a->height_right == b->height_right &&
           a->tilt == b->tilt && a->smile == b->smile && a->red == b->red &&
           a->green == b->green && a->blue == b->blue;
}

typedef struct {
    float cx, cy, hh, radius, happy;
    float tilt_sign;
} EyeStyle;

static EyeStyle eye_style(const FaceModel *f, unsigned eye) {
    float hh = (eye ? f->height_right : f->height_left)*(1.0f-f->blink)+2.0f*f->blink;
    return (EyeStyle){
        .cx=(eye ? 170.0f : 70.0f)+f->gaze_x*7.0f,
        .cy=116.0f+f->gaze_y*5.0f,
        .hh=hh, .radius=fminf(18.0f,hh),
        .happy=bound((f->smile-0.65f)/0.35f,0,1)*(1.0f-f->blink),
        .tilt_sign=eye ? -1.0f : 1.0f
    };
}

typedef struct {
    float abs_dx, offset_y, qx, highlight_x2, lower_lid;
} EyeColumn;

static inline EyeColumn eye_column(const FaceModel *f, const EyeStyle *style, unsigned x) {
    float dx=(float)x-style->cx;
    float hx=(dx+12.0f)/17.0f;
    return (EyeColumn){fabsf(dx), f->tilt*dx*style->tilt_sign,
                       fabsf(dx)-(31.0f-style->radius), hx*hx, -12.0f+0.019f*dx*dx};
}

static inline uint16_t eye_pixel(const FaceModel *f, const EyeStyle *style, const EyeColumn *col, float dy) {
    const float hh=style->hh;
    const float happy=style->happy;
    /* Outside the rounded rectangle plus its five-pixel glow the original
     * shader is black. Still compare/write black to clear the previous pose. */
    if (col->abs_dx>36.01f || fabsf(dy)>hh+5.01f) return 0;
    float qx=col->qx, qy=fabsf(dy)-(hh-style->radius);
    float qmax=qx>qy ? qx : qy;
    float d=(qx>0 && qy>0 ? sqrtf(qx*qx+qy*qy) : (qmax>0 ? qmax : 0)) +
            (qmax<0 ? qmax : 0)-style->radius;
    /* Happy expression lifts the lower lid to form an unmistakable smiling arc. */
    if (happy>0) {
        d=fmaxf(d,(dy-col->lower_lid)*happy-(1.0f-happy)*80.0f);
    }
    if (d>5.0f) return 0;
    float coverage=bound(0.6f-d,0,1);
    float glow=bound((5.0f-d)*0.018f,0,0.09f);
    float shade=bound(0.90f-0.20f*dy/45.0f,0.65f,1.12f);
    float r=f->red*shade,g=f->green*shade,b=f->blue*shade;
    /* A small soft reflection, not a textured iris: readable at 240 pixels. */
    float hy=(dy+hh*0.45f)/7.0f;
    float highlight=bound(1.0f-col->highlight_x2-hy*hy,0,1)*0.30f*(1.0f-f->blink);
    r=mix(r,255,highlight);g=mix(g,255,highlight);b=mix(b,255,highlight);
    return pixel(r*coverage+f->red*glow*(1-coverage),
                 g*coverage+f->green*glow*(1-coverage),
                 b*coverage+f->blue*glow*(1-coverage));
}
static uint16_t mouth_pixel(const FaceModel *f,int x,int y) {
    float dx=(float)x-120.0f;
    if (fabsf(dx)>18.5f) return 0;
    float line=184.0f+f->smile*7.0f*(1.0f-dx*dx/(18.5f*18.5f));
    float a=bound(1.5f-fabsf((float)y-line),0,1)*0.65f;
    return pixel(f->red*a,f->green*a,f->blue*a);
}
size_t Face_Render(const FaceModel *f,uint16_t *fb,FaceRect dirty[FACE_REGION_COUNT]) {
    /* Fixed clear zones contain every possible gaze, mood, glow and blink shape.
     * Tracking actual changed pixels shrinks the transfer during small movements. */
    static const FaceRect zones[3]={{23,58,94,113},{123,58,94,113},{100,177,40,18}};
    size_t count=0;
    for (unsigned i=0;i<3;i++) {
        const FaceRect z=zones[i];
        unsigned minx=FACE_WIDTH,miny=FACE_HEIGHT,maxx=0,maxy=0;
        const EyeStyle style = eye_style(f,i);
        for (unsigned x=z.x;x<z.x+z.w;x++) {
            const EyeColumn col=eye_column(f,&style,x);
            for (unsigned y=z.y;y<z.y+z.h;y++) {
                float dy=(float)y-style.cy-col.offset_y;
                uint16_t p=i<2 ? eye_pixel(f,&style,&col,dy) : mouth_pixel(f,(int)x,(int)y);
                size_t at=y*FACE_WIDTH+x;
                if (fb[at]!=p) {
                    fb[at]=p;
                    if (x<minx) minx=x;
                    if (x>maxx) maxx=x;
                    if (y<miny) miny=y;
                    if (y>maxy) maxy=y;
                }
            }
        }
        if (minx<FACE_WIDTH) dirty[count++]=(FaceRect){minx,miny,maxx-minx+1,maxy-miny+1};
    }
    return count;
}
