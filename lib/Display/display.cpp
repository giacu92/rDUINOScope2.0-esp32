#define LGFX_USE_V1

#include "display.h"

#include <LovyanGFX.hpp>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config.h"

namespace {

class RDuinoDisplay : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9341 panel;
    lgfx::Bus_SPI bus;
    lgfx::Light_PWM light;
    lgfx::Touch_XPT2046 touch;

public:
    RDuinoDisplay() {
        {
            auto cfg = bus.config();
            cfg.spi_host = SPI2_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = 40000000;
            cfg.freq_read = 16000000;
            cfg.spi_3wire = false;
            cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = TFT_SCLK_PIN;
            cfg.pin_mosi = TFT_MOSI_PIN;
            cfg.pin_miso = TFT_MISO_PIN;
            cfg.pin_dc = TFT_DC_PIN;
            bus.config(cfg);
            panel.setBus(&bus);
        }

        {
            auto cfg = panel.config();
            cfg.pin_cs = TFT_CS_PIN;
            cfg.pin_rst = TFT_RST_PIN;
            cfg.pin_busy = -1;
            cfg.panel_width = TFT_WIDTH;
            cfg.panel_height = TFT_HEIGHT;
            cfg.offset_x = 0;
            cfg.offset_y = 0;
            cfg.offset_rotation = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits = 1;
            cfg.readable = true;
            cfg.invert = false;
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = true;
            panel.config(cfg);
        }

        {
            auto cfg = light.config();
            cfg.pin_bl = TFT_BACKLIGHT_PIN;
            cfg.invert = false;
            cfg.freq = 5000;
            cfg.pwm_channel = 7;
            light.config(cfg);
            panel.setLight(&light);
        }

        {
            auto cfg = touch.config();
            cfg.x_min = TOUCH_MIN_X;
            cfg.x_max = TOUCH_MAX_X;
            cfg.y_min = TOUCH_MIN_Y;
            cfg.y_max = TOUCH_MAX_Y;
            cfg.pin_int = TOUCH_IRQ_PIN;
            cfg.bus_shared = true;
            cfg.offset_rotation = 0;
            cfg.spi_host = SPI2_HOST;
            cfg.freq = 2500000;
            cfg.pin_sclk = TFT_SCLK_PIN;
            cfg.pin_mosi = TFT_MOSI_PIN;
            cfg.pin_miso = TFT_MISO_PIN;
            cfg.pin_cs = TOUCH_CS_PIN;
            touch.config(cfg);
            panel.setTouch(&touch);
        }

        setPanel(&panel);
    }
};

RDuinoDisplay lcd;
bool displayReady = false;
SemaphoreHandle_t displayMutex = nullptr;

constexpr uint16_t COLOR_BG = 0x0000;
constexpr uint16_t COLOR_PANEL = 0x0841;
constexpr uint16_t COLOR_TEXT = 0xFFFF;
constexpr uint16_t COLOR_MUTED = 0x9CF3;
constexpr uint16_t COLOR_ACCENT = 0x07FF;
constexpr uint16_t COLOR_WARN = 0xFFE0;
constexpr uint16_t COLOR_OK = 0x07E0;

void drawHeader(const char* title) {
    lcd.fillRect(0, 0, lcd.width(), 28, COLOR_PANEL);
    lcd.setTextColor(COLOR_TEXT, COLOR_PANEL);
    lcd.setTextDatum(textdatum_t::middle_left);
    lcd.setFont(&fonts::Font2);
    lcd.drawString(title, 12, 14);
}

void drawCpuLoad(uint8_t cpu0Load, uint8_t cpu1Load) {
    char text[18];
    snprintf(text, sizeof(text), "C0 %u%% C1 %u%%", cpu0Load, cpu1Load);

    lcd.fillRect(lcd.width() - 128, 0, 128, 28, COLOR_PANEL);
    lcd.setTextColor(COLOR_MUTED, COLOR_PANEL);
    lcd.setTextDatum(textdatum_t::middle_right);
    lcd.setFont(&fonts::Font2);
    lcd.drawString(text, lcd.width() - 8, 14);
}

void drawStatusLine(int y, const char* label, const char* value, uint16_t color) {
    lcd.setTextDatum(textdatum_t::middle_left);
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(COLOR_MUTED, COLOR_BG);
    lcd.drawString(label, 16, y);
    lcd.setTextColor(color, COLOR_BG);
    lcd.drawString(value, 130, y);
}

void drawProgress(uint8_t progress) {
    int x = 24;
    int y = lcd.height() - 58;
    int w = lcd.width() - 48;
    int h = 12;
    int filled = (w * min<uint8_t>(progress, 100)) / 100;

    lcd.drawRect(x, y, w, h, COLOR_MUTED);
    lcd.fillRect(x + 1, y + 1, max(0, filled - 2), h - 2, COLOR_ACCENT);
}

bool lockDisplay(TickType_t timeout = pdMS_TO_TICKS(200)) {
    return !displayMutex || xSemaphoreTake(displayMutex, timeout) == pdTRUE;
}

void unlockDisplay() {
    if (displayMutex) {
        xSemaphoreGive(displayMutex);
    }
}

} // namespace

