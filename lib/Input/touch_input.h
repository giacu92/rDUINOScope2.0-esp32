#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>

enum class TouchEventType : uint8_t {
    Pressed,
    Released
};

struct TouchEvent {
    TouchEventType type;
    uint16_t x;
    uint16_t y;
    uint32_t atMs;
};

using TouchInputChangedCallback = void (*)();

void touchInputBegin(BaseType_t taskCore, TouchInputChangedCallback onTouchEvent = nullptr);
bool touchInputGetEvent(TouchEvent& event, TickType_t waitTicks = 0);
