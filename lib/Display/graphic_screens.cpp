#include "graphic_screens.h"
#include <LovyanGFX.hpp>
#include "display.h"
#include "config.h"

namespace {

// Convention:
// - displayShow* functions below are complete paint operations and own the display lock.
// - draw* helpers in this namespace are shared drawing primitives and never lock.

constexpr UiPalette DAY_PALETTE = {
    TFT_BLACK,       // background
    0x0841,          // panel
    TFT_WHITE,       // text
    0x9CF3,          // muted
    TFT_CYAN,        // accent
    TFT_YELLOW,      // warning
    TFT_GREEN,       // ok
    TFT_ORANGE,      // titleBackground
    TFT_BLACK,       // titleText
    TFT_LIGHTGREY,   // bodyText
    TFT_WHITE,       // lightText
    TFT_LIGHTGREY,   // dimText
    TFT_GREENYELLOW, // buttonTextLight
    TFT_DARKGREEN,   // buttonTextDim
    TFT_GREEN,       // buttonBorderLight
    TFT_DARKGREEN,   // buttonBorderDim
    TFT_DARKGREEN,   // buttonSelection
    TFT_PURPLE,      // messageBackground
    TFT_GREENYELLOW, // messageText
    TFT_DARKGREEN,   // stateOn
    TFT_BLACK,       // stateOff
    TFT_BLACK        // buttonTitle
};

constexpr UiPalette NIGHT_PALETTE = {
    TFT_BLACK,  // background
    TFT_MAROON, // panel
    TFT_RED,    // text
    TFT_MAROON, // muted
    TFT_RED,    // accent
    TFT_RED,    // warning
    TFT_RED,    // ok
    TFT_RED,    // titleBackground
    TFT_BLACK,  // titleText
    TFT_MAROON, // bodyText
    TFT_RED,    // lightText
    TFT_MAROON, // dimText
    TFT_RED,    // buttonTextLight
    TFT_MAROON, // buttonTextDim
    TFT_RED,    // buttonBorderLight
    TFT_MAROON, // buttonBorderDim
    TFT_RED,    // buttonSelection
    TFT_RED,    // messageBackground
    TFT_BLACK,  // messageText
    TFT_BLACK,  // stateOn
    TFT_BLACK,  // stateOff
    TFT_BLACK   // buttonTitle
};

UIManager ui;

bool beginScreenDraw(lgfx::LGFX_Device*& lcd) {
    Display& disp = display();
    if (!disp.isReady()) return false;
    if (!disp.lock()) return false;
    lcd = &disp.lcd();
    return true;
}

void endScreenDraw() {
    display().unlock();
}

void drawStatusLine(lgfx::LGFX_Device& lcd, int y, const char* label, const char* value, uint16_t color) {
    const UiPalette& c = uiColors();

    lcd.setTextDatum(textdatum_t::middle_left);
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(c.muted, c.background);
    lcd.drawString(label, 16, y);
    lcd.setTextColor(color, c.background);
    lcd.drawString(value, 130, y);
}

void drawHeader(lgfx::LGFX_Device& lcd, const char* title) {
    const UiPalette& c = uiColors();

    lcd.fillRect(0, 0, lcd.width(), 28, c.panel);
    lcd.setTextColor(c.text, c.panel);
    lcd.setTextDatum(textdatum_t::middle_left);
    lcd.setFont(&fonts::Font2);
    lcd.drawString(title, 12, 14);
}

void drawLegacyHeader(lgfx::LGFX_Device& lcd) {
    const UiPalette& c = uiColors();

    lcd.fillRect(0, 1, lcd.width(), 90, c.titleBackground);
    lcd.drawLine(0, 27, lcd.width(), 27, c.accent);
    lcd.drawLine(0, 92, lcd.width(), 92, c.titleBackground);

    lcd.setTextSize(1);
    lcd.setTextColor(c.titleText, c.buttonTitle);
}

void drawStatusBar(lgfx::LGFX_Device& lcd) {
    const UiPalette& c = uiColors();

    lcd.fillRect(0, 1, lcd.width(), 26, c.titleBackground);
    lcd.setFont(&fonts::Font0);
    lcd.setTextSize(1);
    lcd.setTextColor(c.stateOff);

    // Brightness
    lcd.setCursor(3, 4);
    lcd.print("Bright");

    lcd.setCursor(10, 15);
    lcd.printf("%u%%", static_cast<unsigned>((displayGetBacklight() * 100U) / 255U));
    
    // T-out
    lcd.setCursor(47, 4);
    lcd.print("T-out");

    lcd.setCursor(85, 4);
    lcd.print("Sound");
    lcd.setCursor(85, 15);
    lcd.print("OFF");
  
    lcd.setCursor(125, 4);
    lcd.print("Motors");
    lcd.setCursor(125, 15);
    lcd.print("OFF");
}

void drawBatteryLevel(lgfx::LGFX_Device& lcd, uint8_t x, uint8_t y, int level) {
    const UiPalette& c = uiColors();
    lcd.drawRect(x, y, 23, 12, c.stateOff);
    lcd.drawRect(x + 22, y + 3, 3, 6, c.stateOff);
    lcd.fillRect(x + 1, y + 1, 20, 10, c.titleBackground);

    if (level == -1) {
        lcd.setCursor(x + 3, y + 3);
        lcd.setTextSize(1);
        lcd.setTextColor(c.buttonTextDim);
        lcd.print("USB");
    } else if (level > 20) {
        lcd.fillRect(x + 2, y + 2, round((double)level / 5) - 1, 8, c.stateOff);
    } else {
        uint16_t fillColor = displayIsNightMode() ? c.buttonTextDim : TFT_RED;
        lcd.fillRect(x + 2, y + 2, round((double)level / 5), 8, fillColor);
    }
}

void drawStatusBarWithBattery(lgfx::LGFX_Device& lcd, int batteryLevel) {
    drawStatusBar(lcd);
    drawBatteryLevel(lcd, lcd.width() - 30, 4, batteryLevel);
}

void drawCpuLoad(lgfx::LGFX_Device& lcd, uint8_t cpu0Load, uint8_t cpu1Load) {
    const UiPalette& c = uiColors();
    char text[18];
    snprintf(text, sizeof(text), "C0 %u%% C1 %u%%", cpu0Load, cpu1Load);

    lcd.fillRect(lcd.width() - 128, 0, 128, 28, c.panel);
    lcd.setTextColor(c.muted, c.panel);
    lcd.setTextDatum(textdatum_t::middle_right);
    lcd.setFont(&fonts::Font2);
    lcd.drawString(text, lcd.width() - 8, 14);
}

void drawProgress(lgfx::LGFX_Device& lcd, uint8_t progress) {
    const UiPalette& c = uiColors();

    int x = 24;
    int y = lcd.height() - 58;
    int w = lcd.width() - 48;
    int h = 12;
    int filled = (w * min<uint8_t>(progress, 100)) / 100;

    lcd.drawRect(x, y, w, h, c.muted);
    lcd.fillRect(x + 1, y + 1, max(0, filled - 2), h - 2, c.accent);
}

const char* bootStatusText(BootStatus status) {
    switch (status) {
        case BootStatus::Pending: return "WAIT";
        case BootStatus::Running: return "...";
        case BootStatus::Ok:      return "OK";
        case BootStatus::Fail:    return "FAIL";
        case BootStatus::Skip:    return "SKIP";
    }
    return "?";
}

uint16_t bootStatusColor(BootStatus status) {
    const UiPalette& c = uiColors();
    switch (status) {
        case BootStatus::Pending: return c.muted;
        case BootStatus::Running: return c.warning;
        case BootStatus::Ok:      return c.ok;
        case BootStatus::Fail:    return TFT_RED;
        case BootStatus::Skip:    return c.muted;
    }
    return c.muted;
}

void drawBootStatusLine(lgfx::LGFX_Device& lcd, uint8_t row, const char* label, BootStatus status) {
    const UiPalette& c = uiColors();
    const int y = 132 + (row * 12);
    const int labelX = 18;
    const int statusX = lcd.width() - 18;

    lcd.fillRect(12, y - 8, lcd.width() - 24, 16, c.background);
    lcd.setFont(&fonts::DejaVu9);
    lcd.setTextDatum(textdatum_t::middle_left);
    lcd.setTextColor(c.text, c.background);
    lcd.drawString(label ? label : "-", labelX, y);

    lcd.setTextDatum(textdatum_t::middle_right);
    lcd.setTextColor(bootStatusColor(status), c.background);
    lcd.drawString(bootStatusText(status), statusX, y);
}

} // namespace

