# Breadboard Wiring Guide

This guide is for the first `trail-mapper` breadboard assembly on an 830-point
breadboard with an MB102 breadboard power supply module.

## Power Safety First

Use one of these power modes at a time.

### Mode A: USB debug mode

Use this mode while flashing firmware or watching serial logs.

- ESP32 is powered from laptop USB.
- MB102 powers only the breadboard peripheral rails.
- Connect ESP32 `GND` to MB102/breadboard `GND`.
- Do not connect MB102 `5V` to ESP32 `5V/VIN` while USB is connected.
- Do not connect ESP32 `3V3` to MB102 `3.3V`.

### Mode B: Standalone breadboard mode

Use this mode when the laptop USB is unplugged.

- MB102 `5V` can power ESP32 `5V/VIN`.
- MB102 `3.3V` powers 3.3V peripherals.
- All grounds remain common.
- Laptop USB should be unplugged.

## MB102 And 830-Point Breadboard Notes

Many 830-point breadboards have split power rails. The red/blue rails may be
broken in the middle even if the printed line looks continuous.

Before connecting modules:

- Put MB102 on one end of the breadboard.
- Set one rail to `3.3V` for logic peripherals.
- Set the other rail to `5V` only if you need standalone ESP32 `VIN/5V`.
- Use jumper wires to bridge left/right rail halves if your breadboard rails are
  split.
- Verify rail voltage with a multimeter if available.

Suggested rail layout:

```text
top red rail:    3.3V peripherals
top blue rail:   GND
bottom red rail: 5V for standalone ESP32 VIN only
bottom blue rail:GND
```

Always connect all blue/ground rails together.

## Wiring Checklist

Wire and test in this order:

1. Power rails and ground.
2. ESP32 ground to breadboard ground.
3. GPS.
4. LEDs, record button, buzzer.
5. SD card module.
6. TFT display.

## GPS: NEO-6M

| GPS pin | Connect to |
| --- | --- |
| VCC | 3.3V or 5V according to your module marking |
| GND | GND |
| TX | ESP32 GPIO25 |
| RX | ESP32 GPIO26 |

The known-working firmware setup uses UART2 at 9600 baud:

```text
ESP32 GPIO26 -> GPS RX
ESP32 GPIO25 <- GPS TX
```

## Status LEDs

Use a resistor in series with each LED, typically 220 ohm to 1 kohm.

Simple wiring:

```text
ESP32 GPIO -> resistor -> LED anode/long leg
LED cathode/short leg -> GND
```

| LED | ESP32 GPIO |
| --- | --- |
| Red error LED | GPIO13 |
| Yellow waiting LED | GPIO14 |
| Green recording LED | GPIO21 |

## Record Button

Use the momentary 4-pin button as an active-low input.

| Button side | Connect to |
| --- | --- |
| One side | ESP32 GPIO32 |
| Opposite side | GND |

Firmware will use the internal pull-up, so no external resistor is needed for
the MVP.

For a typical 4-pin tactile button, pins on the same side are already connected
together. Place the button across the breadboard center gap so pressing it
connects GPIO32 to GND.

## KY-012 Active Buzzer

| KY-012 pin | Connect to |
| --- | --- |
| `S` or signal | ESP32 GPIO33 |
| `+` | 3.3V |
| `-` | GND |

KY-012 is active, so GPIO33 only turns it on/off. It does not need PWM for basic
beeps.

## Shared SPI Bus

The TFT and SD card share SPI clock and MOSI. SD also needs MISO. Each device
gets its own chip-select pin.

Shared SPI pins:

| Signal | ESP32 GPIO |
| --- | --- |
| SCLK / SCK / CLK | GPIO18 |
| MOSI / DIN / SDA | GPIO23 |
| MISO / DO | GPIO19 |

## microSD SPI Module

| SD module pin | Connect to |
| --- | --- |
| VCC | 3.3V unless module explicitly requires 5V |
| GND | GND |
| SCK / CLK | GPIO18 |
| MOSI / DI | GPIO23 |
| MISO / DO | GPIO19 |
| CS | GPIO22 |

If the SD module has both `3.3V` and `5V` markings, prefer the documented module
input. Many microSD breakout boards include level shifting and expect 5V, but
bare/simple modules often require 3.3V only.

## ST7735S 0.96-Inch TFT

| TFT pin | Connect to |
| --- | --- |
| VCC | 3.3V |
| GND | GND |
| SCL / SCK / CLK | GPIO18 |
| SDA / MOSI / DIN | GPIO23 |
| CS | GPIO27 |
| DC / A0 | GPIO16 |
| RES / RST | GPIO17 |
| BL / LED | 3.3V |

Most small ST7735S displays do not use MISO.

## Text Scheme

```text
                 830-point breadboard

  3.3V rail  ===================================================== peripherals VCC
  GND rail   ===================================================== all GND

                    ESP32 dev board
             +--------------------------+
 GPS RX  <---| GPIO26              5V/VIN|--- MB102 5V only in standalone mode
 GPS TX  --->| GPIO25                 GND|--- GND rail
 SD CS   <---| GPIO22                GPIO18 ---> SPI SCLK: SD + TFT
 TFT CS  <---| GPIO27                GPIO23 ---> SPI MOSI: SD + TFT
 TFT DC  <---| GPIO16                GPIO19 <--- SPI MISO: SD only
 TFT RST <---| GPIO17                GPIO32 <--- record button to GND
 RED LED <---| GPIO13                GPIO33 ---> KY-012 signal
 YEL LED <---| GPIO14                GPIO21 ---> GREEN LED
             +--------------------------+

  GPS: VCC, GND, TX->GPIO25, RX->GPIO26
  SD:  VCC, GND, SCK->18, MOSI->23, MISO->19, CS->22
  TFT: VCC, GND, SCK->18, MOSI->23, CS->27, DC->16, RST->17, BL->3.3V
```

## First Power-On Check

Before connecting USB or MB102 power:

- Inspect for crossed `5V` and `3.3V` rails.
- Confirm no GPIO is connected directly to `5V`.
- Confirm every LED has a resistor.
- Confirm all grounds are common.
- Confirm SD and TFT chip-select wires are not swapped.

Power up with only ESP32 + GPS first if possible. Add SD/TFT after the first
basic GPS and button/LED checks pass.

