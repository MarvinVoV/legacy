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
}
