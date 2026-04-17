#pragma once

#include <Arduino.h>
#include <stdint.h>

bool displayBegin();
void displayShowBootScreen();
void displayShowInitScreen(const char* step, const char* detail, uint8_t progress);
void displayShowMainScreen(const char* wifiStatus,
                           const char* ipAddress,
                           uint16_t stm32FirmwareVersion,
                           bool motorsEnabled,
                           bool trackingEnabled);
bool displayGetTouch(uint16_t& x, uint16_t& y);
void displaySetBacklight(uint8_t brightness);
