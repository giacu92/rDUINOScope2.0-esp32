#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>

struct CpuLoadSnapshot {
    uint8_t core0;
    uint8_t core1;
};

using CpuLoadChangedCallback = void (*)();

void cpuLoadBegin(BaseType_t taskCore, CpuLoadChangedCallback onChanged = nullptr);
CpuLoadSnapshot cpuLoadGetSnapshot();
