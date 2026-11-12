#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "bridge_codec.h"
#include "motion_status.h"
int main(void)
{
    motion_status_t s={.seq=255,.state=2,.error=9,
        .flags=MOTION_STATUS_FLAG_GOAL_IMU|MOTION_STATUS_FLAG_FAULT|
               MOTION_STATUS_FLAG_LEASE_EXPIRED|(4u<<MOTION_STATUS_FLAG_MODE_SHIFT),
        .xMm=INT32_MIN,.yMm=INT32_MIN,.thetaMdeg=INT32_MIN,.odometerMm=INT32_MIN,
        .goalDistanceMm=INT32_MIN,.goalTurnMdeg=INT32_MIN,.goalDurationMs=UINT32_MAX,
        .leftMmps=INT16_MIN,.rightMmps=INT16_MIN};
    uint8_t payload[MOTION_STATUS_SIZE];char json[512];
    assert(motion_status_pack(&s,payload)==0);
    int n=bridge_motion_to_json(payload,sizeof(payload),json,sizeof(json));
    assert(n>0 && n<480); /* space remains for bridge's session_id suffix */
    assert(strstr(json,"\"goal_heading_source\":\"imu\""));
    s.flags=0;assert(motion_status_pack(&s,payload)==0);
    assert(bridge_motion_to_json(payload,sizeof(payload),json,sizeof(json))>0);
    assert(strstr(json,"\"goal_heading_source\":\"encoder\""));
    uint8_t command[BRIDGE_CONTROL_CMD_SIZE],seq;bool ack;
    const char* stop="{\"seq\":42,\"cmd\":\"motion\",\"motion\":{\"mode\":0}}";
    assert(bridge_json_to_control_cmd(stop,strlen(stop),command,&seq,&ack)>0);
    assert(command[2]==0 && seq==42 && ack);
    /* Same golden 29-byte frame as the STM32 packing test. */
    const uint8_t battery[29]={0x78,0x56,0x34,0x12,5,0xe0,0x2e,0xec,0x2e,
        0xe0,0x2e,0,0,0x78,0,0,0,0xa1,5,0,0,85,7,0,64,100,0,0xec,0x2e};
    assert(bridge_sensor_to_json(battery,sizeof(battery),json,sizeof(json))>0);
    assert(strstr(json,"\"type\":\"battery\""));
    assert(strstr(json,"\"battery_mv\":12012"));
    assert(strstr(json,"\"power_mw\":1441"));
    assert(strstr(json,"\"soc_percent\":85"));
    assert(bridge_sensor_to_json(battery,28,json,sizeof(json))==0);
    assert(bridge_sensor_to_json(battery,29,json,24)==0);
    uint8_t charging[29];memcpy(charging,battery,sizeof(charging));
    charging[13]=0x88;charging[14]=charging[15]=charging[16]=0xff; /* -120mA */
    assert(bridge_sensor_to_json(charging,29,json,sizeof(json))>0);
    assert(strstr(json,"\"current_ma\":-120"));
    puts("motion JSON: source flags, maximum values and STOP recognition passed");
    uint8_t visual[BRIDGE_LIGHTING_CMD_SIZE];
    const char *happy="{\"cmd\":\"lighting\",\"emotion\":\"happy\",\"intensity\":100,\"duration_ms\":3000,\"epoch\":4294967295}";
    assert(bridge_json_is_lighting(happy,strlen(happy)));
    assert(bridge_json_to_lighting(happy,strlen(happy),0x12345678,9,visual)==20);
    assert(visual[0]==1 && visual[1]==1 && visual[2]==1 && visual[3]==100);
    assert(visual[4]==0xb8 && visual[5]==0x0b && visual[8]==0x78 && visual[11]==0x12);
    assert(visual[12]==0xff && visual[15]==0xff && visual[16]==9);
    assert(!bridge_json_to_control_cmd(happy,strlen(happy),command,&seq,&ack));
    const char *bad[]={
        "{\"cmd\":\"lighting\",\"emotion\":\"fault\",\"intensity\":100,\"duration_ms\":3000,\"epoch\":0}",
        "{\"cmd\":\"lighting\",\"emotion\":\"happy\",\"intensity\":101,\"duration_ms\":3000,\"epoch\":0}",
        "{\"cmd\":\"lighting\",\"emotion\":\"happy\",\"intensity\":100,\"duration_ms\":10001,\"epoch\":0}",
        "{\"cmd\":\"lighting\",\"emotion\":\"happy\",\"intensity\":100,\"duration_ms\":3000,\"epoch\":4294967296}",
        "{\"cmd\":\"lighting\",\"clear\":1,\"epoch\":0}"};
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i)
        assert(!bridge_json_to_lighting(bad[i],strlen(bad[i]),1,1,visual));
    const char *clear="{\"cmd\":\"lighting\",\"clear\":true,\"epoch\":0}";
    assert(bridge_json_to_lighting(clear,strlen(clear),1,10,visual)==20 && visual[1]==4 && !visual[4]);
    uint8_t lightStatus[44]={1,0,1,0,4,100,0xb8,0x0b,1,0,0,0,1};
    assert(bridge_lighting_to_json(lightStatus,44,json,sizeof(json))>0);
    assert(strstr(json,"\"type\":\"lighting\"") && strstr(json,"\"emotion\":\"happy\""));
    assert(!bridge_lighting_to_json(lightStatus,43,json,sizeof(json)));
    assert(!bridge_lighting_to_json(lightStatus,44,json,20));
    lightStatus[2]=5;assert(!bridge_lighting_to_json(lightStatus,44,json,sizeof(json)));
    puts("lighting JSON: semantic bounds, independent frame, session/epoch/event and status passed");
}
