#include "spi_lock.h"

static SemaphoreHandle_t s_spi_lock = nullptr;

void spi_lock_init() {
    if (!s_spi_lock) s_spi_lock = xSemaphoreCreateMutex();
}

SemaphoreHandle_t spi_lock_handle() { return s_spi_lock; }
