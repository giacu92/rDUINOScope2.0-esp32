#include "cpu_load.h"

#include <esp_freertos_hooks.h>
#include <freertos/semphr.h>

namespace {

constexpr TickType_t CPU_LOAD_SAMPLE_TICKS = pdMS_TO_TICKS(1000);

SemaphoreHandle_t cpuLoadMutex = nullptr;
CpuLoadSnapshot cpuLoadSnapshot = {0, 0};
CpuLoadChangedCallback changedCallback = nullptr;
volatile uint32_t idleHookCounts[2] = {0, 0};
bool cpuLoadStarted = false;

bool IRAM_ATTR cpuIdleHook0() {
    idleHookCounts[0]++;
    return true;
}

bool IRAM_ATTR cpuIdleHook1() {
    idleHookCounts[1]++;
    return true;
}

void publishCpuLoadSnapshot(uint8_t core0, uint8_t core1) {
    bool changed = false;
    if (cpuLoadMutex && xSemaphoreTake(cpuLoadMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        changed = core0 != cpuLoadSnapshot.core0 || core1 != cpuLoadSnapshot.core1;
        cpuLoadSnapshot = {core0, core1};
        xSemaphoreGive(cpuLoadMutex);
    }

    if (changed && changedCallback) {
        changedCallback();
    }
}

void cpuLoadTask(void* pvParams) {
    (void)pvParams;

    uint32_t previousIdle[2] = {idleHookCounts[0], idleHookCounts[1]};
    uint32_t maxIdleDelta[2] = {1, 1};

    for (;;) {
        vTaskDelay(CPU_LOAD_SAMPLE_TICKS);

        uint32_t currentIdle[2] = {idleHookCounts[0], idleHookCounts[1]};
        uint8_t load[2] = {0, 0};

        for (uint8_t core = 0; core < 2; core++) {
            uint32_t idleDelta = currentIdle[core] - previousIdle[core];
            previousIdle[core] = currentIdle[core];

            if (idleDelta > maxIdleDelta[core]) {
                maxIdleDelta[core] = idleDelta;
            }

            uint32_t idlePercent = (idleDelta * 100UL) / maxIdleDelta[core];
            if (idlePercent > 100) idlePercent = 100;
            load[core] = static_cast<uint8_t>(100 - idlePercent);
        }

        publishCpuLoadSnapshot(load[0], load[1]);
    }
}

} // namespace

void cpuLoadBegin(BaseType_t taskCore, CpuLoadChangedCallback onChanged) {
    if (cpuLoadStarted) return;

    changedCallback = onChanged;
    cpuLoadMutex = xSemaphoreCreateMutex();

    esp_register_freertos_idle_hook_for_cpu(cpuIdleHook0, 0);
    esp_register_freertos_idle_hook_for_cpu(cpuIdleHook1, 1);
    xTaskCreatePinnedToCore(cpuLoadTask, "cpu_load", 2048, nullptr, 0, nullptr, taskCore);

    cpuLoadStarted = true;
}

CpuLoadSnapshot cpuLoadGetSnapshot() {
    CpuLoadSnapshot snapshot = cpuLoadSnapshot;
    if (cpuLoadMutex && xSemaphoreTake(cpuLoadMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        snapshot = cpuLoadSnapshot;
        xSemaphoreGive(cpuLoadMutex);
    }
    return snapshot;
}
