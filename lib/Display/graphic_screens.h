#pragma once

#include <Arduino.h>
#include <stdint.h>

enum class ScreenType {
    GPS = 0,
    Clock = 1,
    Unused2 = 2,
    SelectAlignment = 3,
    Main = 4,
    Coordinates = 5,
    Load = 6,
    Options = 7,
    Unused8 = 8,
    Unused9 = 9,
    Stats = 10,
    StarMap = 11,
    StarSync = 12,
    Constellation = 13,
    TFTCalibration = 14,
    ConfirmSunTrack = 15
};

enum class BootStatus {
    Pending,
    Running,
    Ok,
    Fail,
    Skip
};

struct UiPalette {
    uint16_t background;
    uint16_t panel;
    uint16_t text;
    uint16_t muted;
    uint16_t accent;
    uint16_t warning;
    uint16_t ok;

    uint16_t titleBackground;
    uint16_t titleText;
    uint16_t bodyText;
    uint16_t lightText;
    uint16_t dimText;
    uint16_t buttonTextLight;
    uint16_t buttonTextDim;
    uint16_t buttonBorderLight;
    uint16_t buttonBorderDim;
    uint16_t buttonSelection;
    uint16_t messageBackground;
    uint16_t messageText;
    uint16_t stateOn;
    uint16_t stateOff;
    uint16_t buttonTitle;
};

class UIManager {
public:
    void setScreen(ScreenType newScreen);
    ScreenType getCurrentScreen() const;

    void setNightMode(bool enabled);
    bool isNightMode() const;
    const UiPalette& colors() const;

private:
    ScreenType currentScreen = ScreenType::Main;
    bool nightMode = false;
};

const UiPalette& uiColors();
const UiPalette& uiDayPalette();
const UiPalette& uiNightPalette();

void displaySetNightMode(bool enabled);
bool displayIsNightMode();

// Complete screen/paint operations. These functions own the display lock.
void displayShowBootScreen();
void displayBootSetStatus(uint8_t row, const char* label, BootStatus status);
void displayShowInitScreen(const char* step, const char* detail, uint8_t progress);
void displayShowMainScreen(const char* wifiStatus,
                           const char* ipAddress,
                           uint16_t stm32FirmwareVersion,
                           bool motorsEnabled,
                           bool trackingEnabled,
                           uint8_t cpu0Load,
                           uint8_t cpu1Load);
void displayShowCpuLoad(uint8_t cpu0Load, uint8_t cpu1Load);
