#include "touch_input.h"

#include "config.h"
#include "display.h"

namespace {

constexpr TickType_t TOUCH_IDLE_FALLBACK_TICKS = pdMS_TO_TICKS(1000);
constexpr TickType_t TOUCH_ACTIVE_SAMPLE_TICKS = pdMS_TO_TICKS(50);

TaskHandle_t touchTaskHandle = nullptr;
bool touchInputStarted = false;

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

    for (;;) {
        TickType_t waitTicks = wasTouching ? TOUCH_ACTIVE_SAMPLE_TICKS : TOUCH_IDLE_FALLBACK_TICKS;
        ulTaskNotifyTake(pdTRUE, waitTicks);

        uint16_t x = 0;
        uint16_t y = 0;
        bool touching = displayGetTouch(x, y);

        if (touching && !wasTouching && DEBUG_LX200_FULL) {
            Serial.printf("[TOUCH] x=%u y=%u\n", x, y);
        }

        wasTouching = touching;
    }
}

} // namespace

void touchInputBegin(BaseType_t taskCore) {
    if (touchInputStarted) return;

    xTaskCreatePinnedToCore(touchTask, "touch", 3072, nullptr, 1, &touchTaskHandle, taskCore);
    pinMode(TOUCH_IRQ_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(TOUCH_IRQ_PIN), touchIrqHandler, FALLING);

    touchInputStarted = true;
}
