#include "imu_i2c_transport.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

extern I2C_HandleTypeDef hi2c1;
static StaticSemaphore_t s_completionStorage;
static SemaphoreHandle_t s_completion;
static volatile bool s_active;
static volatile HAL_StatusTypeDef s_result;
volatile uint32_t g_imuI2cCompletions;
volatile uint32_t g_imuI2cTimeouts;
volatile uint32_t g_imuI2cErrors;
volatile uint32_t g_imuI2cBusy;
volatile uint32_t g_imuI2cRecoveries;
volatile uint32_t g_imuI2cRecoveryFailures;
volatile uint32_t g_imuI2cLastIsr, g_imuI2cLastState, g_imuI2cLastError;
volatile uint32_t g_imuI2cLastPins;
static bool s_recovered;
static uint32_t s_lastRecoveryMs;

/* Exclusive ImuTask ownership: quiesce IRQs before invalidating any transfer
 * buffer. This resets peripheral/HAL state, not the sensor or its yaw reference.
 * A physically held-low bus may still require wiring/power repair. */
static HAL_StatusTypeDef recover(void)
{
    g_imuI2cLastIsr = hi2c1.Instance->ISR;
    g_imuI2cLastState = hi2c1.State;
    g_imuI2cLastError = hi2c1.ErrorCode;
    g_imuI2cLastPins = (uint32_t)HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_6) |
        ((uint32_t)HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_7) << 1);
    HAL_NVIC_DisableIRQ(I2C1_EV_IRQn);
    HAL_NVIC_DisableIRQ(I2C1_ER_IRQn);
    s_active = false;
    const HAL_StatusTypeDef stopped = HAL_I2C_DeInit(&hi2c1);
    hi2c1.pBuffPtr = NULL;
    const HAL_StatusTypeDef initialized = HAL_I2C_Init(&hi2c1);
    HAL_NVIC_ClearPendingIRQ(I2C1_EV_IRQn);
    HAL_NVIC_ClearPendingIRQ(I2C1_ER_IRQn);
    (void)xSemaphoreTake(s_completion, 0u);
    HAL_NVIC_EnableIRQ(I2C1_EV_IRQn);
    HAL_NVIC_EnableIRQ(I2C1_ER_IRQn);
    s_lastRecoveryMs = HAL_GetTick();
    s_recovered = true;
    ++g_imuI2cRecoveries;
    if (stopped != HAL_OK || initialized != HAL_OK) {
        ++g_imuI2cRecoveryFailures;
        return HAL_ERROR;
    }
    return HAL_OK;
}

static void complete(I2C_HandleTypeDef *bus, HAL_StatusTypeDef status)
{
    if (bus != &hi2c1 || !s_active) return;
    s_active = false;
    s_result = status;
    if (status == HAL_OK) ++g_imuI2cCompletions;
    else ++g_imuI2cErrors;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(s_completion, &wake);
    portYIELD_FROM_ISR(wake);
}

void I2C1_EV_IRQHandler(void) { HAL_I2C_EV_IRQHandler(&hi2c1); }
void I2C1_ER_IRQHandler(void) { HAL_I2C_ER_IRQHandler(&hi2c1); }
void HAL_I2C_MasterRxCpltCallback(I2C_HandleTypeDef *bus) { complete(bus, HAL_OK); }
void HAL_I2C_MasterTxCpltCallback(I2C_HandleTypeDef *bus) { complete(bus, HAL_OK); }
void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *bus) { complete(bus, HAL_ERROR); }

HAL_StatusTypeDef ImuI2c_Transfer(bool receive, uint16_t address,
                                uint8_t *buffer, uint16_t size, uint32_t timeoutMs)
{
    if (!buffer || !size || !timeoutMs) return HAL_ERROR;
    if (!s_completion) {
        s_completion = xSemaphoreCreateBinaryStatic(&s_completionStorage);
        HAL_NVIC_SetPriority(I2C1_EV_IRQn, 6u, 0u);
        HAL_NVIC_SetPriority(I2C1_ER_IRQn, 6u, 0u);
        HAL_NVIC_ClearPendingIRQ(I2C1_EV_IRQn);
        HAL_NVIC_ClearPendingIRQ(I2C1_ER_IRQn);
        HAL_NVIC_EnableIRQ(I2C1_EV_IRQn);
        HAL_NVIC_EnableIRQ(I2C1_ER_IRQn);
    }
    (void)xSemaphoreTake(s_completion, 0u);
    s_result = HAL_ERROR;
    s_active = true;
    const HAL_StatusTypeDef started = receive
        ? HAL_I2C_Master_Receive_IT(&hi2c1, address, buffer, size)
        : HAL_I2C_Master_Transmit_IT(&hi2c1, address, buffer, size);
    if (started != HAL_OK) {
        s_active = false;
        ++g_imuI2cErrors;
        if (started == HAL_BUSY) {
            ++g_imuI2cBusy;
            /* At most four BUSY recoveries/s; never loop/reset repeatedly inside
             * one call or retry an uncertain write. The caller owns retries. */
            if (!s_recovered || HAL_GetTick()-s_lastRecoveryMs >= 250u) {
                if (recover() != HAL_OK) return HAL_ERROR;
            }
            /* SH-2 polls read() while opening. Yield on persistent BUSY rather
             * than burning hundreds of thousands of failed reads per second. */
            vTaskDelay(pdMS_TO_TICKS(1u));
        }
        return started;
    }
    if (xSemaphoreTake(s_completion, pdMS_TO_TICKS(timeoutMs)) == pdTRUE) return s_result;

    /* A timed-out IT operation must NOT outlive the caller's stack buffer.
     * Disable both IRQs and stop/reinitialize the peripheral before returning.
     * No asynchronous abort is left pointing at an expired header/chunk. */
    ++g_imuI2cTimeouts;
    const HAL_StatusTypeDef recovered = recover();
    return recovered == HAL_OK ? HAL_TIMEOUT : HAL_ERROR;
}
