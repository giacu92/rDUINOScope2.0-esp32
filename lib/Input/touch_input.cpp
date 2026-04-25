#include "touch_input.h"

#include <freertos/queue.h>

#include "config.h"
#include "display.h"

namespace {

constexpr TickType_t TOUCH_IDLE_FALLBACK_TICKS = pdMS_TO_TICKS(1000);
constexpr TickType_t TOUCH_ACTIVE_SAMPLE_TICKS = pdMS_TO_TICKS(50);
constexpr uint32_t TOUCH_EVENT_DEBOUNCE_MS = 80;

TaskHandle_t touchTaskHandle = nullptr;
QueueHandle_t touchEventQueue = nullptr;
TouchInputChangedCallback changedCallback = nullptr;
bool touchInputStarted = false;
uint32_t lastPressedEventMs = 0;
uint32_t lastReleasedEventMs = 0;

void notifyChanged() {
    if (changedCallback) {
        changedCallback();
    }
}

void publishTouchEvent(TouchEventType type, uint16_t x, uint16_t y) {
    if (!touchEventQueue) return;

    uint32_t now = millis();
    uint32_t& lastEventMs = (type == TouchEventType::Pressed)
                          ? lastPressedEventMs
                          : lastReleasedEventMs;
    if (lastEventMs != 0 && now - lastEventMs < TOUCH_EVENT_DEBOUNCE_MS) {
        return;
    }
    lastEventMs = now;

    TouchEvent event = {
        type,
        x,
        y,
        now
    };

    xQueueSend(touchEventQueue, &event, 0);
    notifyChanged();
}

void IRAM_ATTR touchIrqHandler() {
    if (!touchTaskHandle) return;

    BaseType_t higherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(touchTaskHandle, &higherPriorityTaskWoken);
    if (higherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

void touchTask(void* pvParams) {
    (void)pvParams;

    bool wasTouching = false;
    uint16_t lastX = 0;
    uint16_t lastY = 0;

    for (;;) {
        TickType_t waitTicks = wasTouching ? TOUCH_ACTIVE_SAMPLE_TICKS : TOUCH_IDLE_FALLBACK_TICKS;
        ulTaskNotifyTake(pdTRUE, waitTicks);

        uint16_t x = 0;
        uint16_t y = 0;
        bool touching = displayGetTouch(x, y);

        if (touching && !wasTouching && DEBUG_LX200_FULL) {
            Serial.printf("[TOUCH] x=%u y=%u\n", x, y);
        }
        if (touching) {
            lastX = x;
            lastY = y;
        }

        if (touching && !wasTouching) {
            publishTouchEvent(TouchEventType::Pressed, x, y);
        } else if (!touching && wasTouching) {
            publishTouchEvent(TouchEventType::Released, lastX, lastY);
        }

        wasTouching = touching;
    }
}

} // namespace

void touchInputBegin(BaseType_t taskCore, TouchInputChangedCallback onTouchEvent) {
    if (touchInputStarted) return;

    changedCallback = onTouchEvent;
    touchEventQueue = xQueueCreate(8, sizeof(TouchEvent));
    xTaskCreatePinnedToCore(touchTask, "touch", 3072, nullptr, 1, &touchTaskHandle, taskCore);
    pinMode(TOUCH_IRQ_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(TOUCH_IRQ_PIN), touchIrqHandler, FALLING);

    touchInputStarted = true;
}

bool touchInputGetEvent(TouchEvent& event, TickType_t waitTicks) {
    if (!touchEventQueue) return false;
    return xQueueReceive(touchEventQueue, &event, waitTicks) == pdTRUE;
}
