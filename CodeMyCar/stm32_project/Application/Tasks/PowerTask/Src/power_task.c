#include "power_task.h"
#include "battery_service.h"
#include "power_config.h"
#include "stm32h7xx_hal.h"
#include "cmsis_os2.h"
#include "freertos_os2.h"
#include "pkt_protocol.h"
#include "uart_handle.h"
#include "log_manager.h"

/* Only this task owns I2C2. Sensor faults never block motion initialization. */
static I2C_HandleTypeDef s_bus;
static uint8_t s_seq;
static uint8_t s_address = POWER_INA219_ADDRESS;

static uint32_t bus_clock_hz(void)
{
    /* This H7 HAL's GetPeriphCLKFreq does NOT implement I2C identifiers.
     * I2C1/2 share a mux: validate its existing PCLK1 source without changing
     * it underneath the running IMU task. Refuse an unsupported clock setup. */
    return __HAL_RCC_GET_I2C2_SOURCE() == RCC_I2C123CLKSOURCE_D2PCLK1
        ? HAL_RCC_GetPCLK1Freq() : 0u;
}

static uint32_t timing_100k(uint32_t hz)
{
    if (!hz) return 0;
    for (uint32_t presc=0; presc<16; ++presc) {
        const uint32_t ns=(uint32_t)(1000000000ULL*(presc+1u)/hz);
        if (!ns) continue;
        const uint32_t delay=(1100u+ns-1u)/ns;
        const uint32_t half=(5000u+ns-1u)/ns;
        if (delay && delay<=16 && half && half<=256) {
            return (presc<<28)|((delay-1u)<<20)|(((400u+ns-1u)/ns)<<16)|
                   ((half-1u)<<8)|(half-1u);
        }
    }
    return 0;
}

