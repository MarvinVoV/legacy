#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "bridge_codec.h"

static int failures = 0;
#define CHECK(cond, msg) do { if(!(cond)){ printf("FAIL: %s\n", msg); failures++; } else { printf("ok: %s\n", msg);} } while(0)

static void hex(const uint8_t* b, int n){ for(int i=0;i<n;i++) printf("%02X ",b[i]); printf("\n"); }

int main(void){
    uint8_t out[BRIDGE_CONTROL_CMD_SIZE];
    uint8_t seq; bool ack;

    /* 1. differential —— pyserver 真实 JSON（含 null 字段） */
    const char* j_diff = "{\"seq\":7,\"cmd\":\"motion\",\"motion\":{\"mode\":2,\"duration\":1000,"
        "\"params\":{\"left_rpm\":null,\"right_rpm\":null,\"linear_vel\":500,\"angular_vel\":100,"
        "\"steer_angle\":null,\"rotation_point\":null,\"duty_cycle\":null,\"gain\":null}}}";
    int n = bridge_json_to_control_cmd(j_diff, strlen(j_diff), out, &seq, &ack);
    CHECK(n==13, "differential returns 13 bytes");
    CHECK(seq==7, "differential seq=7");
    CHECK(ack==true, "differential ack_req");
    uint8_t exp_diff[13] = {0,0x01,2, 0xE8,0x03, 0xF4,0x01, 0x64,0x00, 0,0,0,0};
    CHECK(memcmp(out, exp_diff, 13)==0, "differential bytes match ControlCmd layout");
    if (memcmp(out,exp_diff,13)!=0) hex(out,13);

    /* 2. emergency_stop */
    const char* j_estop = "{\"seq\":0,\"cmd\":\"motion\",\"motion\":{\"mode\":0,\"duration\":0,\"params\":{}}}";
    n = bridge_json_to_control_cmd(j_estop, strlen(j_estop), out, &seq, &ack);
    uint8_t exp_estop[13] = {0,0x01,0,0,0, 0,0,0,0,0,0,0,0};
    CHECK(n==13 && memcmp(out,exp_estop,13)==0, "emergency_stop all-zero params");

    /* 3. direct: left=120 right=-120 steer=90 */
    const char* j_direct = "{\"seq\":1,\"cmd\":\"motion\",\"motion\":{\"mode\":1,\"duration\":0,"
        "\"params\":{\"left_rpm\":120,\"right_rpm\":-120,\"steer_angle\":90}}}";
    n = bridge_json_to_control_cmd(j_direct, strlen(j_direct), out, &seq, &ack);
    /* -120 = 0xFF88 -> LE 88 FF ; 120=0x0078->78 00 ; 90=0x005A->5A 00 */
    uint8_t exp_direct[13] = {0,0x01,1, 0,0, 0x78,0x00, 0x88,0xFF, 0x5A,0x00, 0,0};
    CHECK(n==13 && memcmp(out,exp_direct,13)==0, "direct bytes (incl negative rpm)");
    if (memcmp(out,exp_direct,13)!=0) hex(out,13);

    /* 4. spin: angular=50 rotation_point=1 */
    const char* j_spin = "{\"seq\":8,\"cmd\":\"motion\",\"motion\":{\"mode\":4,\"duration\":0,"
        "\"params\":{\"angular_vel\":50,\"rotation_point\":1}}}";
    n = bridge_json_to_control_cmd(j_spin, strlen(j_spin), out, &seq, &ack);
    uint8_t exp_spin[13] = {0,0x01,4, 0,0, 0x32,0x00, 0x01, 0,0,0,0,0};
    CHECK(n==13 && memcmp(out,exp_spin,13)==0, "spin bytes");
    if (memcmp(out,exp_spin,13)!=0) hex(out,13);

    /* 5. mixed velocity */
    const char* j_mixv = "{\"seq\":2,\"cmd\":\"motion\",\"motion\":{\"mode\":5,\"duration\":0,"
        "\"params\":{\"linear_vel\":300,\"angular_vel\":50,\"steer_angle\":45,\"gain\":80}}}";
    n = bridge_json_to_control_cmd(j_mixv, strlen(j_mixv), out, &seq, &ack);
    /* driveCtrlType=0(vel)@5, linear@6=300(2C 01), angular@8=50(32 00), steer@10=45(2D 00), gain@12=80(50) */
    uint8_t exp_mixv[13] = {0,0x01,5, 0,0, 0x00, 0x2C,0x01, 0x32,0x00, 0x2D,0x00, 0x50};
    CHECK(n==13 && memcmp(out,exp_mixv,13)==0, "mixed-velocity bytes");
    if (memcmp(out,exp_mixv,13)!=0) hex(out,13);

    /* 6. mixed duty */
    const char* j_mixd = "{\"seq\":3,\"cmd\":\"motion\",\"motion\":{\"mode\":5,\"duration\":0,"
        "\"params\":{\"duty_cycle\":500,\"steer_angle\":30,\"gain\":60}}}";
    n = bridge_json_to_control_cmd(j_mixd, strlen(j_mixd), out, &seq, &ack);
    /* driveCtrlType=1(duty)@5, left@6=500(F4 01), right@8=500(F4 01), steer@10=30(1E 00), gain@12=60(3C) */
    uint8_t exp_mixd[13] = {0,0x01,5, 0,0, 0x01, 0xF4,0x01, 0xF4,0x01, 0x1E,0x00, 0x3C};
    CHECK(n==13 && memcmp(out,exp_mixd,13)==0, "mixed-duty bytes");
    if (memcmp(out,exp_mixd,13)!=0) hex(out,13);

    /* 7. invalid JSON */
    const char* j_bad = "this is not json";
    n = bridge_json_to_control_cmd(j_bad, strlen(j_bad), out, &seq, &ack);
    CHECK(n==0, "invalid JSON rejected");

    /* 8. unknown mode rejected */
    const char* j_badmode = "{\"seq\":0,\"cmd\":\"motion\",\"motion\":{\"mode\":9,\"duration\":0,\"params\":{}}}";
    n = bridge_json_to_control_cmd(j_badmode, strlen(j_badmode), out, &seq, &ack);
    CHECK(n==0, "unknown mode rejected");

    /* 9. non-motion cmd rejected */
    const char* j_nonmotion = "{\"seq\":0,\"cmd\":\"reboot\",\"motion\":{\"mode\":0,\"duration\":0,\"params\":{}}}";
    n = bridge_json_to_control_cmd(j_nonmotion, strlen(j_nonmotion), out, &seq, &ack);
    CHECK(n==0, "non-motion cmd rejected");

    /* 10. sensor IMU -> JSON */
    float imu[6] = {1.5f, -2.0f, 9.8f, 0.1f, 0.2f, 0.3f};
    uint8_t sensor[29]; memset(sensor,0,sizeof(sensor));
    sensor[0]=0x10; sensor[1]=0x27; sensor[2]=0; sensor[3]=0; /* timestamp 10000 */
    sensor[4]=1; /* IMU */
    memcpy(&sensor[5], imu, 24);
    char jbuf[512];
    n = bridge_sensor_to_json(sensor, sizeof(sensor), jbuf, sizeof(jbuf));
    CHECK(n>0, "sensor->json produced");
    CHECK(strstr(jbuf,"\"type\":\"imu\"")!=NULL, "sensor json has type imu");
    CHECK(strstr(jbuf,"1500")!=NULL, "sensor json accel scaled x1000 (1.5->1500)");
    printf("sensor json: %s\n", jbuf);

    /* 11. log -> JSON（消息不含 NUL 终止，按长度传） */
    uint8_t logb[64]; memset(logb,0,sizeof(logb));
    logb[0]=0x88; logb[1]=0x13; /* ts low */
    logb[4]=0x02; logb[5]=0x00; /* module=motor(0x0002) */
    logb[6]=0x08; /* level=ERROR */
    strcpy((char*)&logb[7], "motor stall");
    n = bridge_log_to_json(logb, 7+strlen("motor stall"), jbuf, sizeof(jbuf));
    CHECK(n>0, "log->json produced");
    CHECK(strstr(jbuf,"\"level\":\"ERROR\"")!=NULL, "log json level ERROR");
    CHECK(strstr(jbuf,"\"tag\":\"motor\"")!=NULL, "log json tag motor");
    CHECK(strstr(jbuf,"motor stall")!=NULL, "log json message");
    printf("log json: %s\n", jbuf);

    /* 12. log 转义：含 "、\、控制字符 → 正规 JSON 转义，不破坏 JSON */
    uint8_t logesc[64]; memset(logesc,0,sizeof(logesc));
    logesc[4]=0x02; logesc[5]=0x00; logesc[6]=0x08; /* motor/ERROR */
    const char* rawmsg = "he said \"hi\" \\ tab\x09";
    size_t rlen = strlen(rawmsg); /* \x09 在 strlen 内被算入（0x09 非 0） */
    memcpy(&logesc[7], rawmsg, rlen);
    n = bridge_log_to_json(logesc, 7+rlen, jbuf, sizeof(jbuf));
    CHECK(n>0, "log escape produced");
    /* 期望 " 转义为 \"、\ 转义为 \\、控制字符 0x09 → 空格 */
    CHECK(strstr(jbuf, "\\\"hi\\\"")!=NULL, "log quote escaped to \\\"");
    CHECK(strstr(jbuf, "\\\\")!=NULL, "log backslash escaped to \\\\");
    /* 原始未转义的裸 " 不应出现在 message 区域之外——简单校验输出是合法 JSON：
       以 { 开头、以 } 结尾 */
    CHECK(jbuf[0]=='{', "log json starts with {");
    CHECK(jbuf[n-1]=='}', "log json ends with }");
    printf("log escape json: %s\n", jbuf);

    /* 13. log 超长 → 截断但不丢弃（n>0，NUL 终止，仍含 timestamp） */
    uint8_t logbig[300]; memset(logbig,0,sizeof(logbig));
    logbig[4]=0x01; logbig[5]=0x00; logbig[6]=0x02; /* system/INFO */
    memset(&logbig[7], 'A', 200);
    char small[80];
    n = bridge_log_to_json(logbig, 7+200, small, sizeof(small));
    CHECK(n>0, "log truncated not dropped (n>0)");
    CHECK(n<(int)sizeof(small), "log trunc fits buffer");
    CHECK(small[n]=='\0', "log trunc NUL terminated");
    CHECK(strstr(small, "timestamp")!=NULL, "log trunc still has timestamp");
    CHECK(strstr(small, "AAAA")!=NULL, "log trunc has message prefix");
    CHECK(small[n-1]=='}', "log trunc ends with }");
    printf("log trunc json (len=%d): %s\n", n, small);

    /* 14. ping/pong */
    uint8_t pseq = 0xAA;
    const char* j_ping = "{\"cmd\":\"ping\",\"seq\":42}";
    CHECK(bridge_json_is_ping(j_ping, strlen(j_ping), &pseq) == true, "ping detected");
    CHECK(pseq == 42, "ping seq=42");

    pseq = 0xAA;
    CHECK(bridge_json_is_ping(j_diff, strlen(j_diff), &pseq) == false, "motion is not ping");
    CHECK(pseq == 0xAA, "non-ping leaves seq untouched");

    const char* j_ping_noseq = "{\"cmd\":\"ping\"}";
    pseq = 0xAA;
    CHECK(bridge_json_is_ping(j_ping_noseq, strlen(j_ping_noseq), &pseq) == true, "ping without seq");
    CHECK(pseq == 0, "missing seq defaults to 0");

    const char* j_ping_wrap = "{\"cmd\":\"ping\",\"seq\":300}";
    pseq = 0;
    CHECK(bridge_json_is_ping(j_ping_wrap, strlen(j_ping_wrap), &pseq) == true, "seq>255 still ping");
    CHECK(pseq == 44, "seq 300 & 0xFF == 44");

    CHECK(bridge_json_is_ping("not-json", 8, &pseq) == false, "invalid json is not ping");

    char pong[96];
    int pn = bridge_pong_to_json(42, 123ULL, 80000UL, pong, sizeof(pong));
    CHECK(pn > 0, "pong json built");
    CHECK(strcmp(pong, "{\"seq\":42,\"pong\":true,\"uptime_s\":123,\"heap\":80000}") == 0,
          "pong json exact");
    CHECK(bridge_pong_to_json(1, 1ULL, 1UL, pong, 8) == 0, "tiny buffer rejected");

    printf("\n%s (%d failures)\n", failures? "RESULT: FAIL":"RESULT: PASS", failures);
    return failures? 1:0;
}
