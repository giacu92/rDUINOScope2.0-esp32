#include "mount_link.h"

#include <atomic>
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
volatile bool mountLinkPaused = false;
volatile bool modbusWatchdogRegistered = false;
std::atomic<bool> urgentStopRequested{false};
bool gotoActive = false;
bool followTargetActive = false;
bool jogActive = false;

double currentRA_h = 0.0;
double currentDEC_deg = 0.0;
State currentStatus = State::IDLE;
bool currentIsSlewing = false;
bool currentMotorsEnabled = false;
bool currentTrackingEnabled = false;
TrackingMode currentTrackingMode = TrackingMode::SIDEREAL;
uint16_t currentSTM32FirmwareVersion = 0;
MountLinkSnapshot mountStateSnapshot = {
    0.0,
    0.0,
    State::IDLE,
    false,
    false,
    false,
    0,
    0
};

void readPositionFromModbus();

void notifyChanged() {
    if (changedCallback) {
        changedCallback();
    }
}

void publishMountStateSnapshot() {
    if (!mountStateMutex) return;

    MountLinkSnapshot snapshot = {
        currentRA_h,
        currentDEC_deg,
        currentStatus,
        currentIsSlewing,
        currentMotorsEnabled,
        currentTrackingEnabled,
        currentSTM32FirmwareVersion,
        millis()
    };

    bool shouldNotifyDisplay = false;
    if (xSemaphoreTake(mountStateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        shouldNotifyDisplay = snapshot.status != mountStateSnapshot.status
                           || snapshot.isSlewing != mountStateSnapshot.isSlewing
                           || snapshot.motorsEnabled != mountStateSnapshot.motorsEnabled
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

bool enqueuePriorityMountCommand(const MountCommand& cmd, TickType_t waitTicks = 0) {
    if (!mountCommandQueue) return false;
    if (xQueueSendToFront(mountCommandQueue, &cmd, waitTicks) == pdTRUE) return true;

    MountCommand discarded;
    if (xQueueReceive(mountCommandQueue, &discarded, 0) == pdTRUE) {
        return xQueueSendToFront(mountCommandQueue, &cmd, 0) == pdTRUE;
    }
    return false;
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

void clearLocalMotionRequests() {
    gotoActive = false;
    followTargetActive = false;
    jogActive = false;

    currentIsSlewing = false;
    currentTrackingEnabled = false;
    publishMountStateSnapshot();
}

bool applyStopRequest(uint8_t maxStopPoll = 50) {
    if (!telescopeRef) return false;

    bool ok = writeCommandRequest(CMD_STOP);
    if (ok) {
        clearLocalMotionRequests();

        for (uint8_t attempt = 0; attempt < maxStopPoll; ++attempt) {
            if (modbusWatchdogRegistered) {
                esp_task_wdt_reset();
            }
            readPositionFromModbus();
            if (currentStatus == State::IDLE) {
                return true;
            }
            if (currentStatus == State::ERROR) {
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        ok = false;
    }
    if (DEBUG_LX200_MODBUS) {
        if (!ok) {
            Serial.println("[Modbus] STOP non completato: STM32 non ha riportato IDLE.");
        }
    }
    return ok;
}

bool readSTM32FirmwareVersion() {
    if (!telescopeRef) return false;

    uint8_t result = modbus.readHoldingRegisters(REG_RES_STM32_FW_VERSION, 1);
    if (result != modbus.ku8MBSuccess) {
        currentSTM32FirmwareVersion = 0xFFFF;
        publishMountStateSnapshot();
        if (DEBUG_LX200_FULL) {
            Serial.printf("[Modbus] Errore lettura versione STM32: 0x%02X\n", result);
        }
        return false;
    }

    currentSTM32FirmwareVersion = modbus.getResponseBuffer(0);
    publishMountStateSnapshot();
    if (DEBUG_LX200_FULL) {
        Serial.printf("[Modbus] STM32 FW=0x%04X\n", currentSTM32FirmwareVersion);
    }
    return currentSTM32FirmwareVersion != 0xFFFF;
}

void readPositionFromModbus() {
    if (!telescopeRef) return;

    uint8_t result = modbus.readHoldingRegisters(REG_RES_STATUS, 5);
    if (result == modbus.ku8MBSuccess) {
        currentStatus = static_cast<State>(modbus.getResponseBuffer(0) & 0xF);

        int32_t ra_arcsec100 = (int32_t)((uint32_t)modbus.getResponseBuffer(1) << 16
                                        | (uint32_t)modbus.getResponseBuffer(2));
        int32_t dec_arcsec100 = (int32_t)((uint32_t)modbus.getResponseBuffer(3) << 16
                                         | (uint32_t)modbus.getResponseBuffer(4));

        currentRA_h = (ra_arcsec100 / 100.0) / 3600.0 / 15.0;
        currentDEC_deg = (dec_arcsec100 / 100.0) / 3600.0;
        currentIsSlewing = (currentStatus == State::SLEWING)
                        || (currentStatus == State::MANUAL_JOG);
        publishMountStateSnapshot();

        if (DEBUG_LX200_FULL) {
            Serial.printf("[Modbus] Status=%d RA=%.4fh DEC=%.4fdeg\n",
                          (int)currentStatus, currentRA_h, currentDEC_deg);
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
            urgentStopRequested.store(false, std::memory_order_release);
            result = applyStopRequest() ? modbus.ku8MBSuccess : 0xFF;
            break;

        case MountCommandType::SET_TRACKING:
            result = modbus.writeSingleRegister(REG_REQ_TRACKING_ENABLE, cmd.value1);
            if (result == modbus.ku8MBSuccess) {
                result = modbus.writeSingleRegister(REG_REQ_TRACKING_MODE, cmd.value2);
            }
            if (result == modbus.ku8MBSuccess) {
                result = writeCommandRequest(CMD_SET_TRACKING) ? modbus.ku8MBSuccess : 0xFF;
            }
            if (result == modbus.ku8MBSuccess) {
                currentTrackingEnabled = (cmd.value1 != 0);
                currentTrackingMode = static_cast<TrackingMode>(cmd.value2);
                publishMountStateSnapshot();
            }
            break;

        case MountCommandType::SET_MOTORS:
            result = modbus.writeSingleRegister(REG_REQ_MOTORS_ENABLE, cmd.value1);
            if (result == modbus.ku8MBSuccess) {
                result = writeCommandRequest(CMD_SET_MOTORS) ? modbus.ku8MBSuccess : 0xFF;
            }
            if (result == modbus.ku8MBSuccess) {
                currentMotorsEnabled = (cmd.value1 != 0);
                if (!currentMotorsEnabled) {
                    currentIsSlewing = false;
                }
                publishMountStateSnapshot();
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
            if (result == modbus.ku8MBSuccess) {
                jogActive = true;
                currentIsSlewing = true;
                publishMountStateSnapshot();
            }
            break;

        case MountCommandType::JOG_STOP:
            result = writeCommandRequest(CMD_JOG_STOP) ? modbus.ku8MBSuccess : 0xFF;
            if (result == modbus.ku8MBSuccess) {
                jogActive = false;
                currentIsSlewing = false;
                publishMountStateSnapshot();
            }
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
    modbusWatchdogRegistered = true;

    static unsigned long lastPosRead = 0;
    for (;;) {
        esp_task_wdt_reset();

        if (mountLinkPaused) {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

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
                currentStatus = State::IDLE;
                currentIsSlewing = false;
                publishMountStateSnapshot();
                continue;
            }

            uint8_t pendingResult = modbus.writeSingleRegister(REG_REQ_COMMAND_PENDING, 1);
            if (pendingResult != modbus.ku8MBSuccess) {
                if (DEBUG_LX200_MODBUS) {
                    Serial.printf("[Modbus] Errore pending GOTO: 0x%02X\n", pendingResult);
                }
                currentStatus = State::IDLE;
                currentIsSlewing = false;
                publishMountStateSnapshot();
                continue;
            }

            currentStatus = State::SLEWING;
            currentIsSlewing = true;
            gotoActive = true;
            publishMountStateSnapshot();

            const int MAX_POLL = 3000;
            bool gotoFinished = false;
            bool sawSlewing = false;
            bool warnedTrackingWithoutSlewing = false;
            for (int i = 0; i < MAX_POLL; i++) {
                esp_task_wdt_reset();
                if (mountLinkPaused) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));

                if (urgentStopRequested.exchange(false, std::memory_order_acq_rel)) {
                    bool stopped = applyStopRequest();
                    if (DEBUG_LX200_MODBUS) {
                        Serial.printf("[Modbus] STOP prioritario durante GOTO: %s.\n",
                                      stopped ? "ok" : "fallito");
                    }
                    gotoFinished = true;
                    break;
                }

                readPositionFromModbus();

                if (currentStatus == State::SLEWING) {
                    sawSlewing = true;
                } else if (currentStatus == State::TRACKING) {
                    if (sawSlewing || currentPositionMatchesTarget(cmd.ra_arcsec100, cmd.dec_arcsec100)) {
                        if (DEBUG_LX200_MODBUS) {
                            Serial.println("[Modbus] GOTO completato, tracking attivo.");
                        }
                        currentStatus = State::TRACKING;
                        currentIsSlewing = false;
                        gotoActive = false;
                        publishMountStateSnapshot();
                        gotoFinished = true;
                        break;
                    }

                    if (i > 5 && !warnedTrackingWithoutSlewing && DEBUG_LX200_MODBUS) {
                        Serial.println("[Modbus] TRACKING letto senza SLEWING: attendo avvio reale GOTO.");
                        warnedTrackingWithoutSlewing = true;
                    }
                } else if (currentStatus == State::ERROR) {
                    if (DEBUG_LX200_MODBUS) {
                        Serial.println("[Modbus] STM32 ha riportato errore GOTO.");
                    }
                    currentStatus = State::ERROR;
                    currentIsSlewing = false;
                    gotoActive = false;
                    publishMountStateSnapshot();
                    gotoFinished = true;
                    break;
                } else if (currentStatus == State::IDLE) {
                    if (i > 2) {
                        if (DEBUG_LX200_MODBUS) {
                            Serial.println("[Modbus] STM32 IDLE durante GOTO, uscita.");
                        }
                        currentStatus = State::IDLE;
                        currentIsSlewing = false;
                        gotoActive = false;
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
                currentStatus = State::IDLE;
                currentIsSlewing = false;
                gotoActive = false;
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
    bool stopped = false;
    if (stm32Ready) {
        constexpr uint8_t INIT_STOP_POLL_ATTEMPTS = 5;
        stopped = applyStopRequest(INIT_STOP_POLL_ATTEMPTS);
        if (stopped) {
            currentStatus = State::IDLE;
            currentIsSlewing = false;
        } else if (DEBUG_LX200_MODBUS) {
            Serial.println("[Modbus] Errore invio STOP iniziale.");
        }
    }
    publishMountStateSnapshot();
    return stm32Ready && stopped;
}

void mountLinkStartTask(BaseType_t taskCore) {
    xTaskCreatePinnedToCore(modbusTask, "modbus", 4096, nullptr, 1, nullptr, taskCore);
}

void mountLinkSetPaused(bool paused) {
    mountLinkPaused = paused;
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

    MountCommand cmd = {MountCommandType::GOTO, ra_arcsec100, dec_arcsec100, 0, 0, 0};
    if (!enqueueMountCommand(cmd, pdMS_TO_TICKS(100))) {
        if (DEBUG_LX200) {
            Serial.println("[WARN] Coda GOTO piena, comando scartato.");
        }
        return false;
    }
    return true;
}

bool mountLinkRequestStop() {
    if (!telescopeRef) return false;

    MountCommand cmd = {MountCommandType::STOP, 0, 0, 0, 0, 0};
    urgentStopRequested.store(true, std::memory_order_release);
    if (!enqueuePriorityMountCommand(cmd)) {
        urgentStopRequested.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

bool mountLinkRequestTrackingEnabled(bool enabled) {
    if (!telescopeRef) return false;

    MountCommand cmd = {
        MountCommandType::SET_TRACKING,
        0,
        0,
        static_cast<uint16_t>(enabled ? 1U : 0U),
        static_cast<uint16_t>(currentTrackingMode),
        0
    };
    if (enqueueMountCommand(cmd)) {
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
        static_cast<uint16_t>(currentTrackingEnabled ? 1U : 0U),
        static_cast<uint16_t>(mode),
        0
    };
    if (enqueueMountCommand(cmd)) {
        return true;
    }
    return false;
}

bool mountLinkRequestMotorsEnabled(bool enabled) {
    if (!telescopeRef) return false;

    MountCommand cmd = {MountCommandType::SET_MOTORS, 0, 0, static_cast<uint16_t>(enabled ? 1U : 0U), 0, 0};
    if (enqueueMountCommand(cmd)) {
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
