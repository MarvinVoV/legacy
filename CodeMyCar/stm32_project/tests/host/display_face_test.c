#include "display_face.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static uint16_t fb[FACE_WIDTH*FACE_HEIGHT],before[FACE_WIDTH*FACE_HEIGHT];
static void step(FaceModel *f,FaceRequest *r,uint32_t begin,uint32_t duration) {
    for (uint32_t t=20;t<=duration;t+=20) Face_Update(f,r,begin+t);
}
static void validate_dirty(FaceModel *f) {
    memcpy(before,fb,sizeof(fb));
    FaceRect dirty[3];size_t count=Face_Render(f,fb,dirty);
    assert(count<=3);
    for (size_t i=0;i<count;i++) {
        assert(dirty[i].w>0 && dirty[i].h>0);
        assert(dirty[i].x+dirty[i].w<=240 && dirty[i].y+dirty[i].h<=240);
    }
    for (unsigned y=0;y<240;y++) for (unsigned x=0;x<240;x++) {
        if (fb[y*240+x]==before[y*240+x]) continue;
        bool covered=false;
        for (size_t i=0;i<count;i++) {
            FaceRect d=dirty[i];
            covered|=x>=d.x && x<d.x+d.w && y>=d.y && y<d.y+d.h;
        }
        assert(covered);
    }
    assert(Face_Render(f,fb,dirty)==0); /* identical image sends zero SPI bytes */
}
static void test_animation(void) {
    FaceModel f;FaceRequest r={0};Face_Init(&f,0);
    Face_Update(&f,&r,3200);assert(f.blink==0);
    Face_Update(&f,&r,3265);assert(f.blink>.99f);
    Face_Update(&f,&r,3380);assert(!f.blink && !f.blinking);
    r.manual_gaze=true;r.gaze_x=1;r.gaze_y=-1;
    r.manual_open=true;r.openness=.3f;
    r.manual_color=true;r.color=0xf800;
    step(&f,&r,3380,4000);
    assert(f.gaze_x>.99f && f.gaze_y<-.99f);
    assert(fabsf(f.blink-.7f)<.001f && f.red>254 && f.green<.01f && f.blue<.01f);
    r.gaze_x=NAN;r.gaze_y=INFINITY;r.openness=NAN;r.mood=255;
    Face_Update(&f,&r,7400);
    assert(isfinite(f.gaze_x) && isfinite(f.gaze_y) && isfinite(f.blink));
    Face_Init(&f,UINT32_MAX-100u);
    Face_Update(&f,NULL,50u);assert(isfinite(f.gaze_x));
    Face_Update(&f,NULL,3200u);assert(f.blinking); /* scheduled blink across tick wrap */
    /* Same elapsed time, different frame cadence -> same smoothing. */
    FaceModel a,b;Face_Init(&a,0);Face_Init(&b,0);r=(FaceRequest){.mood=4,.manual_gaze=true,.gaze_x=1};
    step(&a,&r,0,1000);
    for (unsigned t=50;t<=1000;t+=50) Face_Update(&b,&r,t);
    assert(fabsf(a.gaze_x-b.gaze_x)<.0001f && fabsf(a.height_left-b.height_left)<.0001f);
}
static void test_rendering(void) {
    FaceModel f;Face_Init(&f,0);FaceRequest r={0};
    uint32_t now=0;
    for (unsigned m=0;m<5;m++) {
        r.mood=m;
        for (unsigned i=0;i<80;i++) {
            now+=50;Face_Update(&f,&r,now);validate_dirty(&f);
        }
    }
    for (unsigned i=0;i<20;i++) {
        r.manual_gaze=true;r.gaze_x=i&1 ? -1:1;r.gaze_y=i&2 ? -1:1;
        r.manual_open=true;r.openness=(float)(i%5)/4;
        now+=150;Face_Update(&f,&r,now);validate_dirty(&f);
    }
    assert(fb[0]==0 && fb[239*240+239]==0);
    Face_Init(&f,0);r=(FaceRequest){.manual_color=true,.color=0xf800,.manual_gaze=true};
    step(&f,&r,0,2000);FaceRect dirty[3];Face_Render(&f,fb,dirty);
    uint16_t wire=fb[116*240+70];
    uint16_t native=(uint16_t)((wire>>8)|(wire<<8));
    assert((native&0xf800)>0 && (native&0x07ff)==0); /* SPI big-endian RGB565 */
}
static void write_ppm(const char *path) {
    FILE *out=fopen(path,"wb");assert(out);
    fprintf(out,"P6\n240 240\n255\n");
    for (unsigned i=0;i<240*240;i++) {
        uint16_t p=(uint16_t)((fb[i]>>8)|(fb[i]<<8));
        unsigned char rgb[3]={(unsigned char)(((p>>11)&31)*255/31),
                              (unsigned char)(((p>>5)&63)*255/63),
                              (unsigned char)((p&31)*255/31)};
        assert(fwrite(rgb,1,3,out)==3);
    }
    fclose(out);
}
int main(int argc,char **argv) {
    if (argc>1) {
        const char *names[]={"calm","happy","curious","comforting","sad"};
        FaceRect dirty[3];char path[1024];
        for (unsigned m=0;m<5;m++) {
            FaceModel f;Face_Init(&f,0);FaceRequest r={.mood=m,.manual_gaze=true};
            memset(fb,0,sizeof(fb));step(&f,&r,0,1600);Face_Render(&f,fb,dirty);
            snprintf(path,sizeof(path),"%s/%s.ppm",argv[1],names[m]);write_ppm(path);
        }
        FaceModel f;Face_Init(&f,0);FaceRequest r={0};memset(fb,0,sizeof(fb));
        for (unsigned i=0;i<220;i++) {
            r.mood=(uint8_t)((i/44)%5);Face_Update(&f,&r,i*50u);Face_Render(&f,fb,dirty);
            snprintf(path,sizeof(path),"%s/frame-%03u.ppm",argv[1],i);write_ppm(path);
        }
    }
    test_animation();test_rendering();puts("display face: animation, overrides, wrap, RGB565 and dirty coverage passed");
    return 0;
}
