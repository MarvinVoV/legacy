#include "sonar_config.h"
#include <assert.h>

int main(void)
{
    SonarConfig state = {.enabled=true};
    SonarRequest req;
    uint8_t set[SONAR_SET_SIZE] = {SONAR_SET, 0};
    sonar_write32(set+2, 0x12345678);
    sonar_write32(set+10, 1);
    sonar_write32(set+14, UINT32_MAX-20);
    assert(set[2]==0x78 && set[5]==0x12);
    assert(!sonar_decode_request(set,2,&req));
    set[1]=2;
    assert(!sonar_decode_request(set,sizeof(set),&req));
    set[1]=0;
    assert(sonar_decode_request(set,sizeof(set),&req));
    assert(!sonar_apply(&state,&req,10,true,true)); /* Unbound after reset. */
    uint8_t bind[]={SONAR_BIND,0x78,0x56,0x34,0x12};
    SonarRequest binding;
    assert(sonar_decode_request(bind,sizeof(bind),&binding));
    assert(!sonar_apply(&state,&binding,10,true,true));
    assert(!sonar_apply(&state,&req,10,true,false) && state.result==SONAR_BUSY);
    req.request_id++;
    assert(sonar_apply(&state,&req,10,true,true)); /* clock wrap, stationary */
    assert(!state.enabled && state.generation==1);
    assert(!sonar_apply(&state,&req,20,true,true)); /* Duplicate preserves reply. */
    req.request_id++;
    req.enable=1;
    assert(!sonar_apply(&state,&req,20,true,true) && state.result==SONAR_CONFLICT);
    req.generation=1;
    req.request_id++;
    assert(!sonar_apply(&state,&req,4000,true,true) && state.result==SONAR_EXPIRED);
    req.request_id++;
    assert(!sonar_apply(&state,&req,30,false,true) && state.result==SONAR_UNSUPPORTED);
    req.request_id++;
    assert(sonar_apply(&state,&req,30,true,false) && state.enabled);
    req.generation=state.generation;
    req.request_id++;
    assert(!sonar_apply(&state,&req,30,true,false) && state.result==SONAR_OK);
}
