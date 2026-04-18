#include "display.h"

#include <freertos/semphr.h>

#include "config.h"

namespace {

class RDuinoDisplay : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9341 panel;
    lgfx::Bus_SPI bus;
    lgfx::Light_PWM light;
    lgfx::Touch_XPT2046 touch;
    uint8_t backlightBrightness = TFT_BRIGHTNESS;

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

    void setBacklightBrightness(uint8_t brightness) {
        backlightBrightness = brightness;
        setBrightness(backlightBrightness);
    }

    uint8_t getBacklightBrightness() const {
        return backlightBrightness;
    }

    bool probeTouch() {
        return touch.init();
    }
};

RDuinoDisplay lcdDevice;
Display displayInstance;
bool ready = false;
SemaphoreHandle_t mutex = nullptr;

} // namespace

bool Display::begin() {
    if (ready) return true;

    if (!mutex) {
        mutex = xSemaphoreCreateMutex();
    }
    if (!lock()) return false;

    if (!lcdDevice.init()) {
        unlock();
        return false;
    }

    lcdDevice.setRotation(TFT_ROTATION);
    lcdDevice.setBacklightBrightness(TFT_BRIGHTNESS);
    lcdDevice.setColorDepth(16);
    ready = true;
    unlock();
    return true;
}

bool Display::isReady() const {
    return ready;
}

bool Display::lock(TickType_t timeout) {
    return !mutex || xSemaphoreTake(mutex, timeout) == pdTRUE;
}

void Display::unlock() {
    if (mutex) {
        xSemaphoreGive(mutex);
    }
}

lgfx::LGFX_Device& Display::lcd() {
    return lcdDevice;
}

bool Display::getTouch(uint16_t& x, uint16_t& y) {
    if (!ready) return false;
    if (!lock(pdMS_TO_TICKS(100))) return false;
    bool touched = lcdDevice.getTouch(&x, &y);
    unlock();
    return touched;
}

bool Display::probeTouch() {
    if (!ready) return false;
    if (!lock(pdMS_TO_TICKS(100))) return false;
    bool ok = lcdDevice.probeTouch();
    unlock();
    return ok;
}

void Display::setBacklight(uint8_t brightness) {
    if (!ready) return;
    if (!lock()) return;
    lcdDevice.setBacklightBrightness(brightness);
    unlock();
}

uint8_t Display::getBacklight() const {
    return lcdDevice.getBacklightBrightness();
}

Display& display() {
    return displayInstance;
}

bool displayBegin() {
    return display().begin();
}

bool displayGetTouch(uint16_t& x, uint16_t& y) {
    return display().getTouch(x, y);
}

bool displayProbeTouch() {
    return display().probeTouch();
}

void displaySetBacklight(uint8_t brightness) {
    display().setBacklight(brightness);
}

uint8_t displayGetBacklight() {
    return display().getBacklight();
}
