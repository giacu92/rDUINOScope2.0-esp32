#pragma once

#define LGFX_USE_V1

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <freertos/FreeRTOS.h>
#include <stdint.h>

class Display {
public:
    bool begin();
    bool isReady() const;
    bool lock(TickType_t timeout = pdMS_TO_TICKS(200));
    void unlock();
    lgfx::LGFX_Device& lcd();
    bool getTouch(uint16_t& x, uint16_t& y);
    bool probeTouch();
    void setBacklight(uint8_t brightness);
    uint8_t getBacklight() const;
};

Display& display();

bool displayBegin();
bool displayGetTouch(uint16_t& x, uint16_t& y);
bool displayProbeTouch();
void displaySetBacklight(uint8_t brightness);
uint8_t displayGetBacklight();
