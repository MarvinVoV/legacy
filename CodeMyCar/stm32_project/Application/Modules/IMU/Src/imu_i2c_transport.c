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
        return started;
    }
    if (xSemaphoreTake(s_completion, pdMS_TO_TICKS(timeoutMs)) == pdTRUE) return s_result;

    /* A timed-out IT operation must NOT outlive the caller's stack buffer.
     * Disable both IRQs and stop/reinitialize the peripheral before returning.
     * No asynchronous abort is left pointing at an expired header/chunk. */
    HAL_NVIC_DisableIRQ(I2C1_EV_IRQn);
    HAL_NVIC_DisableIRQ(I2C1_ER_IRQn);
    s_active = false;
    ++g_imuI2cTimeouts;
    (void)HAL_I2C_DeInit(&hi2c1);
    hi2c1.pBuffPtr = NULL;
    const HAL_StatusTypeDef recovered = HAL_I2C_Init(&hi2c1);
    HAL_NVIC_ClearPendingIRQ(I2C1_EV_IRQn);
    HAL_NVIC_ClearPendingIRQ(I2C1_ER_IRQn);
    (void)xSemaphoreTake(s_completion, 0u);
    HAL_NVIC_EnableIRQ(I2C1_EV_IRQn);
    HAL_NVIC_EnableIRQ(I2C1_ER_IRQn);
    return recovered == HAL_OK ? HAL_TIMEOUT : HAL_ERROR;
}
