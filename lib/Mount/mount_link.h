#pragma once

#include <Arduino.h>
#include <stdint.h>
#include "telescope.h"

struct MountLinkSnapshot {
    double ra_h;
    double dec_deg;
    State status;
    bool isSlewing;
    bool motorsEnabled;
    bool trackingEnabled;
    uint16_t stm32FirmwareVersion;
    uint32_t updatedAtMs;
};

using MountLinkChangedCallback = void (*)();

bool mountLinkBegin(Telescope& telescope, MountLinkChangedCallback onVisibleStateChanged = nullptr);
void mountLinkStartTask(BaseType_t taskCore);
void mountLinkReadSTM32FirmwareVersion();
MountLinkSnapshot mountLinkGetSnapshot();

bool mountLinkRequestGoto(double targetRA_h, double targetDEC_deg);
bool mountLinkRequestStop();
bool mountLinkRequestTrackingEnabled(bool enabled);
bool mountLinkRequestTrackingMode(TrackingMode mode);
bool mountLinkRequestMotorsEnabled(bool enabled);
bool mountLinkRequestJog(uint16_t axis, uint16_t direction, uint16_t speed);
bool mountLinkRequestJogStop();
