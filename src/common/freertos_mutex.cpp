#include <common/freertos_mutex.hpp>

#include "rtos_api.hpp"
#include "semphr.h"

namespace freertos {

Mutex::Mutex() noexcept {
    SemaphoreHandle_t semaphore = xSemaphoreCreateMutexStatic(&xSemaphoreData);
    // We are creating static FreeRTOS object here, supplying our own buffer
    // to be used by FreeRTOS. FreeRTOS constructs an object in that memory
    // and gives back a handle, which in current version is just a pointer
    // to the same buffer we provided. If this ever changes, we will have to
    // store the handle separately, but right now we can just use the pointer
    // to the buffer instead of the handle and save 4 bytes per instance.
    configASSERT(semaphore == &xSemaphoreData);
}

Mutex::~Mutex() {
    // Empty here. But we need that one for tests.
}

void Mutex::unlock() {
    SemaphoreHandle_t semaphore = &xSemaphoreData;
    xSemaphoreGive(semaphore);
}

bool Mutex::try_lock() {
    SemaphoreHandle_t semaphore = &xSemaphoreData;
    return xSemaphoreTake(semaphore, 0) == pdTRUE;
}

void Mutex::lock() {
    SemaphoreHandle_t semaphore = &xSemaphoreData;
    xSemaphoreTake(semaphore, portMAX_DELAY);
}

} // namespace freertos

void buddy::lock(std::unique_lock<freertos::Mutex> &l1, std::unique_lock<freertos::Mutex> &l2) {
    if (&l1 < &l2) {
        l1.lock();
        l2.lock();
    } else {
        l2.lock();
        l1.lock();
    }
}