static bool bus_init(void)
{
    (void)HAL_I2C_DeInit(&s_bus);
    /* A timed-out register read can leave the slave driving SDA. Release it
     * with at most nine clocks and a STOP before restoring alternate function.
     * All signals remain open-drain; no interrupts are disabled. */
    __HAL_RCC_GPIOB_CLK_ENABLE();
    HAL_GPIO_WritePin(GPIOB,GPIO_PIN_10|GPIO_PIN_11,GPIO_PIN_SET);
    GPIO_InitTypeDef pins={0};
    pins.Pin=GPIO_PIN_10|GPIO_PIN_11;
    pins.Mode=GPIO_MODE_OUTPUT_OD; pins.Pull=GPIO_PULLUP;
    pins.Speed=GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB,&pins);
    for (unsigned i=0; i<9 && HAL_GPIO_ReadPin(GPIOB,GPIO_PIN_11)==GPIO_PIN_RESET; ++i) {
        HAL_GPIO_WritePin(GPIOB,GPIO_PIN_10,GPIO_PIN_RESET);
        osDelay(pdMS_TO_TICKS(1));
        HAL_GPIO_WritePin(GPIOB,GPIO_PIN_10,GPIO_PIN_SET);
        osDelay(pdMS_TO_TICKS(1));
        if (HAL_GPIO_ReadPin(GPIOB,GPIO_PIN_10)==GPIO_PIN_RESET) return false;
    }
    HAL_GPIO_WritePin(GPIOB,GPIO_PIN_10|GPIO_PIN_11,GPIO_PIN_RESET);
    osDelay(pdMS_TO_TICKS(1));
    HAL_GPIO_WritePin(GPIOB,GPIO_PIN_10,GPIO_PIN_SET);
    osDelay(pdMS_TO_TICKS(1));
    if (HAL_GPIO_ReadPin(GPIOB,GPIO_PIN_10)==GPIO_PIN_RESET) {
        HAL_GPIO_WritePin(GPIOB,GPIO_PIN_11,GPIO_PIN_SET);
        return false;
    }
    HAL_GPIO_WritePin(GPIOB,GPIO_PIN_11,GPIO_PIN_SET);
    osDelay(pdMS_TO_TICKS(1));
    if (HAL_GPIO_ReadPin(GPIOB,GPIO_PIN_11)==GPIO_PIN_RESET) return false;
    s_bus.Instance=I2C2;
    s_bus.Init.Timing=timing_100k(bus_clock_hz());
    s_bus.Init.AddressingMode=I2C_ADDRESSINGMODE_7BIT;
    s_bus.Init.DualAddressMode=I2C_DUALADDRESS_DISABLE;
    s_bus.Init.OwnAddress1=0; s_bus.Init.OwnAddress2=0;
    s_bus.Init.OwnAddress2Masks=I2C_OA2_NOMASK;
    s_bus.Init.GeneralCallMode=I2C_GENERALCALL_DISABLE;
    s_bus.Init.NoStretchMode=I2C_NOSTRETCH_DISABLE;
    return s_bus.Init.Timing && HAL_I2C_Init(&s_bus)==HAL_OK &&
           HAL_I2CEx_ConfigAnalogFilter(&s_bus,I2C_ANALOGFILTER_ENABLE)==HAL_OK;
}
static bool read_reg(void *ctx, uint8_t reg, uint16_t *value)
{
    uint8_t bytes[2];
    const uint32_t started=HAL_GetTick();
    if (HAL_I2C_Mem_Read(ctx, s_address<<1, reg, I2C_MEMADD_SIZE_8BIT,
                         bytes,2,POWER_I2C_TIMEOUT_MS)!=HAL_OK) {
        LOG_WARN(LOG_MODULE_SENSOR,"POWER read reg=%u hal=%lu dt=%lums",reg,
                 (unsigned long)HAL_I2C_GetError(ctx),(unsigned long)(HAL_GetTick()-started));
        (void)started;
        return false;
    }
    *value=(uint16_t)(((uint16_t)bytes[0]<<8)|bytes[1]); return true;
}
static bool write_reg(void *ctx, uint8_t reg, uint16_t value)
{
    uint8_t bytes[2]={(uint8_t)(value>>8),(uint8_t)value};
    const uint32_t started=HAL_GetTick();
    if (HAL_I2C_Mem_Write(ctx,s_address<<1,reg,I2C_MEMADD_SIZE_8BIT,
                         bytes,2,POWER_I2C_TIMEOUT_MS)==HAL_OK) return true;
    LOG_WARN(LOG_MODULE_SENSOR,"POWER write reg=%u hal=%lu dt=%lums",reg,
             (unsigned long)HAL_I2C_GetError(ctx),(unsigned long)(HAL_GetTick()-started));
    (void)started;
    return false;
}
static void power_run(void *arg)
{
    (void)arg;
    BatteryService service={.shuntMilliOhm=CMCAR_POWER_SHUNT_MILLIOHM,
                            .address=POWER_INA219_ADDRESS};
    Ina219 device={.ctx=&s_bus,.read=read_reg,.write=write_reg};
    uint32_t nextInit=0,lastReport=0;
    bool configured=false;
    for (;;) {
        const uint32_t started=osKernelGetTickCount();
        Ina219Result rc=INA219_IO;
        Ina219Sample sample={0};
        if (!configured && (int32_t)(HAL_GetTick()-nextInit)>=0) {
            /* Set Instance before DeInit; first attempt must never dereference NULL. */
            s_bus.Instance=I2C2;
            if (bus_init()) {
                /* Dedicated bus: accept exactly one responding INA219 address.
                 * Readiness probes never write registers on unidentified devices. */
                unsigned found=0;
                for (uint8_t address=0x40; address<=0x4f; ++address) {
                    if (HAL_I2C_IsDeviceReady(&s_bus,address<<1,1,POWER_I2C_TIMEOUT_MS)==HAL_OK) {
                        s_address=address; ++found;
                    }
                }
                service.address=s_address;
                if (found==1) rc=Ina219_Configure(&device);
                else if (found>1) rc=INA219_CONFIG;
            }
            configured=rc==INA219_OK;
            if (configured) {
                LOG_INFO(LOG_MODULE_SENSOR,"POWER ready addr=0x%02X shunt=%umOhm",
                         s_address,service.shuntMilliOhm);
            } else {
                LOG_WARN(LOG_MODULE_SENSOR,"POWER probe rc=%u hal=%lu clk=%lu SCL=%u SDA=%u",
                         (unsigned)rc,(unsigned long)HAL_I2C_GetError(&s_bus),
                         (unsigned long)bus_clock_hz(),
                         (unsigned)HAL_GPIO_ReadPin(GPIOB,GPIO_PIN_10),
                         (unsigned)HAL_GPIO_ReadPin(GPIOB,GPIO_PIN_11));
            }
            nextInit=HAL_GetTick()+2000u;
        }
        if (configured) {
            rc=Ina219_Trigger(&device);
            if (rc==INA219_OK) {
                osDelay(pdMS_TO_TICKS(POWER_CONVERSION_WAIT_MS));
                rc=Ina219_Read(&device,&sample);
            }
            if (rc==INA219_IO || rc==INA219_CONFIG) {
                configured=false; nextInit=HAL_GetTick()+2000u;
            }
        }
        BatteryReading reading;
        BatteryService_Update(&service,rc,&sample,HAL_GetTick(),&reading);
        if (HAL_GetTick()-lastReport>=POWER_REPORT_PERIOD_MS) {
            lastReport=HAL_GetTick();
            uint8_t frame[BATTERY_FRAME_SIZE];
            BatteryService_Pack(&reading,frame);
            (void)UART_Send_Frame(PROTOCOL_TYPE_SENSOR,s_seq++,0,frame,sizeof(frame));
            LOG_DEBUG(LOG_MODULE_SENSOR,"POWER flags=%u err=%u bus=%umV shunt=%lduV",
                      reading.flags,reading.error,reading.busMv,(long)reading.shuntUv);
        }
        osDelayUntil(started+pdMS_TO_TICKS(POWER_SAMPLE_PERIOD_MS));
    }
}
void PowerTask_Init(void)
{
    const osThreadAttr_t attrs={.name="PowerTask",.stack_size=1536,
                               .priority=osPriorityBelowNormal};
    if (!osThreadNew(power_run,NULL,&attrs))
        LOG_ERROR(LOG_MODULE_SENSOR,"Failed to create PowerTask");
}
