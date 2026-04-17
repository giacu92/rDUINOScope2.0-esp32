# rDUINOScope 2.0 - ESP32-S3 Interface Firmware

This repository contains the ESP32-S3 side of the rDUINOScope 2.0 controller.
The full system is split across two MCUs: the ESP32-S3 handles WiFi,
Stellarium connectivity, LX200 compatibility, time synchronization, and user
facing status; the STM32F401 runs the time-critical motor control loop.

The split is intentional. WiFi and TCP traffic can introduce latency and task
scheduling jitter, while stepper pulse generation needs deterministic timing.
The ESP32 therefore acts as the network-facing master and exchanges compact
position and command data with the STM32 over Modbus RTU.

## Main Responsibilities

The firmware exposes a TCP telescope server on port `10003` for Stellarium and
compatible clients. It supports both Stellarium Desktop and Stellarium Mobile on
the same port:

- Stellarium Desktop uses the native binary telescope protocol.
- Stellarium Mobile and similar clients use LX200-style ASCII commands over TCP.

The protocol is detected automatically from the first received byte of a new TCP
session. Binary packets are treated as Stellarium Desktop traffic; text commands
are handled by the LX200 parser.

The ESP32 also keeps local telescope state for the client-facing protocol layer:
current RA/DEC, target RA/DEC, site coordinates, LX200 date/time, and slewing
state. Actual motor movement and tracking are delegated to the STM32.

## Hardware Target

The current PlatformIO environment targets:

```ini
board = esp32-s3-devkitc-1
framework = arduino
```

The expected board is an ESP32-S3 DevKitC-1 style module with an onboard
WS2812/NeoPixel RGB LED on GPIO 48.

Important ESP32 pin assignments are defined in `lib/Telescope/config.h`:

| Function | GPIO | Notes |
| :-- | :-- | :-- |
| Modbus TX | 17 | ESP32 UART TX toward STM32/RS485 side |
| Modbus RX | 16 | ESP32 UART RX from STM32/RS485 side |
| RS485 DE | 4 | Driver enable, active HIGH |
| RGB LED | 48 | Onboard WS2812/NeoPixel status LED |
| TFT SCLK | 10 | Provisional SPI clock for ILI9341/ILI9488 |
| TFT MOSI | 11 | Provisional SPI MOSI for display/touch |
| TFT MISO | 46 | Provisional SPI MISO for touch/display reads |
| TFT CS | 14 | Provisional display chip select |
| TFT DC | 12 | Provisional display data/command |
| TFT RST | 13 | Provisional display reset |
| TFT BL | 9 | Provisional backlight PWM |
| Touch CS | 15 | Provisional XPT2046 chip select |
| Touch IRQ | 18 | Provisional XPT2046 interrupt |

Modbus runs on `Serial2` at `57600` baud, using slave ID `1` on the STM32 side.

The display/touch pins are Milestone 1 defaults and must be checked against the
final wiring. GPIO 4 is intentionally avoided for touch chip select because it is
already used by RS485 driver enable.

## WiFi and Time

WiFi credentials are read from `lib/Telescope/secrets.h` through the `Secrets`
helper class. Keep real credentials local and avoid committing personal network
details to a shared repository.

After WiFi connects, the firmware synchronizes time through NTP:

```cpp
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 3600;
const int   DAYLIGHT_OFFSET_SEC = 3600;
```

The LX200 time model can also be updated by clients through date/time commands.
When a client manually sets date or time, the firmware avoids overwriting it
immediately with NTP-derived time.

## Status LED

The onboard RGB LED is handled by a low-priority FreeRTOS task, separate from
the main TCP loop. It currently reports WiFi state:

| Color | Meaning |
| :-- | :-- |
| Red | WiFi disconnected or connecting |
| Blue | WiFi connected |
| Purple breathing at 0.25 Hz | Stellarium client connected |
| Yellow | Reserved for future captive portal mode |

Brightness is intentionally low:

```cpp
constexpr uint8_t RGB_LED_BRIGHTNESS = 16;  // about 6%
```

When captive portal support is added, it can switch the indicator with:

```cpp
setWifiLedState(WIFI_LED_CAPTIVE_PORTAL);
```

## Display And Touch

Milestone 1 starts the local UI layer with LovyanGFX driving an SPI TFT
display and XPT2046-compatible touch controller. The current bench setup uses
an ILI9341 panel; the intended larger UI can move back to ILI9488 by changing
the LovyanGFX panel type and display geometry constants. The current firmware
shows:

- boot screen immediately after serial startup;
- init screen while LED, Modbus, WiFi, NTP, STM32 status, and TCP server are
  initialized;
