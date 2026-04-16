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

#define REG_RA_HIGH     0
#define REG_RA_LOW      1
#define REG_DEC_HIGH    2
#define REG_DEC_LOW     3
#define REG_COMMAND     4
#define REG_STATUS      5   // 0=idle, 1=slewing, 2=tracking, 3=error
#define REG_CURRENT_RA_HIGH  6
#define REG_CURRENT_RA_LOW   7
#define REG_CURRENT_DEC_HIGH 8
#define REG_CURRENT_DEC_LOW  9
#define REG_ERROR_CODE       10

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