bool displayBegin() {
    if (displayReady) return true;

    if (!displayMutex) {
        displayMutex = xSemaphoreCreateMutex();
    }
    if (!lockDisplay()) return false;

    if (!lcd.init()) {
        unlockDisplay();
        return false;
    }

    lcd.setRotation(TFT_ROTATION);
    lcd.setBrightness(TFT_BRIGHTNESS);
    lcd.setColorDepth(16);
    lcd.fillScreen(COLOR_BG);
    displayReady = true;
    unlockDisplay();
    return true;
}

void displayShowBootScreen() {
    if (!displayReady) return;
    if (!lockDisplay()) return;

    lcd.fillScreen(COLOR_BG);
    lcd.drawRect(10, 10, lcd.width() - 20, lcd.height() - 20, COLOR_ACCENT);
    lcd.drawCircle(lcd.width() / 2, 118, 48, COLOR_ACCENT);
    lcd.drawLine(lcd.width() / 2 - 58, 118, lcd.width() / 2 + 58, 118, COLOR_ACCENT);
    lcd.drawLine(lcd.width() / 2, 60, lcd.width() / 2, 176, COLOR_ACCENT);

    lcd.setTextDatum(textdatum_t::middle_center);
    lcd.setTextColor(COLOR_TEXT, COLOR_BG);
    lcd.setFont(&fonts::Font4);
    lcd.drawString("rDuinoScope", lcd.width() / 2, 230);
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(COLOR_MUTED, COLOR_BG);
    lcd.drawString("ESP32-S3 interface", lcd.width() / 2, 262);
    lcd.setTextColor(COLOR_ACCENT, COLOR_BG);
    lcd.drawString("Starting controller", lcd.width() / 2, lcd.height() - 48);
    unlockDisplay();
}

void displayShowInitScreen(const char* step, const char* detail, uint8_t progress) {
    if (!displayReady) return;
    if (!lockDisplay()) return;

    lcd.fillScreen(COLOR_BG);
    drawHeader("System init");

    lcd.setTextDatum(textdatum_t::middle_left);
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(COLOR_TEXT, COLOR_BG);
    lcd.drawString(step ? step : "Starting", 18, 152);

    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(COLOR_MUTED, COLOR_BG);
    lcd.drawString(detail ? detail : "", 18, 168);

    drawStatusLine(72,  "Display", "ILI9341", COLOR_OK);
    drawStatusLine(88, "Touch", "XPT2046", COLOR_OK);
    drawStatusLine(102, "Mount link", progress >= 40 ? "ready" : "starting", progress >= 40 ? COLOR_OK : COLOR_WARN);
    drawStatusLine(118, "Network", progress >= 70 ? "ready" : "waiting", progress >= 70 ? COLOR_OK : COLOR_WARN);

    drawProgress(progress);
    unlockDisplay();
}

void displayShowMainScreen(const char* wifiStatus,
                           const char* ipAddress,
                           uint16_t stm32FirmwareVersion,
                           bool motorsEnabled,
                           bool trackingEnabled,
                           uint8_t cpu0Load,
                           uint8_t cpu1Load) {
    if (!displayReady) return;
    if (!lockDisplay()) return;

    char version[12];
    snprintf(version, sizeof(version), "%u.%u",
             (stm32FirmwareVersion >> 8) & 0xFF,
             stm32FirmwareVersion & 0xFF);

    lcd.fillScreen(COLOR_BG);
    drawHeader("Main");
    drawCpuLoad(cpu0Load, cpu1Load);

    lcd.setTextDatum(textdatum_t::middle_left);
    lcd.setFont(&fonts::Font4);
    lcd.setTextColor(COLOR_TEXT, COLOR_BG);
    lcd.drawString("Ready", 18, 45);

    drawStatusLine(72, "WiFi", wifiStatus ? wifiStatus : "unknown", COLOR_OK);
    drawStatusLine(104, "IP", ipAddress ? ipAddress : "-", COLOR_TEXT);
    drawStatusLine(136, "STM32 FW", version, stm32FirmwareVersion ? COLOR_OK : COLOR_WARN);
    drawStatusLine(172, "Motors", motorsEnabled ? "enabled" : "disabled", motorsEnabled ? COLOR_OK : COLOR_WARN);
    drawStatusLine(204, "Tracking", trackingEnabled ? "on" : "off", trackingEnabled ? COLOR_OK : COLOR_MUTED);

    lcd.drawRect(18, lcd.height() - 86, lcd.width() - 36, 54, COLOR_ACCENT);
    lcd.setTextDatum(textdatum_t::middle_center);
    lcd.setTextColor(COLOR_ACCENT, COLOR_BG);
    lcd.drawString("Options", lcd.width() / 2, lcd.height() - 59);
    unlockDisplay();
}

void displayShowCpuLoad(uint8_t cpu0Load, uint8_t cpu1Load) {
    if (!displayReady) return;
    if (!lockDisplay()) return;
    drawCpuLoad(cpu0Load, cpu1Load);
    unlockDisplay();
}

bool displayGetTouch(uint16_t& x, uint16_t& y) {
    if (!displayReady) return false;
    if (!lockDisplay(pdMS_TO_TICKS(100))) return false;
    bool touched = lcd.getTouch(&x, &y);
    unlockDisplay();
    return touched;
}

void displaySetBacklight(uint8_t brightness) {
    if (!displayReady) return;
    if (!lockDisplay()) return;
    lcd.setBrightness(brightness);
    unlockDisplay();
}