void UIManager::setScreen(ScreenType newScreen) {
    currentScreen = newScreen;
}

ScreenType UIManager::getCurrentScreen() const {
    return currentScreen;
}

void UIManager::setNightMode(bool enabled) {
    nightMode = enabled;
}

bool UIManager::isNightMode() const {
    return nightMode;
}

const UiPalette& UIManager::colors() const {
    return nightMode ? NIGHT_PALETTE : DAY_PALETTE;
}

const UiPalette& uiColors() {
    return ui.colors();
}

const UiPalette& uiDayPalette() {
    return DAY_PALETTE;
}

const UiPalette& uiNightPalette() {
    return NIGHT_PALETTE;
}

void displaySetNightMode(bool enabled) {
    ui.setNightMode(enabled);
}

bool displayIsNightMode() {
    return ui.isNightMode();
}

#if 0
void displayShowBootScreen() {
    lgfx::LGFX_Device* lcd = nullptr;
    if (!beginScreenDraw(lcd)) return;

    const UiPalette& c = uiColors();
    lcd->fillScreen(c.background);
    lcd->drawRect(10, 10, lcd->width() - 20, lcd->height() - 20, c.accent);
    lcd->drawCircle(lcd->width() / 2, 118, 48, c.accent);
    lcd->drawLine(lcd->width() / 2 - 58, 118, lcd->width() / 2 + 58, 118, c.accent);
    lcd->drawLine(lcd->width() / 2, 60, lcd->width() / 2, 176, c.accent);

    lcd->setTextDatum(textdatum_t::middle_center);
    lcd->setTextColor(c.titleBackground, c.background);
    lcd->setFont(&fonts::Font4);
    lcd->drawString("rDuinoScope", lcd->width() / 2, 230);
    lcd->setFont(&fonts::Font2);
    lcd->setTextColor(c.muted, c.background);
    lcd->drawString("ESP32-S3 interface", lcd->width() / 2, 262);
    lcd->setTextColor(c.accent, c.background);
    lcd->drawString("Starting controller", lcd->width() / 2, lcd->height() - 48);

    endScreenDraw();
}
#else
void displayShowBootScreen() {
    lgfx::LGFX_Device* lcd = nullptr;
    if (!beginScreenDraw(lcd)) return;

    const UiPalette& c = uiColors();
    char version[12];
    snprintf(version, sizeof(version), "%02X.%02X",
             (ESP32_FIRMWARE_VERSION >> 8) & 0xFF,
             ESP32_FIRMWARE_VERSION & 0xFF);

    lcd->fillScreen(c.background);
    // Title
    lcd->setTextDatum(textdatum_t::middle_center);
    lcd->setTextColor(c.titleBackground, c.background);
    lcd->setFont(&fonts::Orbitron_Light_24);
    lcd->drawString("rDUINOScope 2.0", lcd->width() / 2, 14);
    lcd->setFont(&fonts::DejaVu9);
    lcd->setTextColor(c.text, c.background);
    lcd->drawString("coded by <giacu92>", lcd->width() / 2, 34);
    //lcd->setFont(&fonts::DejaVu12);
    //lcd->drawString("Giacomo Mammarella", lcd->width() / 2, 52);
    lcd->drawString("rduinoscope.byethost24.com", lcd->width() / 2, 58);
    String versionText = String("Version: v") + version + " - " + ESP32_FIRMWARE_NAME;
    lcd->drawString(versionText, lcd->width() / 2, 70);
    lcd->drawString(" (c) 2026 giacu92", lcd->width() / 2, 82);
    lcd->drawString("GNU General Public License", lcd->width() / 2, 94);


    drawBootStatusLine(*lcd, 0, "Touchscreen XPT2046", BootStatus::Pending);
    drawBootStatusLine(*lcd, 1, "WiFi", BootStatus::Pending);
    drawBootStatusLine(*lcd, 2, "Clock / NTP", BootStatus::Pending);
    drawBootStatusLine(*lcd, 3, "GPS", BootStatus::Pending);
    drawBootStatusLine(*lcd, 4, "BME280", BootStatus::Pending);
    drawBootStatusLine(*lcd, 5, "RTC", BootStatus::Pending);
    drawBootStatusLine(*lcd, 6, "SD Card", BootStatus::Pending);
    drawBootStatusLine(*lcd, 7, "Modbus (STM32F)", BootStatus::Pending);
    drawBootStatusLine(*lcd, 8, "TCP server", BootStatus::Pending);
    drawBootStatusLine(*lcd, 9, "Custom configs", BootStatus::Pending);
    endScreenDraw();
}
#endif

