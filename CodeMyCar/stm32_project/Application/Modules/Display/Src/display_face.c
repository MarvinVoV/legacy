#include "display_face.h"
#include <math.h>
#include <string.h>

typedef struct { float left, right, tilt, smile, r, g, b; } Mood;
static const Mood moods[5] = {
    {38, 38,  0.00f, 0.22f,  62, 225, 245}, /* calm: clear cyan */
    {28, 28,  0.00f, 1.00f, 255, 199,  74}, /* happy: smiling gold crescents */
    {43, 30,  0.00f, 0.05f, 112, 190, 255}, /* curious: asymmetrical */
    {29, 29,  0.08f, 0.55f, 157, 222, 205}, /* comforting: soft mint */
    {30, 30, -0.28f,-0.60f, 111, 144, 226}, /* sad: inner corners raised */
};
static float clamp(float v, float lo, float hi) {
    return isfinite(v) ? fminf(hi, fmaxf(lo, v)) : lo;
}
static float mix(float a, float b, float k) { return a + (b-a)*k; }
static uint32_t random_next(FaceModel *f) {
    f->rng = f->rng * 1664525u + 1013904223u; return f->rng;
}
static bool reached(uint32_t now, uint32_t due) { return (int32_t)(now-due) >= 0; }
static uint16_t pixel(float r, float g, float b) {
    uint16_t value = (uint16_t)(((uint16_t)clamp(r,0,255)>>3)<<11 |
                              ((uint16_t)clamp(g,0,255)>>2)<<5 |
                              ((uint16_t)clamp(b,0,255)>>3));
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
    const Mood *m=&moods[request->mood<5u ? request->mood : 0u];
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
    float gx=request->manual_gaze ? clamp(request->gaze_x,-1,1) : f->target_x;
    float gy=request->manual_gaze ? clamp(request->gaze_y,-1,1) : f->target_y;
    f->gaze_x=mix(f->gaze_x,gx,gaze_k); f->gaze_y=mix(f->gaze_y,gy,gaze_k);
    f->height_left=mix(f->height_left,m->left,mood_k);
    f->height_right=mix(f->height_right,m->right,mood_k);
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
/* Distance to a rounded rectangle. Only rounded corners need a square root. */
static float rounded(float x, float y, float hw, float hh, float radius) {
    float qx=fabsf(x)-(hw-radius), qy=fabsf(y)-(hh-radius);
    float ox=fmaxf(qx,0), oy=fmaxf(qy,0);
    return (ox>0 && oy>0 ? sqrtf(ox*ox+oy*oy) : fmaxf(ox,oy)) +
           fminf(fmaxf(qx,qy),0)-radius;
}
static uint16_t eye_pixel(const FaceModel *f, int x, int y, unsigned eye) {
    float cx=(eye ? 170.0f : 70.0f)+f->gaze_x*7.0f;
    float cy=116.0f+f->gaze_y*5.0f;
    float dx=(float)x-cx, dy=(float)y-cy;
    float hh=(eye ? f->height_right : f->height_left)*(1.0f-f->blink)+2.0f*f->blink;
    dy-=f->tilt*dx*(eye ? -1.0f : 1.0f);
    float d=rounded(dx,dy,31.0f,hh,fminf(18.0f,hh));
    /* Happy expression lifts the lower lid to form an unmistakable smiling arc. */
    float happy=clamp((f->smile-0.65f)/0.35f,0,1)*(1.0f-f->blink);
    if (happy>0) {
        float lower=-12.0f+0.019f*dx*dx;
        d=fmaxf(d,(dy-lower)*happy-(1.0f-happy)*80.0f);
    }
    if (d>5.0f) return 0;
    float coverage=clamp(0.6f-d,0,1);
    float glow=clamp((5.0f-d)*0.018f,0,0.09f);
    float shade=clamp(0.90f-0.20f*dy/45.0f,0.65f,1.12f);
    float r=f->red*shade,g=f->green*shade,b=f->blue*shade;
    /* A small soft reflection, not a textured iris: readable at 240 pixels. */
    float hx=(dx+12.0f)/17.0f,hy=(dy+hh*0.45f)/7.0f;
    float highlight=clamp(1.0f-hx*hx-hy*hy,0,1)*0.30f*(1.0f-f->blink);
    r=mix(r,255,highlight);g=mix(g,255,highlight);b=mix(b,255,highlight);
    return pixel(r*coverage+f->red*glow*(1-coverage),
                 g*coverage+f->green*glow*(1-coverage),
                 b*coverage+f->blue*glow*(1-coverage));
}
static uint16_t mouth_pixel(const FaceModel *f,int x,int y) {
    float dx=(float)x-120.0f;
    if (fabsf(dx)>18.5f) return 0;
    float line=184.0f+f->smile*7.0f*(1.0f-dx*dx/(18.5f*18.5f));
    float a=clamp(1.5f-fabsf((float)y-line),0,1)*0.65f;
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
        for (unsigned y=z.y;y<z.y+z.h;y++) {
            for (unsigned x=z.x;x<z.x+z.w;x++) {
                uint16_t p=i<2 ? eye_pixel(f,(int)x,(int)y,i) : mouth_pixel(f,(int)x,(int)y);
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
