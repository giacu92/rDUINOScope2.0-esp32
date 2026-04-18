#include "mount_link.h"

#include <math.h>
#include <esp_task_wdt.h>
#include <ModbusMaster.h>
#include <freertos/semphr.h>
#include "config.h"

namespace {

enum class MountCommandType : uint8_t {
    GOTO,
    STOP,
    SET_TRACKING,
    SET_MOTORS,
    JOG_START,
    JOG_STOP
};

struct MountCommand {
    MountCommandType type;
    int32_t ra_arcsec100;
    int32_t dec_arcsec100;
    uint16_t value1;
    uint16_t value2;
    uint16_t value3;
};

ModbusMaster modbus;
Telescope* telescopeRef = nullptr;
QueueHandle_t mountCommandQueue = nullptr;
SemaphoreHandle_t mountStateMutex = nullptr;
MountLinkChangedCallback changedCallback = nullptr;

double currentRA_h = 0.0;
double currentDEC_deg = 0.0;
MountLinkSnapshot mountStateSnapshot = {
    0.0,
    0.0,
    State::IDLE,
    false,
    true,
    false,
    0,
    0
};

void notifyChanged() {
    if (changedCallback) {
        changedCallback();
    }
}

void publishMountStateSnapshot() {
    if (!mountStateMutex || !telescopeRef) return;

    MountLinkSnapshot snapshot = {
        currentRA_h,
        currentDEC_deg,
        telescopeRef->status,
        telescopeRef->isSlewing,
        telescopeRef->motorsEnabled,
        telescopeRef->trackingEnabled,
        telescopeRef->stm32FirmwareVersion,
        millis()
    };

    bool shouldNotifyDisplay = false;
    if (xSemaphoreTake(mountStateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        shouldNotifyDisplay = snapshot.motorsEnabled != mountStateSnapshot.motorsEnabled
                           || snapshot.trackingEnabled != mountStateSnapshot.trackingEnabled
                           || snapshot.stm32FirmwareVersion != mountStateSnapshot.stm32FirmwareVersion;
        mountStateSnapshot = snapshot;
        xSemaphoreGive(mountStateMutex);
    }
    if (shouldNotifyDisplay) {
        notifyChanged();
    }
}

bool enqueueMountCommand(const MountCommand& cmd, TickType_t waitTicks = 0) {
    if (!mountCommandQueue) return false;
    return xQueueSend(mountCommandQueue, &cmd, waitTicks) == pdTRUE;
}

double raDeltaHours(double a, double b) {
    double delta = fabs(a - b);
    if (delta > 12.0) delta = 24.0 - delta;
    return delta;
}

bool currentPositionMatchesTarget(int32_t targetRaArcsec100, int32_t targetDecArcsec100) {
    const double RA_TOL_H = 0.001111;     // circa 1 arcmin in RA
    const double DEC_TOL_DEG = 0.016667;  // circa 1 arcmin

    double targetRaH = ((targetRaArcsec100 / 100.0) / 3600.0) / 15.0;
    double targetDecD = (targetDecArcsec100 / 100.0) / 3600.0;
    MountLinkSnapshot state = mountLinkGetSnapshot();

    return raDeltaHours(state.ra_h, targetRaH) <= RA_TOL_H
        && fabs(state.dec_deg - targetDecD) <= DEC_TOL_DEG;
}

void preTransmission() {
    digitalWrite(RS485_DE_PIN, HIGH);
}

void postTransmission() {
    digitalWrite(RS485_DE_PIN, LOW);
}

bool writeCommandRequest(uint16_t command) {
    uint8_t commandResult = modbus.writeSingleRegister(REG_REQ_COMMAND, command);
    if (commandResult != modbus.ku8MBSuccess) return false;

    uint8_t pendingResult = modbus.writeSingleRegister(REG_REQ_COMMAND_PENDING, 1);
    if (pendingResult != modbus.ku8MBSuccess) return false;

    if (DEBUG_LX200_MODBUS) {
        Serial.printf("[Modbus] COMMAND request = %u.\n", command);
    }
    return true;
}

bool readSTM32FirmwareVersion() {
    if (!telescopeRef) return false;

    uint8_t result = modbus.readHoldingRegisters(REG_RES_STM32_FW_VERSION, 1);
    if (result != modbus.ku8MBSuccess) {
        telescopeRef->stm32FirmwareVersion = 0xFFFF;
        publishMountStateSnapshot();
        if (DEBUG_LX200_FULL) {
            Serial.printf("[Modbus] Errore lettura versione STM32: 0x%02X\n", result);
        }
        return false;
    }

    telescopeRef->stm32FirmwareVersion = modbus.getResponseBuffer(0);
    publishMountStateSnapshot();
    if (DEBUG_LX200_FULL) {
        Serial.printf("[Modbus] STM32 FW=0x%04X\n", telescopeRef->stm32FirmwareVersion);
    }
    return telescopeRef->stm32FirmwareVersion != 0xFFFF;
}

void readPositionFromModbus() {
    if (!telescopeRef) return;

    uint8_t result = modbus.readHoldingRegisters(REG_RES_STATUS, 5);
    if (result == modbus.ku8MBSuccess) {
        telescopeRef->status = static_cast<State>(modbus.getResponseBuffer(0) & 0xF);

        int32_t ra_arcsec100 = (int32_t)((uint32_t)modbus.getResponseBuffer(1) << 16
                                        | (uint32_t)modbus.getResponseBuffer(2));
        int32_t dec_arcsec100 = (int32_t)((uint32_t)modbus.getResponseBuffer(3) << 16
                                         | (uint32_t)modbus.getResponseBuffer(4));

        currentRA_h = (ra_arcsec100 / 100.0) / 3600.0 / 15.0;
        currentDEC_deg = (dec_arcsec100 / 100.0) / 3600.0;
        telescopeRef->ra = currentRA_h;
        telescopeRef->dec = currentDEC_deg;
        telescopeRef->isSlewing = (telescopeRef->status == State::SLEWING);
        publishMountStateSnapshot();

        if (DEBUG_LX200_FULL) {
            Serial.printf("[Modbus] Status=%d RA=%.4fh DEC=%.4fdeg\n",
                          (int)telescopeRef->status, currentRA_h, currentDEC_deg);
        }
    } else if (DEBUG_LX200_MODBUS) {
        Serial.printf("[Modbus] Errore lettura posizione: 0x%02X\n", result);
    }
}

void applyMountCommand(const MountCommand& cmd) {
    if (!telescopeRef) return;

    uint8_t result = modbus.ku8MBSuccess;

    switch (cmd.type) {
        case MountCommandType::STOP:
            result = writeCommandRequest(CMD_STOP) ? modbus.ku8MBSuccess : 0xFF;
            if (result == modbus.ku8MBSuccess) {
                telescopeRef->setSlewing(false);
                publishMountStateSnapshot();
            }
            break;

        case MountCommandType::SET_TRACKING:
            result = modbus.writeSingleRegister(REG_REQ_TRACKING_ENABLE, cmd.value1);
            if (result == modbus.ku8MBSuccess) {
                result = modbus.writeSingleRegister(REG_REQ_TRACKING_MODE, cmd.value2);
            }
            if (result == modbus.ku8MBSuccess) {
                result = writeCommandRequest(CMD_SET_TRACKING) ? modbus.ku8MBSuccess : 0xFF;
            }
            break;

        case MountCommandType::SET_MOTORS:
            result = modbus.writeSingleRegister(REG_REQ_MOTORS_ENABLE, cmd.value1);
            if (result == modbus.ku8MBSuccess) {
                result = writeCommandRequest(CMD_SET_MOTORS) ? modbus.ku8MBSuccess : 0xFF;
            }
            break;

        case MountCommandType::JOG_START:
            result = modbus.writeSingleRegister(REG_REQ_JOG_AXIS, cmd.value1);
            if (result == modbus.ku8MBSuccess) {
                result = modbus.writeSingleRegister(REG_REQ_JOG_DIRECTION, cmd.value2);
            }
            if (result == modbus.ku8MBSuccess) {
                result = modbus.writeSingleRegister(REG_REQ_JOG_SPEED, cmd.value3);
            }
            if (result == modbus.ku8MBSuccess) {
                result = writeCommandRequest(CMD_JOG_START) ? modbus.ku8MBSuccess : 0xFF;
            }
            break;

        case MountCommandType::JOG_STOP:
            result = writeCommandRequest(CMD_JOG_STOP) ? modbus.ku8MBSuccess : 0xFF;
            break;

        case MountCommandType::GOTO:
        default:
            return;
    }

    if (result != modbus.ku8MBSuccess && DEBUG_LX200_MODBUS) {
        Serial.printf("[Modbus] Errore comando mount: type=%d result=0x%02X\n",
                      (int)cmd.type,
                      result);
    }
}

void modbusTask(void* pvParams) {
    (void)pvParams;
    esp_task_wdt_add(nullptr);

    static unsigned long lastPosRead = 0;
    for (;;) {
        esp_task_wdt_reset();

        MountCommand cmd;
        if (xQueueReceive(mountCommandQueue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (cmd.type != MountCommandType::GOTO) {
                applyMountCommand(cmd);
                continue;
            }

            uint16_t ra_high = (uint16_t)((cmd.ra_arcsec100 >> 16) & 0xFFFF);
            uint16_t ra_low = (uint16_t)(cmd.ra_arcsec100 & 0xFFFF);
            uint16_t dec_high = (uint16_t)((cmd.dec_arcsec100 >> 16) & 0xFFFF);
            uint16_t dec_low = (uint16_t)(cmd.dec_arcsec100 & 0xFFFF);

            modbus.setTransmitBuffer(0, ra_high);
            modbus.setTransmitBuffer(1, ra_low);
            modbus.setTransmitBuffer(2, dec_high);
            modbus.setTransmitBuffer(3, dec_low);
            modbus.setTransmitBuffer(4, CMD_GOTO);

            if (DEBUG_LX200_MODBUS) {
                Serial.printf("[Modbus] sending: RA_high=0x%04X RA_low=0x%04X DEC_high=0x%04X DEC_low=0x%04X COMMAND=%d\n",
                              ra_high, ra_low, dec_high, dec_low, 1);
            }

            uint8_t result = modbus.writeMultipleRegisters(REG_REQ_TARGET_RA_HIGH, 5);
            if (result != modbus.ku8MBSuccess) {
                if (DEBUG_LX200_MODBUS) {
                    Serial.printf("[Modbus] Errore scrittura: 0x%02X\n", result);
                }
                telescopeRef->setSlewing(false);
                publishMountStateSnapshot();
                continue;
            }

            uint8_t pendingResult = modbus.writeSingleRegister(REG_REQ_COMMAND_PENDING, 1);
            if (pendingResult != modbus.ku8MBSuccess) {
                if (DEBUG_LX200_MODBUS) {
                    Serial.printf("[Modbus] Errore pending GOTO: 0x%02X\n", pendingResult);
                }
                telescopeRef->setSlewing(false);
                publishMountStateSnapshot();
                continue;
            }

            const int MAX_POLL = 3000;
            bool gotoFinished = false;
            bool sawSlewing = false;
            bool warnedTrackingWithoutSlewing = false;
            for (int i = 0; i < MAX_POLL; i++) {
                esp_task_wdt_reset();
                vTaskDelay(pdMS_TO_TICKS(100));

                readPositionFromModbus();

                if (telescopeRef->status == State::SLEWING) {
                    sawSlewing = true;
                } else if (telescopeRef->status == State::TRACKING) {
                    if (sawSlewing || currentPositionMatchesTarget(cmd.ra_arcsec100, cmd.dec_arcsec100)) {
                        if (DEBUG_LX200_MODBUS) {
                            Serial.println("[Modbus] GOTO completato, tracking attivo.");
                        }
                        telescopeRef->setSlewing(false);
                        publishMountStateSnapshot();
                        gotoFinished = true;
                        break;
                    }

                    if (i > 5 && !warnedTrackingWithoutSlewing && DEBUG_LX200_MODBUS) {
                        Serial.println("[Modbus] TRACKING letto senza SLEWING: attendo avvio reale GOTO.");
                        warnedTrackingWithoutSlewing = true;
                    }
                } else if (telescopeRef->status == State::ERROR) {
                    if (DEBUG_LX200_MODBUS) {
                        Serial.println("[Modbus] STM32 ha riportato errore GOTO.");
                    }
                    telescopeRef->setSlewing(false);
                    publishMountStateSnapshot();
                    gotoFinished = true;
                    break;
                } else if (telescopeRef->status == State::IDLE) {
                    if (i > 2) {
                        if (DEBUG_LX200_MODBUS) {
                            Serial.println("[Modbus] STM32 IDLE durante GOTO, uscita.");
                        }
                        telescopeRef->setSlewing(false);
                        publishMountStateSnapshot();
                        gotoFinished = true;
                        break;
                    }
                }
            }

            if (!gotoFinished) {
                if (DEBUG_LX200_MODBUS) {
                    Serial.println("[Modbus] Timeout polling STM32.");
                }
                telescopeRef->setSlewing(false);
                publishMountStateSnapshot();
            }
            lastPosRead = millis();
        }

        if (millis() - lastPosRead >= 250) {
            lastPosRead = millis();
            readPositionFromModbus();
        }
    }
}

} // namespace

bool mountLinkBegin(Telescope& telescope, MountLinkChangedCallback onVisibleStateChanged) {
    telescopeRef = &telescope;
    changedCallback = onVisibleStateChanged;
    mountStateMutex = xSemaphoreCreateMutex();
    mountCommandQueue = xQueueCreate(8, sizeof(MountCommand));

    pinMode(RS485_DE_PIN, OUTPUT);
    digitalWrite(RS485_DE_PIN, LOW);

    Serial2.begin(MODBUS_BAUDRATE, SERIAL_8N1, MODBUS_RX_PIN, MODBUS_TX_PIN);
    modbus.begin(MODBUS_SLAVE_ID, Serial2);
    modbus.preTransmission(preTransmission);
    modbus.postTransmission(postTransmission);

    bool stm32Ready = readSTM32FirmwareVersion();
    publishMountStateSnapshot();
    return stm32Ready;
}

void mountLinkStartTask(BaseType_t taskCore) {
    xTaskCreatePinnedToCore(modbusTask, "modbus", 4096, nullptr, 1, nullptr, taskCore);
}

void mountLinkReadSTM32FirmwareVersion() {
    readSTM32FirmwareVersion();
}

MountLinkSnapshot mountLinkGetSnapshot() {
    MountLinkSnapshot snapshot = mountStateSnapshot;
    if (mountStateMutex && xSemaphoreTake(mountStateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        snapshot = mountStateSnapshot;
        xSemaphoreGive(mountStateMutex);
    }
    return snapshot;
}

bool mountLinkRequestGoto(double targetRA_h, double targetDEC_deg) {
    if (!telescopeRef) return false;

    int32_t ra_arcsec100 = (int32_t)(targetRA_h * 15.0 * 3600.0 * 100.0);
    int32_t dec_arcsec100 = (int32_t)(targetDEC_deg * 3600.0 * 100.0);

    if (DEBUG_LX200_MODBUS) {
        Serial.printf("[GOTO] RA=%.4fh DEC=%.4f deg -> RA_arcsec100=%ld DEC_arcsec100=%ld\n",
                      targetRA_h, targetDEC_deg, (long)ra_arcsec100, (long)dec_arcsec100);
    }

    telescopeRef->setSlewing(true);

    MountCommand cmd = {MountCommandType::GOTO, ra_arcsec100, dec_arcsec100, 0, 0, 0};
    if (!enqueueMountCommand(cmd, pdMS_TO_TICKS(100))) {
        if (DEBUG_LX200) {
            Serial.println("[WARN] Coda GOTO piena, comando scartato.");
        }
        telescopeRef->setSlewing(false);
        publishMountStateSnapshot();
        return false;
    }
    publishMountStateSnapshot();
    return true;
}

bool mountLinkRequestStop() {
    if (!telescopeRef) return false;

    MountCommand cmd = {MountCommandType::STOP, 0, 0, 0, 0, 0};
    telescopeRef->setSlewing(false);
    publishMountStateSnapshot();
    return enqueueMountCommand(cmd);
}

bool mountLinkRequestTrackingEnabled(bool enabled) {
    if (!telescopeRef) return false;

    MountCommand cmd = {
        MountCommandType::SET_TRACKING,
        0,
        0,
        static_cast<uint16_t>(enabled ? 1U : 0U),
        static_cast<uint16_t>(telescopeRef->trackingMode),
        0
    };
    if (enqueueMountCommand(cmd)) {
        telescopeRef->trackingEnabled = enabled;
        publishMountStateSnapshot();
        return true;
    }
    return false;
}

bool mountLinkRequestTrackingMode(TrackingMode mode) {
    if (!telescopeRef) return false;

    MountCommand cmd = {
        MountCommandType::SET_TRACKING,
        0,
        0,
        static_cast<uint16_t>(telescopeRef->trackingEnabled ? 1U : 0U),
        static_cast<uint16_t>(mode),
        0
    };
    if (enqueueMountCommand(cmd)) {
        telescopeRef->trackingMode = mode;
        publishMountStateSnapshot();
        return true;
    }
    return false;
}

bool mountLinkRequestMotorsEnabled(bool enabled) {
    if (!telescopeRef) return false;

    MountCommand cmd = {MountCommandType::SET_MOTORS, 0, 0, static_cast<uint16_t>(enabled ? 1U : 0U), 0, 0};
    if (enqueueMountCommand(cmd)) {
        telescopeRef->motorsEnabled = enabled;
        if (!enabled) telescopeRef->setSlewing(false);
        publishMountStateSnapshot();
        return true;
    }
    return false;
}

bool mountLinkRequestJog(uint16_t axis, uint16_t direction, uint16_t speed) {
    MountCommand cmd = {MountCommandType::JOG_START, 0, 0, axis, direction, speed};
    return enqueueMountCommand(cmd);
}

bool mountLinkRequestJogStop() {
    MountCommand cmd = {MountCommandType::JOG_STOP, 0, 0, 0, 0, 0};
    return enqueueMountCommand(cmd);
}