void displayBootSetStatus(uint8_t row, const char* label, BootStatus status) {
    lgfx::LGFX_Device* lcd = nullptr;
    if (!beginScreenDraw(lcd)) return;

    drawBootStatusLine(*lcd, row, label, status);

    endScreenDraw();
}

void displayShowInitScreen(const char* step, const char* detail, uint8_t progress) {
    lgfx::LGFX_Device* lcd = nullptr;
    if (!beginScreenDraw(lcd)) return;

    const UiPalette& c = uiColors();
    lcd->fillScreen(c.background);

    drawHeader(*lcd, "System init");

    lcd->setTextDatum(textdatum_t::middle_left);
    lcd->setFont(&fonts::Font2);
    lcd->setTextColor(c.text, c.background);
    lcd->drawString(step ? step : "Starting", 18, 152);

    lcd->setTextColor(c.muted, c.background);
    lcd->drawString(detail ? detail : "", 18, 168);

    drawStatusLine(*lcd, 72, "Display", "ILI9341", c.ok);
    drawStatusLine(*lcd, 88, "Touch", "XPT2046", c.ok);
    drawStatusLine(*lcd, 102, "Mount link", progress >= 40 ? "ready" : "starting", progress >= 40 ? c.ok : c.warning);
    drawStatusLine(*lcd, 118, "Network", progress >= 70 ? "ready" : "waiting", progress >= 70 ? c.ok : c.warning);

    drawProgress(*lcd, progress);
    endScreenDraw();
}

