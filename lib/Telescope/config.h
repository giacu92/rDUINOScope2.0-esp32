#include "secrets.h"   // WiFi credentials (non versionato)

#pragma once

// ═══════════════════════════════════════════════════════════════════════════
//  rDuinoScope 2.0 — ESP32-S3 User Configuration
// ═══════════════════════════════════════════════════════════════════════════

// ── Debug Flags ───────────────────────────────────────────────────────────
#define DEBUG_LX200         true
#define DEBUG_LX200_FULL    false
#define DEBUG_LX200_MODBUS  true

// ── WiFi flags ────────────────────────────────────────────────────────────
const char* WIFI_SSID     = secrets.getSSID();
const char* WIFI_PASSWORD = secrets.getPassword();

// ── Mechanical parameters ─────────────────────────────────────────────────
constexpr uint16_t GEAR_TEETH      = 144;
constexpr uint16_t MOTOR_STEPS     = 200;
constexpr uint16_t MICROSTEPPING   = 16;

// ── Stellarium Config ─────────────────────────────────────────────────────
const double LONGITUDE_DEG        = 14.233; // Initial longitude, can be updated by :Gg# command
const double LATITUDE_DEG         = 42.350; // Initial latitude, can be updated by :Gt# command

const uint16_t STEL_PORT          = 10003;
#define STEL_UPDATE_MS  500

// ═══════════════════════════════════════════════════════════════════════════
//  rDuinoScope 2.0 — ESP32-S3 User Configuration
// ═══════════════════════════════════════════════════════════════════════════

// ── NTP flags ─────────────────────────────────────────────────────────────
const char* NTP_SERVER            = "pool.ntp.org";
const long  GMT_OFFSET_SEC        =  3600;   // UTC+1 (ora solare)
const int   DAYLIGHT_OFFSET_SEC   =  3600;   // ora legale

// ── Feature flags ─────────────────────────────────────────────────────────
const long STEPS_PER_REV = (long)GEAR_TEETH * MOTOR_STEPS * MICROSTEPPING;

// Modbus Master request registers: written by ESP32, observed by STM32.
#define REG_REQ_TARGET_RA_HIGH     0
#define REG_REQ_TARGET_RA_LOW      1
#define REG_REQ_TARGET_DEC_HIGH    2
#define REG_REQ_TARGET_DEC_LOW     3
#define REG_REQ_COMMAND            4

// Modbus Slave response registers: written by STM32, read by ESP32.
#define REG_RES_STATUS             5   // 0=idle, 1=slewing, 2=tracking, 3=error, 4=motors disabled, 5=manual jog
#define REG_RES_CURRENT_RA_HIGH  6
#define REG_RES_CURRENT_RA_LOW   7
#define REG_RES_CURRENT_DEC_HIGH 8
#define REG_RES_CURRENT_DEC_LOW  9
#define REG_RES_ERROR_CODE       10

// Milestone 0 request registers.
#define REG_REQ_TRACKING_ENABLE  11  // 0=off, 1=on
#define REG_REQ_TRACKING_MODE    12  // 0=lunar, 1=sidereal, 2=solar
#define REG_REQ_MOTORS_ENABLE    13  // 0=disabled, 1=enabled
#define REG_REQ_JOG_AXIS         14  // 0=RA, 1=DEC
#define REG_REQ_JOG_DIRECTION    15  // 0=negative/west/south, 1=positive/east/north
#define REG_REQ_JOG_SPEED        16  // implementation-defined speed/profile

// Handshake bit:
//  - ESP32 sets this to 1 after writing REG_REQ_COMMAND;
//  - STM32 clears it to 0 after copying the command.
#define REG_REQ_COMMAND_PENDING  17  // 1=new command pending; STM32 clears to 0 after consuming
#define REG_RES_STM32_FW_VERSION 18  // STM32 firmware version, packed as 0xMMmm

#define CMD_NONE             0
#define CMD_GOTO             1
#define CMD_STOP             2
#define CMD_SYNC             3
#define CMD_FOLLOW_TARGET    4

// Milestone 0 commands. Commands 3 and 4 are kept compatible with the current
// STM32 firmware: 3=SYNC, 4=FOLLOW_TARGET. New commands start at 5.
#define CMD_SET_TRACKING     5
#define CMD_SET_MOTORS       6
#define CMD_JOG_START        7
#define CMD_JOG_STOP         8

#define TRACKING_MODE_LUNAR     0
#define TRACKING_MODE_SIDEREAL  1
#define TRACKING_MODE_SOLAR     2

#define JOG_AXIS_RA          0
#define JOG_AXIS_DEC         1
#define JOG_DIR_NEGATIVE     0
#define JOG_DIR_POSITIVE     1
#define JOG_SPEED_GUIDE      1
#define JOG_SPEED_CENTER     2
#define JOG_SPEED_SLEW       3

// Watchdog: se il task Modbus non fa reset entro 5 s viene riavviato
#define WDT_TIMEOUT_S   5

// ── Modbus — Serial1 (USART1) ─────────────────────────────────────────────
// PA9=TX, PA10=RX  (hardware USART1)
#define MODBUS_BAUDRATE 57600
#define MODBUS_SLAVE_ID 1

constexpr uint8_t MODBUS_TX_PIN = 17;
constexpr uint8_t MODBUS_RX_PIN = 16;
constexpr uint8_t RS485_DE_PIN  = 4;

// ESP32-S3 DevKitC-1 onboard RGB LED (WS2812/NeoPixel)
constexpr uint8_t RGB_LED_PIN        = 48;
constexpr uint8_t RGB_LED_COUNT      = 1;
constexpr uint8_t RGB_LED_BRIGHTNESS = 16;  // ~6% of 255