- static main screen with WiFi, IP, STM32 firmware, motors, and tracking state.

The display code lives in `lib/Display/display.h` and
`lib/Display/display.cpp`. It is intentionally separate from the Stellarium and
Modbus code so future screens can read state and emit high-level events without
owning hardware services directly.

After boot, `displayTask` is responsible for rendering screens when visible
state changes. Setup may draw the early boot and
initialization screens before the task starts; from the main screen onward,
other code should publish state or enqueue high-level UI actions instead of
drawing directly.

Touch input has its own path. The XPT2046 IRQ wakes `touchTask`, and that task
reads coordinates outside the interrupt handler. The ISR only notifies the task:
it does not use SPI, LovyanGFX, logging, or UI logic. Because display and touch
share the same SPI/LovyanGFX device, the public display functions are protected
by a short mutex. The touch task also keeps a slow idle fallback check and, once
a press is active, samples until release so future buttons can get stable press
and release events.

## Stellarium Command Flow

For a GOTO request, the ESP32 receives a target from either protocol path:

- `:Sr` and `:Sd` followed by `:MS#` from LX200 clients.
- A 20-byte binary GOTO packet from Stellarium Desktop.

The target is converted to the shared Modbus coordinate format:

```text
RA  = hours * 15 * 3600 * 100
DEC = degrees * 3600 * 100
```

Those signed 32-bit values are split across two 16-bit request registers each.
The ESP32 writes `REG_REQ_COMMAND=1`, then sets `REG_REQ_COMMAND_PENDING=1`.

The STM32 owns the actual movement. It performs acceleration, deceleration,
tracking transition, and error handling. The ESP32 polls the STM32 status and
position registers, then reports the current position back to Stellarium.

Milestone 0 extends the same boundary with high-level mount controls for the
future local UI: tracking enable/mode, motors enable/disable, and manual jog.
The ESP32 still does not generate motor pulses; it only writes intent into
Modbus registers. Registers named `REG_REQ_*` are Modbus Master-side requests;
registers named `REG_RES_*` are Modbus Slave-side responses. The one deliberate
exception is `REG_REQ_COMMAND_PENDING`: ESP32 sets it to `1` after writing a
command, and STM32 clears that pending flag back to `0` after consuming the
command.

The STM32 firmware already defines `CMD_SYNC=3` and `CMD_FOLLOW_TARGET=4`; the
ESP32 keeps those values reserved. Manual jog intentionally uses dedicated
axis/direction/speed registers instead of `FOLLOW_TARGET`, so the STM32 remains
responsible for jog speed profiles, acceleration, deceleration, limits, and
safety handling.

## Modbus Register Boundary

The ESP32 and STM32 share this compact register map:

| Register | Name | Direction from ESP32 view | Meaning |
| :-- | :-- | :-- | :-- |
| 0 | `REG_REQ_TARGET_RA_HIGH` | Write | Target RA high word |
| 1 | `REG_REQ_TARGET_RA_LOW` | Write | Target RA low word |
| 2 | `REG_REQ_TARGET_DEC_HIGH` | Write | Target DEC high word |
| 3 | `REG_REQ_TARGET_DEC_LOW` | Write | Target DEC low word |
| 4 | `REG_REQ_COMMAND` | Write | ESP32-owned command request/state |
| 5 | `REG_RES_STATUS` | Read | STM32 state |
| 6 | `REG_RES_CURRENT_RA_HIGH` | Read | Current RA high word |
| 7 | `REG_RES_CURRENT_RA_LOW` | Read | Current RA low word |
| 8 | `REG_RES_CURRENT_DEC_HIGH` | Read | Current DEC high word |
| 9 | `REG_RES_CURRENT_DEC_LOW` | Read | Current DEC low word |
| 10 | `REG_RES_ERROR_CODE` | Read | STM32 error code |
| 11 | `REG_REQ_TRACKING_ENABLE` | Write | `0` off, `1` on |
| 12 | `REG_REQ_TRACKING_MODE` | Write | `0` lunar, `1` sidereal, `2` solar |
| 13 | `REG_REQ_MOTORS_ENABLE` | Write | `0` disabled, `1` enabled |
| 14 | `REG_REQ_JOG_AXIS` | Write | `0` RA, `1` DEC |
| 15 | `REG_REQ_JOG_DIRECTION` | Write | `0` negative/west/south, `1` positive/east/north |
| 16 | `REG_REQ_JOG_SPEED` | Write | STM32-defined jog speed/profile |
| 17 | `REG_REQ_COMMAND_PENDING` | Write/consume | ESP32 sets to `1`; STM32 clears to `0` after consuming `REG_REQ_COMMAND` |
| 18 | `REG_RES_STM32_FW_VERSION` | Read | STM32 firmware version, packed as `0xMMmm` |

