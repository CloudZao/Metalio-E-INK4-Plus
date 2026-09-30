/**
 * @file metalio_i2c1.c
 * @brief I2C1 互斥：TCA / TPS / 触摸共用，避免 clear bus
 */

#include "metalio_i2c1.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t s_mu;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;

static void ensure_mu(void) {
    if (s_mu != NULL) {
        return;
    }
    portENTER_CRITICAL(&s_init_mux);
    if (s_mu == NULL) {
        s_mu = xSemaphoreCreateMutex();
    }
    portEXIT_CRITICAL(&s_init_mux);
}

bool metalio_i2c1_lock(int timeout_ms) {
    ensure_mu();
    if (s_mu == NULL) {
        return false;
    }
    const TickType_t ticks =
        (timeout_ms < 0) ? portMAX_DELAY
                         : (timeout_ms == 0 ? 0 : pdMS_TO_TICKS((uint32_t)timeout_ms));
    return xSemaphoreTake(s_mu, ticks) == pdTRUE;
}

void metalio_i2c1_unlock(void) {
    if (s_mu != NULL) {
        xSemaphoreGive(s_mu);
    }
}
