#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "battery_service.h"

static uint16_t regs[6];
static bool failRead, failWrite, corruptConfig;
static bool read_reg(void *ctx, uint8_t reg, uint16_t *out)
{
    (void)ctx;
    if (failRead) return false;
    *out=regs[reg];
    if (reg==0 && corruptConfig) *out^=0x0800u;
    return true;
}
static bool write_reg(void *ctx, uint8_t reg, uint16_t value)
{
    (void)ctx;
    if (failWrite) return false;
    regs[reg]=value; return true;
}

int main(void)
{
    Ina219 dev={.read=read_reg,.write=write_reg};
    Ina219Sample sample={0};
    assert(Ina219_Configure(&dev)==INA219_OK);
    assert(regs[0]==0x3e63); /* datasheet ADC/mode bits, not a driver-generated expectation */
    regs[2]=(3000u<<3)|2u; regs[1]=1200;
    assert(Ina219_Read(&dev,&sample)==INA219_OK);
    assert(sample.busMv==12000 && sample.shuntUv==12000);
    regs[1]=(uint16_t)(int16_t)-1200;
    assert(Ina219_Read(&dev,&sample)==INA219_OK && sample.shuntUv==-12000);
    regs[1]=32000; assert(Ina219_Read(&dev,&sample)==INA219_RANGE);
    regs[1]=0; regs[2]|=1; assert(Ina219_Read(&dev,&sample)==INA219_RANGE);
    regs[2]=0; assert(Ina219_Read(&dev,&sample)==INA219_NOT_READY);
    regs[2]=(6501u<<3)|2u; assert(Ina219_Read(&dev,&sample)==INA219_RANGE);
    failRead=true; assert(Ina219_Read(&dev,&sample)==INA219_IO);
    assert(Ina219_Configure(&dev)==INA219_IO); failRead=false;
    failWrite=true; assert(Ina219_Trigger(&dev)==INA219_IO); failWrite=false;
    corruptConfig=true; assert(Ina219_Configure(&dev)==INA219_CONFIG); corruptConfig=false;
    assert(Ina219_Read(NULL,&sample)==INA219_CONFIG);

    BatteryService service={.address=0x40}; BatteryReading r;
    sample=(Ina219Sample){.busMv=12000,.shuntUv=12000};
    BatteryService_Update(&service,INA219_OK,&sample,10,&r);
    assert(r.batteryMv==12012 && r.flags==5 && r.currentMa==0);
    service.shuntMilliOhm=100;
    BatteryService_Update(&service,INA219_OK,&sample,0x12345678,&r);
    assert(r.flags==7 && r.currentMa==120 && r.powerMw==1441 && r.percent==85);
    uint8_t frame[BATTERY_FRAME_SIZE]; BatteryService_Pack(&r,frame);
    const uint8_t expected[29]={0x78,0x56,0x34,0x12,5,0xe0,0x2e,0xec,0x2e,
        0xe0,0x2e,0,0,0x78,0,0,0,0xa1,5,0,0,85,7,0,64,100,0,0xec,0x2e};
    assert(memcmp(frame,expected,sizeof(frame))==0);
    sample.shuntUv=-12000;
    BatteryService_Update(&service,INA219_OK,&sample,20,&r);
    assert(r.currentMa==-120 && r.powerMw<0 && r.batteryMv==11988);
    BatteryService_Update(&service,INA219_IO,&sample,30,&r);
    assert(r.flags==0 && r.currentMa==0 && r.percent==0 && !service.filterReady);
    sample=(Ina219Sample){.busMv=11100};
    BatteryService_Update(&service,INA219_OK,&sample,40,&r);
    assert(r.filteredMv==11100 && r.percent==40); /* no stale pre-failure filter */
    sample.busMv=0;
    BatteryService_Update(&service,INA219_OK,&sample,50,&r);
    assert(!(r.flags & BATTERY_SOC_VALID)); /* powered by USB, battery absent */
    assert(!service.filterReady);
    sample.busMv=11100; sample.shuntUv=10000; service.shuntMilliOhm=10;
    BatteryService_Update(&service,INA219_OK,&sample,60,&r);
    assert(r.currentMa==1000 && r.batteryMv==11110 && r.powerMw==11110);
    assert(r.filteredMv==11110); /* reconnect does not blend with zero volts */
    uint8_t last=0,p=0;
    assert(!BatteryService_VoltagePercent(0,&p));
    assert(!BatteryService_VoltagePercent(12901,&p));
    for (unsigned mv=7500; mv<=12900; ++mv) {
        assert(BatteryService_VoltagePercent(mv,&p)); assert(p>=last && p<=100); last=p;
    }
    assert(BatteryService_VoltagePercent(12600,&p) && p==100);
    puts("power: trigger/config/error/signed conversion, uncalibrated null contract, SOC and wire golden passed");
}