Command values written to `REG_REQ_COMMAND`:

| Value | Command |
| :-- | :-- |
| 0 | `CMD_NONE` |
| 1 | `CMD_GOTO` |
| 2 | `CMD_STOP` |
| 3 | `CMD_SYNC` |
| 4 | `CMD_FOLLOW_TARGET` |
| 5 | `CMD_SET_TRACKING` |
| 6 | `CMD_SET_MOTORS` |
| 7 | `CMD_JOG_START` |
| 8 | `CMD_JOG_STOP` |

`REG_REQ_COMMAND` may remain at the last requested command. A new command is
signaled by setting `REG_REQ_COMMAND_PENDING=1`, so repeated identical commands
do not depend on short timed pulses.

`REG_RES_STM32_FW_VERSION` stores the STM32 firmware version as `0xMMmm`, where
the high byte is the major version and the low byte is the minor version. For
example, `0x0200` means `2.0`.

`CMD_JOG_START` reads `REG_REQ_JOG_AXIS`, `REG_REQ_JOG_DIRECTION`, and
`REG_REQ_JOG_SPEED`, then STM32 should enter `MANUAL_JOG` and move the requested
axis until `CMD_JOG_STOP` or `CMD_STOP` arrives. `CMD_JOG_STOP` is the normal
button-release path; `CMD_STOP` remains the priority abort path.

State values mirror the STM32 firmware:

| Value | State |
| :-- | :-- |
| 0 | `IDLE` |
| 1 | `SLEWING` |
| 2 | `TRACKING` |
| 3 | `ERROR` |
| 4 | `MOTORS_DISABLED` |
| 5 | `MANUAL_JOG` |

## FreeRTOS Layout

The firmware keeps blocking or slower work away from the main TCP loop:

- Arduino `loop()` is pinned to core 1 by `CONFIG_ARDUINO_RUNNING_CORE=1` and
  handles Stellarium TCP traffic plus periodic position packets.
- `displayTask` runs on core 1 and renders screens when a refresh is needed.
- `touchTask` runs on core 1, is woken by the XPT2046 IRQ, and reads touch
  coordinates outside the ISR.
- `modbusTask` runs on core 0 and owns all Modbus communication with the STM32.
- `statusLedTask` runs on core 0 at low priority and updates the RGB LED state.

GOTO commands are passed to the Modbus task through a FreeRTOS queue. This keeps
the network path responsive while the STM32 is being polled for motion status.
The Modbus task publishes a compact mount-state snapshot protected by a mutex;
the TCP and display paths copy that snapshot briefly instead of reading live
cross-core state directly.

The main screen also shows an indicative per-core CPU load in the top-right
header area. `cpuLoadTask` samples FreeRTOS idle hooks once per second and
updates only that small display region, avoiding a full-screen redraw. The
numbers are useful for balancing tasks across cores, but they are an idle-time
estimate rather than a precise profiler.

## Configuration Points

Most project-level constants are in `lib/Telescope/config.h`:

- WiFi credential access
- Debug flags
- Site latitude and longitude
- Stellarium TCP port and update interval
- NTP server and timezone offsets
- Modbus baud rate, slave ID, and ESP32 pins
- Onboard RGB LED pin and brightness

PlatformIO board and build options are in `platformio.ini`.

## Build and Upload

Build the firmware with PlatformIO:

```bash
pio run
```

Upload to the ESP32-S3:

```bash
pio run -t upload
```

Open the serial monitor at `115200` baud:

```bash
pio device monitor
```

The configured upload speed is `921600`.

## Dependencies

The project uses:

- `4-20ma/ModbusMaster` for Modbus RTU master communication.
- `adafruit/Adafruit NeoPixel` for the onboard WS2812 status LED.
- `lovyan03/LovyanGFX` for SPI TFT display and XPT2046 touch support.
- Arduino ESP32 `WiFi` and time APIs.

Dependencies are declared in `platformio.ini`.

## Current Limitations

Captive portal provisioning is planned but not implemented yet. The yellow LED
state is already reserved for that mode.

The ESP32 currently assumes the STM32 register map and coordinate format used by
the matching rDUINOScope 2.0 STM32 firmware. If the STM32 register layout
changes, the constants in `config.h` and the Modbus packing code must be kept in
sync.

## Related Firmware

This firmware is designed to work with the rDUINOScope 2.0 STM32F401 motor
control firmware. The STM32 project documents the motor loop, driver handling,
encoder support, hard stop behavior, and detailed Modbus slave implementation.
This repository intentionally focuses on the ESP32 network and protocol side.
