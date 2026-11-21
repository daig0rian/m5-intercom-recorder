// Core2 は液晶と SD カードが同じ SPI バスを使う。描画と SD アクセスはこのロックで排他にする。
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

void spi_lock_init();
SemaphoreHandle_t spi_lock_handle();

class SpiLockGuard {
public:
    SpiLockGuard() { xSemaphoreTake(spi_lock_handle(), portMAX_DELAY); }
    ~SpiLockGuard() { xSemaphoreGive(spi_lock_handle()); }
    SpiLockGuard(const SpiLockGuard&) = delete;
    SpiLockGuard& operator=(const SpiLockGuard&) = delete;
};
