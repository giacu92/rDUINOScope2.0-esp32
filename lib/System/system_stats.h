#pragma once

#include <Arduino.h>
#include <esp_freertos_hooks.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "config.h"

struct CpuLoadSnapshot {
    uint8_t core0;
    uint8_t core1;
};

using CpuLoadChangedCallback = void (*)();

namespace systemStats {

static constexpr TickType_t CPU_LOAD_SAMPLE_TICKS = pdMS_TO_TICKS(1000);
static constexpr uint8_t CPU_CORE_COUNT = portNUM_PROCESSORS;

static SemaphoreHandle_t cpuLoadMutex = nullptr;
static CpuLoadSnapshot cpuLoadSnapshot = {0, 0};
static CpuLoadChangedCallback changedCallback = nullptr;
#if ENABLE_CPU_LOAD_MONITOR
static volatile uint32_t idleHookCounts[2] = {0, 0};
static bool cpuLoadStarted = false;
#endif
static bool taskStatsStarted = false;

#if ENABLE_CPU_LOAD_MONITOR
static bool IRAM_ATTR cpuIdleHook0() {
    idleHookCounts[0]++;
    return true;
}

static bool IRAM_ATTR cpuIdleHook1() {
    idleHookCounts[1]++;
    return true;
}

static void publishCpuLoadSnapshot(uint8_t core0, uint8_t core1) {
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

static void cpuLoadTask(void* pvParams) {
    (void)pvParams;

    uint32_t previousIdle[2] = {idleHookCounts[0], idleHookCounts[1]};
    uint32_t maxIdleDelta[2] = {1, 1};

    for (;;) {
        vTaskDelay(CPU_LOAD_SAMPLE_TICKS);

        uint32_t currentIdle[2] = {idleHookCounts[0], idleHookCounts[1]};
        uint8_t load[2] = {0, 0};

        for (uint8_t core = 0; core < CPU_CORE_COUNT; core++) {
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
#endif

static void printTaskStats() {
    CpuLoadSnapshot load = cpuLoadSnapshot;
    if (cpuLoadMutex && xSemaphoreTake(cpuLoadMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        load = cpuLoadSnapshot;
        xSemaphoreGive(cpuLoadMutex);
    }

    Serial.println();
    Serial.println("--- SYSTEM STATS ---");
    Serial.printf("CPU core 0: %3u%%\n", static_cast<unsigned>(load.core0));
#if portNUM_PROCESSORS > 1
    Serial.printf("CPU core 1: %3u%%\n", static_cast<unsigned>(load.core1));
#endif
    Serial.printf("Tasks:      %3u\n", static_cast<unsigned>(uxTaskGetNumberOfTasks()));
    Serial.printf("Heap:       %lu free, %lu min\n",
                  static_cast<unsigned long>(ESP.getFreeHeap()),
                  static_cast<unsigned long>(ESP.getMinFreeHeap()));
}

static void taskStatsTask(void* pvParams) {
    (void)pvParams;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(FREERTOS_TASK_STATS_INTERVAL_MS));
        printTaskStats();
    }
}

} // namespace systemStats

static void cpuLoadBegin(BaseType_t taskCore, CpuLoadChangedCallback onChanged = nullptr) {
#if ENABLE_CPU_LOAD_MONITOR
    if (systemStats::cpuLoadStarted) return;

    systemStats::changedCallback = onChanged;
    systemStats::cpuLoadMutex = xSemaphoreCreateMutex();

    esp_register_freertos_idle_hook_for_cpu(systemStats::cpuIdleHook0, 0);
#if portNUM_PROCESSORS > 1
    esp_register_freertos_idle_hook_for_cpu(systemStats::cpuIdleHook1, 1);
#endif
    xTaskCreatePinnedToCore(systemStats::cpuLoadTask, "cpu_load", 2048, nullptr, 0, nullptr, taskCore);

    systemStats::cpuLoadStarted = true;
#else
    (void)taskCore;
    (void)onChanged;
#endif
}

static CpuLoadSnapshot cpuLoadGetSnapshot() {
    CpuLoadSnapshot snapshot = systemStats::cpuLoadSnapshot;
    if (systemStats::cpuLoadMutex && xSemaphoreTake(systemStats::cpuLoadMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        snapshot = systemStats::cpuLoadSnapshot;
        xSemaphoreGive(systemStats::cpuLoadMutex);
    }
    return snapshot;
}

static void taskStatsBegin(BaseType_t taskCore) {
#if ENABLE_FREERTOS_TASK_STATS
    if (systemStats::taskStatsStarted) return;

    xTaskCreatePinnedToCore(systemStats::taskStatsTask, "task_stats", 2048, nullptr, 0, nullptr, taskCore);
    systemStats::taskStatsStarted = true;
#else
    (void)taskCore;
#endif
}
