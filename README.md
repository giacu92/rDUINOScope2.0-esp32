# rDUINOScope 2.0 ESP32-S3 Interface Firmware

This repository contains the ESP32-S3 firmware for the rDUINOScope 2.0
controller.

The system is intentionally split across two MCUs:

- The ESP32-S3 owns WiFi, OTA updates, Stellarium/LX200 connectivity, local UI,
  touch input, display rendering, time sources, user configuration, and high
  level mount commands.
- The STM32F401 owns deterministic mount control: step generation, tracking,
  acceleration/deceleration, motor safety, stop behavior, and real machine
  state.

That boundary is the core design decision. The ESP32 is allowed to run rich and
occasionally non-deterministic services; the STM32 remains responsible for all
time-critical motion.

## Project Shape

The current firmware is the network/UI side of the controller, not a complete
standalone telescope mount firmware. It is designed to work with the matching
rDUINOScope 2.0 STM32 firmware over Modbus RTU.

Implemented today:

- TCP telescope server on port `10003`;
- automatic protocol detection between Stellarium Desktop binary packets and
  LX200 ASCII commands;
- LX200 command handling for common Stellarium/SkySafari flows;
- NTP time synchronization;
- ArduinoOTA after WiFi connection;
- Modbus RTU master link to STM32;
- mount command queue for GOTO, STOP, tracking, motors, and jog intents;
- boot diagnostics screen;
- main display screen with WiFi, client, STM32 firmware, and mount state;
- LovyanGFX display service for SPI TFT and XPT2046 touch;
- debounced touch task with press/release events;
- basic UI routing and soft-button pages;
- onboard WS2812 status LED;
- night-mode palette input through an active-low GPIO.

Planned or incomplete areas:

- SD storage and local catalogs;
- GPS and external RTC;
- BME280/environment data;
- battery measurement;
- joystick/manual control UI;
- alignment workflow;
- meridian flip workflow;
- full options screen;
- session logging and observation reports;
- UI-driven firmware/catalog updates.

## Design Principles

The codebase should stay modular because this project is a port from a much more
monolithic legacy firmware.

Guidelines:

- ESP32 never generates step pulses.
- UI code publishes user intent; it must not directly manipulate motor timing or
  Modbus registers inside rendering code.
- STM32 state is treated as authoritative for movement and current position.
- STOP and motors-off paths must remain priority paths.
- Slow work should run outside the TCP loop.
- Blocking hardware operations should live in tasks or short service calls.
- Shared state should be copied through snapshots or small APIs, not read
  cross-module as mutable globals.
- Legacy features should be ported by behavior, not by copying `.ino` structure.
- Protocol contracts should be documented near the code that owns them.

## Runtime Architecture

| Area | Path | Responsibility |
| --- | --- | --- |
| Application | `src/main.cpp` | startup, WiFi, OTA, TCP server, Stellarium/LX200 routing, task creation |
| Mount link | `lib/Mount` | Modbus master, RS485 direction, command queue, STM32 polling, mount snapshot |
| Display hardware | `lib/Display/display.*` | LovyanGFX setup, SPI TFT, XPT2046 touch, display mutex, backlight |
| UI screens | `lib/Display/graphic_screens.*` | palette, boot/main/options screens, UI routing, high-level UI actions |
| Touch input | `lib/Input` | touch IRQ wakeup, task-side touch reads, debounce, event queue |
| Telescope model | `lib/Telescope` | configuration constants, LX200 formatting, controller clock, site coordinates |
| System services | `lib/System` | optional diagnostics and slow board-level inputs |
| Astronomy helpers | `lib/planetcalc.h` | approximate planetary calculations for future local features |

The application layer should call APIs such as `mountLinkRequestGoto()` and
`mountLinkRequestStop()`. It should not write Modbus registers directly.

## Protocol Handling

The ESP32 exposes one TCP server for both client families:

- Stellarium Desktop uses the native binary telescope protocol.
- Stellarium Mobile, SkySafari, and similar clients use LX200-style ASCII
  commands over TCP.

The first byte of a new TCP session selects the protocol:

- `0x14` means a 20-byte Stellarium Desktop binary GOTO packet;
- anything else is handled as LX200 ASCII.

