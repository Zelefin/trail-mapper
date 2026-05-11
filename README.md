# trail mapper

## About

Trail mapper is an ESP32-based GPS logger for walking trails and later exporting
the recorded path into map tools.

## Trail mapper usage

- go into forest/park
- turn on the device
- wait for GPS readiness
- press the record button to start logging
- walk
- after finishing walking, press the record button again to stop and save
- switch the device off after the saved indication
- come home and upload data to your PC
- parse data and create your own map of the trails

## How it works

When you turn on the device it initializes the peripherals and waits without
recording. Pressing the record button arms the logger. If GPS already has a fix,
the device starts writing valid coordinates to the microSD card. If GPS has no
fix yet, it waits and starts automatically after the first valid fix.

The logs are later parsed into formats usable by Google Maps, OpenStreetMap, or
other mapping tools.

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

The first integrated prototype will be assembled on a breadboard and powered by
an MB102 breadboard power supply module. See
[docs/breadboard-prototype.md](docs/breadboard-prototype.md) for the proposed
pin map, state model, logging format, and manual acceptance checklist. See
[docs/breadboard-wiring.md](docs/breadboard-wiring.md) for the wiring guide.
