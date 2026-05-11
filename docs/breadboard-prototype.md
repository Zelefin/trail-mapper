# Breadboard Prototype Plan

This document describes the first integrated breadboard build for `trail-mapper`.
The goal is to test the device as it should behave in the field, before moving
the circuit to a perfboard.

For step-by-step wiring, see [breadboard-wiring.md](breadboard-wiring.md).

## Goal

Build a breadboard MVP that can:

- power on from the MB102 breadboard power supply module
- wait in an idle state without recording
- use one momentary button to start and stop a recording session
- read GPS data from the NEO-6M module
- write valid GPS fixes to a CSV file on the microSD card
- show state and basic GPS details on the ST7735S TFT display
- show state with red/yellow/green LEDs
- beep on important state changes with the KY-012 active buzzer

The hardware power switch is handled outside firmware for this prototype. Stop
recording first, wait for the saved indication, then switch power off.

## Hardware

Main modules:

- ESP32-WROOM dev board, ESP32-D0WD-V3, 4MB flash
- MB102 breadboard power supply module
- NEO-6M GPS module GY-NEO6MV2
- 0.96 inch 160x80 SPI TFT display, ST7735S controller
- microSD card SPI module
- one momentary record button
- one 1P2T power switch
- red, yellow, and green status LEDs
- KY-012 active buzzer

Power plan:

- MB102 powers the breadboard prototype.
- MB102 `5V` powers the ESP32 dev board through `5V`/`VIN`.
- MB102 `3.3V` powers logic-level peripherals where appropriate.
- All grounds must be common: MB102, ESP32, GPS, TFT, SD, LEDs, button, buzzer.
- Do not connect any signal line to a peripheral unless the peripheral ground is
  also connected to ESP32 ground.

## Proposed Pin Map

The pin map avoids ESP32 boot-strapping pins where practical and keeps GPS on the
known-working UART pins from the earlier test project.

| Function | ESP32 pin | Notes |
| --- | --- | --- |
| GPS UART | UART2 | NEO-6M default 9600 baud |
| GPS TX from ESP32 | GPIO26 | ESP32 TX -> GPS RX |
| GPS RX into ESP32 | GPIO25 | ESP32 RX <- GPS TX |
| SPI SCLK | GPIO18 | Shared by TFT and SD |
| SPI MOSI | GPIO23 | Shared by TFT and SD |
| SPI MISO | GPIO19 | SD uses MISO; TFT may not |
| SD CS | GPIO22 | SD card chip select |
| TFT CS | GPIO27 | ST7735S chip select |
| TFT DC | GPIO16 | Data/command |
| TFT RST | GPIO17 | Reset |
| TFT BL | 3.3V | Backlight always on for MVP |
| Record button | GPIO32 | Active-low, internal pull-up |
| Buzzer | GPIO33 | KY-012 active buzzer |
| Red LED | GPIO13 | Error |
| Yellow LED | GPIO14 | Waiting/armed/GPS lost |
| Green LED | GPIO21 | Recording |

Use current-limiting resistors for the LEDs. Start with 220 ohm to 1 kohm.

## State Model

| State | Meaning | LED | Display | Buzzer |
| --- | --- | --- | --- | --- |
| `IDLE` | Powered on, not recording | Yellow on | Ready, no active session | None |
| `ARMED_WAIT_FIX` | Record pressed, waiting for valid GPS fix | Yellow on/blink | Waiting for GPS fix, satellites/HDOP if available | One short beep on entry |
| `RECORDING` | GPS fix valid and CSV file open | Green on | Recording, satellites, last coordinates, filename | Two short beeps on entry |
| `GPS_LOST` | Session open, GPS fix lost | Yellow on/blink | GPS lost, session still open | One medium beep on transition |
| `ERROR` | SD/init/write or unrecoverable peripheral error | Red on | Error summary | Three short beeps |
| `STOPPED_SAVED` | User stopped recording and file is closed | Green off, yellow on | Saved, then ready | Two short beeps |

The record button behavior:

- In `IDLE`, press once to arm recording.
- If GPS already has a valid fix, transition to `RECORDING`.
- If GPS has no valid fix, transition to `ARMED_WAIT_FIX`.
- In `ARMED_WAIT_FIX`, recording starts automatically when the first valid fix
  arrives.
- In `RECORDING` or `GPS_LOST`, press once to flush and close the CSV file.
- After saving, return to `IDLE`.

## GPS Input

Use the GPS wiring and parsing approach already proven in
`/home/heorhii/projects/test_wroom`:

- UART: `UART_NUM_2`
- Baud: `9600`
- ESP32 TX: `GPIO26`
- ESP32 RX: `GPIO25`
- Parse `$GPGGA` and `$GNGGA`

Minimum parsed fields:

- UTC time
- latitude
- longitude
- fix quality
- satellite count
- HDOP

Coordinates should be converted from NMEA degrees/minutes into signed decimal
degrees. `S` and `W` hemispheres are negative.

## SD Logging

CSV is the MVP log format.

Create one file per recording session after the first valid GPS fix. Name the
file from GPS UTC date/time when available. GGA provides UTC time but not date,
so the MVP may use a safe fallback such as `SESSION01.CSV`. Date-based filenames
can be added later by parsing RMC sentences.

CSV header:

```csv
utc_date,utc_time,latitude,longitude,satellites,hdop,fix_quality
```

Example row:

```csv
2026-05-10,12:34:56,49.233123,28.468456,8,1.10,1
```

Logging rules:

- Write one row for each valid GPS fix.
- Do not write coordinate rows while the fix quality is `0`.
- Flush periodically and always flush/close on stop.
- If a write or flush fails, transition to `ERROR`.

Blackbox diagnostics:

- Append important recording events to `/sdcard/blackbox.log`.
- Keep the log human-readable so it can be copied from the SD card and shared
  after a failed field test.
- Include at least session start/stop, created file names, first fix, periodic
  counters, dropped GPS lines, and any SD/GPS errors that happen after the SD
  card has mounted.
- Serial monitor output is useful during bench testing, but the SD blackbox is
  the source of truth for field-test diagnostics because it survives resets and
  disconnected USB.

## Display

Use the ST7735S TFT as a simple status screen for the MVP. Do not introduce LVGL
yet unless the basic ESP-IDF display path becomes impractical.

Recommended dependency:

```text
waveshare/esp_lcd_st7735==1.0.1
```

The MVP screen should show:

- current state
- satellites
- HDOP
- last latitude/longitude if available
- current file/session name while recording
- short error text in `ERROR`

## Testing Before Flashing

Before flashing any firmware change:

- run unit tests for changed pure logic, where present
- run `$IDF_PATH/tools/idf.py build`
- do not flash if the build or tests fail

Firmware should be split so logic is testable without hardware:

- NMEA parser
- state machine
- CSV formatter/session naming

Planned tests:

- valid GGA line converts latitude/longitude correctly
- no-fix GGA does not produce a loggable coordinate
- malformed or short GGA is rejected without crashing
- `S` and `W` coordinates become negative
- pressing record in `IDLE` enters armed/waiting or recording
- first valid fix in `ARMED_WAIT_FIX` starts recording
- lost fix during recording enters `GPS_LOST`
- pressing record while recording closes the session
- SD write error enters `ERROR`
- CSV header and rows match the documented format

## Manual Breadboard Acceptance

The breadboard MVP is accepted when these checks pass:

- ESP32 boots from MB102 power with all grounds common.
- Device starts in `IDLE`.
- Pressing the record button before GPS fix enters `ARMED_WAIT_FIX`.
- When GPS fix arrives, green LED turns on and CSV logging starts.
- The display shows recording state, satellites, HDOP, and last coordinates.
- The SD card contains a readable CSV file with plausible coordinates.
- Losing GPS fix changes status without corrupting the open file.
- Pressing record again closes the CSV file and returns to `IDLE`.
- After saved indication, power can be switched off without losing the file.
