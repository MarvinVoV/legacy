#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ws2812_encoder.h"
#include "lighting_service.h"

int main(void)
{
    Ws2812Timing timing;
    assert(!Ws2812_Timing(0,&timing));
    assert(!Ws2812_Timing(1000000,&timing));
    assert(Ws2812_Timing(250000000,&timing));
    assert(timing.periodTicks==313 && timing.zeroTicks==95 && timing.oneTicks==188);
    assert((timing.resetSlots-2u)*timing.periodTicks*4u>=400000u);
    LedRgb pixels[6]={{.r=0x80,.g=0x01,.b=0xff}};
    uint32_t words[WS2812_BUFFER_WORDS];
    const size_t count=Ws2812_Encode(pixels,&timing,words,WS2812_BUFFER_WORDS);
    assert(count==144u+timing.resetSlots);
    for (unsigned i=0; i<7; ++i) assert(words[i]==95);
    assert(words[7]==188);
    assert(words[8]==188);
    for (unsigned i=9; i<16; ++i) assert(words[i]==95);
    for (unsigned i=16; i<24; ++i) assert(words[i]==188);
    for (unsigned i=24; i<144; ++i) assert(words[i]==95);
    for (size_t i=144; i<count; ++i) assert(words[i]==0);
    assert(!Ws2812_Encode(pixels,&timing,words,count-1));
    assert(!Ws2812_Encode(NULL,&timing,words,count));
    uint32_t preload=0;
    for (size_t update=0; update<count; ++update) {
        uint32_t active=preload;
        if (update>0) assert(active==words[update-1]);
        preload=words[update];
    }
    for (uint32_t ms=0; ms<11000; ms+=10) {
        Lighting_RenderBringup(ms,pixels);
        for (unsigned i=0; i<6; ++i)
            assert(pixels[i].r<=12 && pixels[i].g<=12 && pixels[i].b<=12);
    }
    Lighting_RenderBringup(500,pixels);assert(pixels[0].r==12 && pixels[0].g==0);
    Lighting_RenderBringup(1500,pixels);assert(pixels[0].g==12 && pixels[0].b==0);
    Lighting_RenderBringup(2500,pixels);assert(pixels[0].b==12 && pixels[0].r==0);
    for (unsigned position=0; position<6; ++position) {
        Lighting_RenderBringup(3300u+400u*position,pixels);
        for (unsigned i=0; i<6; ++i)
            assert(pixels[i].r==(i==position?8:0));
    }
    puts("WS2812: GRB/MSB, first/last bit, reset margin, capacity, RGB sequence and brightness cap passed");
}
