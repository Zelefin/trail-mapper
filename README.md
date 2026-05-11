# trail mapper

## About

Trail mapper is an ESP32-based GPS logger for walking trails and later exporting
the recorded path into map tools.

## Trail mapper usage

- Go into a forest, park, or other walking area.
- Turn on the device.
- Wait for the ready screen.
- Press the record button once to start logging.
- Walk.
- Press the record button again to stop and save.
- Wait for the saved indication before switching the device off.
- Copy the data from the microSD card to a PC.
- Parse the data and create a map of the trails.

## How it works

When you turn on the device it initializes the peripherals and waits without
recording. Pressing the record button arms the logger. If GPS already has a fix,
the device starts writing valid coordinates to the microSD card. If GPS has no
fix yet, it waits and starts automatically after the first valid fix.

The logs are later parsed into formats usable by Google Maps, OpenStreetMap, or
other mapping tools.

## Current firmware status

The breadboard firmware has been tested with GPS, microSD, TFT display, LEDs,
record button, buzzer, and battery power.

On each recording session the firmware creates:

- `trackNNN.csv`: valid GPS fixes in CSV format
- `trackNNN.nmea`: raw NMEA sentences from the GPS module
- `blackbox.log`: append-only diagnostic events for field-test debugging

The blackbox log is useful when something goes wrong away from USB serial. It
records session start/stop, created file names, first fix, progress counters,
dropped GPS lines, and errors that happen after the SD card mounts.

## Battery power notes

For standalone testing, use a regulated 5V rail from the battery/boost converter:

- 5V rail to ESP32 `5V`/`VIN`
- 5V rail to the SD card module `VCC`
- common GND shared by ESP32, SD, GPS, TFT, LEDs, button, and buzzer
- TFT powered from 3.3V

Avoid connecting USB and the external 5V rail at the same time unless the exact
ESP32 dev board power path has been verified.

## Status indicators

- red LED: error
- yellow LED: waiting, armed, or GPS signal lost
- green LED: recording
- ST7735S TFT: current state, satellites, HDOP, and recent coordinates
- KY-012 active buzzer: short beeps on state changes and errors

## List of components

- TFT display 0.96" SPI 160x80 IPS (RGB), ST7735S driver
- momentary record button, DIP 4-pin
- 1P2T switch button for hardware power
- NEO-6M GPS module GY-NEO6MV2
- 3 LED (red, yellow, green)
- KY-012 active buzzer
- Charge module TP4056 Type-C with battery protection
- Adjustable Boost Converter 2A 28V MT3608
- Battery Li-Po 1000 mAh 3.7V
- microSD card module
- ESP32-WROOM (chip ESP32, 4MB flash)

## Breadboard prototype

The first integrated prototype is assembled on a breadboard and can be powered
from USB for bench work or from a regulated battery 5V rail for field tests. See
[docs/breadboard-prototype.md](docs/breadboard-prototype.md) for the proposed
pin map, state model, logging format, and manual acceptance checklist. See
[docs/breadboard-wiring.md](docs/breadboard-wiring.md) for the wiring guide.