The LX200 path keeps target RA/DEC separately from current position. A GOTO is
accepted after both target coordinates have been received, then forwarded to the
STM32 through the mount link queue.

## Mount Boundary

The ESP32 and STM32 communicate over Modbus RTU:

- port: `Serial2`;
- baudrate: `57600`;
- slave ID: `1`;
- RS485 direction pin: `RS485_DE_PIN`.

The register map is compact by design. ESP32-owned registers are requests;
STM32-owned registers are responses.

| Register | Name | Owner | Meaning |
| --- | --- | --- | --- |
| 0 | `REG_REQ_TARGET_RA_HIGH` | ESP32 writes | target RA high word |
| 1 | `REG_REQ_TARGET_RA_LOW` | ESP32 writes | target RA low word |
| 2 | `REG_REQ_TARGET_DEC_HIGH` | ESP32 writes | target DEC high word |
| 3 | `REG_REQ_TARGET_DEC_LOW` | ESP32 writes | target DEC low word |
| 4 | `REG_REQ_COMMAND` | ESP32 writes | requested command |
| 5 | `REG_RES_STATUS` | STM32 writes | current mount state |
| 6 | `REG_RES_CURRENT_RA_HIGH` | STM32 writes | current RA high word |
| 7 | `REG_RES_CURRENT_RA_LOW` | STM32 writes | current RA low word |
| 8 | `REG_RES_CURRENT_DEC_HIGH` | STM32 writes | current DEC high word |
| 9 | `REG_RES_CURRENT_DEC_LOW` | STM32 writes | current DEC low word |
| 10 | `REG_RES_ERROR_CODE` | STM32 writes | current error code |
| 11 | `REG_REQ_TRACKING_ENABLE` | ESP32 writes | tracking on/off intent |
| 12 | `REG_REQ_TRACKING_MODE` | ESP32 writes | lunar/sidereal/solar mode |
| 13 | `REG_REQ_MOTORS_ENABLE` | ESP32 writes | motors enable intent |
| 14 | `REG_REQ_JOG_AXIS` | ESP32 writes | RA or DEC jog axis |
| 15 | `REG_REQ_JOG_DIRECTION` | ESP32 writes | jog direction |
| 16 | `REG_REQ_JOG_SPEED` | ESP32 writes | STM32-defined jog profile |
| 17 | `REG_REQ_COMMAND_PENDING` | ESP32 sets, STM32 clears | command handshake |
| 18 | `REG_RES_STM32_FW_VERSION` | STM32 writes | packed STM32 firmware version |

Command values:

| Value | Command | Intent |
| --- | --- | --- |
| 0 | `CMD_NONE` | no command |
| 1 | `CMD_GOTO` | slew to target |
| 2 | `CMD_STOP` | priority stop |
| 3 | `CMD_SYNC` | reserved for sync compatibility |
| 4 | `CMD_FOLLOW_TARGET` | reserved for STM32 compatibility |
| 5 | `CMD_SET_TRACKING` | apply tracking enable/mode |
| 6 | `CMD_SET_MOTORS` | apply motor power intent |
| 7 | `CMD_JOG_START` | begin manual jog |
| 8 | `CMD_JOG_STOP` | stop manual jog |

Coordinate packing:

```text
RA  = hours * 15 * 3600 * 100
DEC = degrees * 3600 * 100
```

The resulting signed 32-bit values are split into high and low 16-bit words.

Important rule: a repeated command is not represented by briefly pulsing
`REG_REQ_COMMAND`. The ESP32 writes the command data and sets
`REG_REQ_COMMAND_PENDING=1`; the STM32 clears that pending flag after consuming
the command.

## STOP Semantics

STOP is not modeled as "tracking disabled". It is an explicit priority command:

1. ESP32 writes `CMD_STOP`.
2. ESP32 sets `REG_REQ_COMMAND_PENDING=1`.
3. STM32 performs its own controlled stop/deceleration.
4. ESP32 polls until the STM32 reports `IDLE` or `ERROR`.
5. The final STM32 position registers become the authoritative reached position.