void displayShowMainScreen(const char* wifiStatus,
                           const char* ipAddress,
                           uint16_t stm32FirmwareVersion,
                           bool motorsEnabled,
                           bool trackingEnabled,
                           uint8_t cpu0Load,
    uint8_t cpu1Load) {
    lgfx::LGFX_Device* lcd = nullptr;
    if (!beginScreenDraw(lcd)) return;

    const UiPalette& c = uiColors();
    char version[12];
    snprintf(version, sizeof(version), "v%02X.%02X",
             (stm32FirmwareVersion >> 8) & 0xFF,
             stm32FirmwareVersion & 0xFF);

    lcd->fillScreen(c.background);

    drawLegacyHeader(*lcd);
    drawStatusBarWithBattery(*lcd, -1);

    lcd->setTextDatum(textdatum_t::middle_left);
    lcd->setFont(&fonts::Font4);
    lcd->setTextColor(c.text, c.background);
    lcd->drawString("Ready", 18, 45);

    drawStatusLine(*lcd, 72, "WiFi", wifiStatus ? wifiStatus : "unknown", c.ok);
    drawStatusLine(*lcd, 104, "IP", ipAddress ? ipAddress : "-", c.text);
    drawStatusLine(*lcd, 136, "STM32 FW", version, stm32FirmwareVersion ? c.ok : c.warning);
    drawStatusLine(*lcd, 172, "Motors", motorsEnabled ? "enabled" : "disabled", motorsEnabled ? c.ok : c.warning);
    drawStatusLine(*lcd, 204, "Tracking", trackingEnabled ? "on" : "off", trackingEnabled ? c.ok : c.muted);

    lcd->drawRect(18, lcd->height() - 86, lcd->width() - 36, 54, c.accent);
    lcd->setTextDatum(textdatum_t::middle_center);
    lcd->setTextColor(c.accent, c.background);
    lcd->drawString("Options", lcd->width() / 2, lcd->height() - 59);

    endScreenDraw();
}

void displayShowCpuLoad(uint8_t cpu0Load, uint8_t cpu1Load) {
    lgfx::LGFX_Device* lcd = nullptr;
    if (!beginScreenDraw(lcd)) return;

    drawCpuLoad(*lcd, cpu0Load, cpu1Load);

    endScreenDraw();
}
