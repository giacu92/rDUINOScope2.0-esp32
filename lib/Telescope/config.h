#include "secrets.h"   // WiFi credentials (non versionato)

#pragma once

// ESP32 firmware identity. Version is packed as 0xMMmm and printed as %02X.%02X.
constexpr uint16_t ESP32_FIRMWARE_VERSION = 0xB001;
static const char* const ESP32_FIRMWARE_NAME = "Alice EQ";

// ═══════════════════════════════════════════════════════════════════════════
//  rDuinoScope 2.0 — ESP32-S3 User Configuration
// ═══════════════════════════════════════════════════════════════════════════

// ── Debug Flags ───────────────────────────────────────────────────────────
#define DEBUG_LX200         true
#define DEBUG_LX200_FULL    false
#define DEBUG_LX200_MODBUS  true


// WiFi should prioritize latency and connection stability over modem sleep.
#ifndef WIFI_DISABLE_SLEEP
#define WIFI_DISABLE_SLEEP true
#endif

// Simple runtime diagnostics for debug builds.
#ifndef ENABLE_CPU_LOAD_MONITOR
#define ENABLE_CPU_LOAD_MONITOR false
#endif

#ifndef ENABLE_FREERTOS_TASK_STATS
#define ENABLE_FREERTOS_TASK_STATS false
#endif

#ifndef FREERTOS_TASK_STATS_INTERVAL_MS
#define FREERTOS_TASK_STATS_INTERVAL_MS 5000
#endif

#ifndef MOUNT_GOTO_MAX_POLL_MS
#define MOUNT_GOTO_MAX_POLL_MS 90000
#endif

// ── WiFi flags ────────────────────────────────────────────────────────────
static const char* const WIFI_SSID     = secrets.getSSID();
static const char* const WIFI_PASSWORD = secrets.getPassword();

// ArduinoOTA network firmware updates. Leave OTA_PASSWORD empty to allow
// passwordless uploads on trusted local networks.
static const char* const OTA_HOSTNAME  = "rduinoscope-esp32";
static const char* const OTA_PASSWORD  = "";

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
static const char* const NTP_SERVER    = "pool.ntp.org";
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

// Active-low digital input. INPUT_PULLUP keeps day mode by default; pulling
// this pin to GND forces the display night palette.
constexpr uint8_t IS_NIGHT_MODE_PIN  = 8;

// Milestone 1 display/touch pins.
// These are provisional ESP32-S3 SPI pins and must match the final wiring.
// GPIO 4 is intentionally avoided because it is already used by RS485_DE_PIN.
constexpr uint8_t TFT_CS_PIN         = 14;
constexpr uint8_t TFT_RST_PIN        = 13;
constexpr uint8_t TFT_DC_PIN         = 12;
constexpr uint8_t TFT_MOSI_PIN       = 11;
constexpr uint8_t TFT_SCLK_PIN       = 10;
constexpr uint8_t TFT_BACKLIGHT_PIN  = 9;
constexpr uint8_t TFT_MISO_PIN       = 46;

constexpr uint8_t TOUCH_CS_PIN       = 15;
constexpr uint8_t TOUCH_IRQ_PIN      = 18;

constexpr uint16_t TFT_WIDTH         = 240;
constexpr uint16_t TFT_HEIGHT        = 320;
constexpr uint8_t TFT_ROTATION       = 0;   // 0/2 portrait, 1/3 landscape
constexpr uint8_t TFT_BRIGHTNESS     = 180; // 0..255

constexpr uint16_t TOUCH_MIN_X       = 257;  // Legacy initial calibration
constexpr uint16_t TOUCH_MAX_X       = 3900;
constexpr uint16_t TOUCH_MIN_Y       = 445;
constexpr uint16_t TOUCH_MAX_Y       = 3900;