This matters because tracking, GOTO, and manual jog have different mechanical
meaning even if all of them can involve motor motion.

## Display And UI

The display stack uses LovyanGFX with an SPI TFT and XPT2046-compatible touch
controller.

Current display behavior:

- boot diagnostics show hardware/service startup state;
- the main screen shows connection and mount state;
- soft buttons are arranged as a 2x3 grid;
- the center lower button rotates between soft-button pages;
- mount STOP is available from the mount page and from the moving overlay;
- night mode changes the palette from a board-level input.

Display and touch share the same SPI/LovyanGFX device, so public display access
is protected by a short mutex. Full screen operations are exposed as
`displayShow*()` functions and own the display lock.

The screen module emits `UiAction` values instead of calling services directly.
The application/display task consumes those actions and calls the relevant
service API.

## Touch Input

The touch interrupt does not read SPI and does not run UI logic. It only wakes
the touch task.

The task:

- reads touch coordinates through the display service;
- publishes debounced `Pressed` and `Released` events;
- keeps sampling while a press is active;
- lets the display/UI task decide what the event means.

This keeps interrupt work small and prevents rendering, logging, and SPI reads
from happening inside the ISR.

## WiFi, OTA, And Time

WiFi credentials are read from `lib/Telescope/secrets.h`. Real credentials
should remain local and should not be committed.

After WiFi connects:

- the firmware starts ArduinoOTA;
- NTP is used to initialize the controller clock;
- the TCP telescope server accepts clients on `STEL_PORT`.

The controller clock is owned by `Telescope`. NTP and LX200 date/time commands
update the same logical clock. A future RTC service should read and write that
same clock instead of introducing a second time model.

OTA is manual through the PlatformIO OTA environment. During OTA, the firmware
closes the active Stellarium client and pauses non-essential application work.

## Status LED

The onboard WS2812 LED reports high-level connection state:

| State | Meaning |
| --- | --- |
| red | WiFi disconnected or connecting |
| blue | WiFi connected |
| purple breathing | Stellarium client connected |
| yellow | reserved for future captive portal mode |

Brightness is intentionally low to avoid a distracting controller box.

## Hardware Target

Primary PlatformIO environment:

```ini
[env:esp32-s3-devkitc-1]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
```

Current expected peripherals:

- ESP32-S3 DevKitC-1 style board;
- onboard WS2812 on GPIO 48;
- RS485 transceiver toward STM32;
- SPI TFT, currently configured as ILI9341 bench hardware;
- XPT2046 touch controller;
- active-low night-mode input.

Display/touch pins are current bench defaults and must match the final wiring.
GPIO 4 is reserved for RS485 driver enable and should not be reused for touch
chip select.

## Configuration Points

Most project constants live in `lib/Telescope/config.h`:

- firmware identity;
- debug flags;
- optional diagnostics;
- WiFi and OTA settings;
- site coordinates;
- Stellarium TCP port;
- NTP server and offsets;
- Modbus baudrate, slave ID, pins, registers, and command IDs;
- display and touch pins;
- night-mode input;
- RGB LED pin and brightness.

PlatformIO board, build, upload, monitor, flash, PSRAM, and dependency settings
live in `platformio.ini`.

## Build And Upload

Build all environments:

```bash
pio run
```

Upload over USB:

```bash
pio run -t upload
```

Open serial monitor:

```bash
pio device monitor
```

Upload over OTA after the first USB flash:

```bash
pio run -e esp32-s3-devkitc-1-ota -t upload
```

## Dependencies

Declared in `platformio.ini`:

- `4-20ma/ModbusMaster`;
- `adafruit/Adafruit NeoPixel`;
- `lovyan03/LovyanGFX`;
- Arduino ESP32 WiFi, OTA, time, and FreeRTOS facilities.

## Development Notes

The most important next cleanup is not visual polish; it is hardening the
interfaces:

- make LX200 parsing stricter and testable;
- use mount snapshots for client-facing position reads;
- keep UI state owned by the display task;
- check RTOS resource creation failures;
- verify board flash/PSRAM settings against real hardware;
- add host-side tests for protocol parsing and coordinate conversion.

The current review notes are in `CODE_REVIEW.md`.
