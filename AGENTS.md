# AGENTS.md

Guidance for AI agents working in this repository.

## Project Context

`trail-mapper` is an ESP-IDF project for an ESP32-WROOM based handheld trail logger.
The intended device records GPS coordinates while walking, stores logs locally, and
later exports/parses them into formats usable by map tools such as Google Maps or
OpenStreetMap.

The current hardware integration plan lives in
`docs/breadboard-prototype.md`. Read that document before assigning pins,
changing peripheral behavior, or flashing hardware-facing firmware.

Hardware noted so far:

- ESP32-WROOM module, ESP32 chip, 4MB flash
- NEO-6M GPS module GY-NEO6MV2
- 0.96 inch SPI TFT display, 160x80 IPS RGB
- microSD card module
- momentary record button
- 1P2T power switch
- 3 LEDs: red, yellow, green
- KY-012 active buzzer
- TP4056 Type-C Li-Po charger/protection module
- MT3608 boost converter
- 1000 mAh 3.7V Li-Po battery

## ESP-IDF Environment

The developer normally activates ESP-IDF with this shell alias:

```sh
activate-esp
```

The alias expands to:

```sh
source /home/heorhii/.espressif/tools/activate_idf_v6.0.sh
```

In Codex/non-interactive shells, `idf.py` may not be available as a plain command
because the activation script defines it as a shell alias. Prefer using the real
script path:

```sh
$IDF_PATH/tools/idf.py --version
```

Known working environment:

- ESP-IDF: `v6.0`
- IDF path: `/home/heorhii/.espressif/v6.0/esp-idf`
- Python venv: `/home/heorhii/.espressif/tools/python/v6.0/venv`
- Target: `esp32`

If direct esptool access is needed, use:

```sh
$IDF_PYTHON_ENV_PATH/bin/python -m esptool ...
```

## Board / Serial Facts

The development board has been verified on:

```sh
/dev/ttyUSB0
```

Kernel detected the USB-UART bridge as:

```text
Silicon Labs CP2102 USB to UART Bridge Controller
```

The user is in the `uucp` group and `/dev/ttyUSB0` was usable without extra
permission changes.

Verified ESP32 details:

```text
Chip: ESP32-D0WD-V3, revision v3.1
Features: Wi-Fi, BT, dual core, 240MHz
Crystal: 40MHz
MAC: 1c:c3:ab:c1:15:30
Flash: 4MB
Flash voltage: 3.3V
```

The project `sdkconfig` should use 4MB flash:

```text
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
```

Generated flash args should include:

```text
--flash-size 4MB
```

## Common Commands

Build:

```sh
$IDF_PATH/tools/idf.py build
```

Flash:

```sh
$IDF_PATH/tools/idf.py -p /dev/ttyUSB0 flash
```

Size report:

```sh
$IDF_PATH/tools/idf.py size
```

Probe chip:

```sh
$IDF_PYTHON_ENV_PATH/bin/python -m esptool --port /dev/ttyUSB0 chip-id
```

Probe flash:

```sh
$IDF_PYTHON_ENV_PATH/bin/python -m esptool --port /dev/ttyUSB0 flash-id
```

Serial monitor via `idf.py monitor` may fail in non-interactive Codex shells
because it requires stdin attached to a TTY. For quick log capture, use direct
serial reading:

```sh
stty -F /dev/ttyUSB0 115200 raw -echo
timeout 8s cat /dev/ttyUSB0
```

Expected monitor baud:

```text
115200
```

## Current Baseline App

The current basic firmware logs a heartbeat once per second:

```text
I (...) trail-mapper: Hello, World! heartbeat=N
```

This has been built, flashed, and observed over `/dev/ttyUSB0`.

## Testing And Flashing Expectations

Before flashing firmware, provide appropriate test coverage for the change.

Minimum expectation:

- Run `idf.py build` before every flash.
- For pure build/config changes, a successful build is usually enough.
- For logic changes, add or update tests where practical before flashing.
- If hardware behavior is changed, also describe the manual hardware check that
  will verify it after flashing.
- Do not flash code that does not build cleanly.

Prefer small, testable modules for parser/state-machine/device logic so future
host-side or ESP-IDF unit tests can cover behavior without always requiring the
board.

## Git / Generated Files

Do not commit generated build output.

Ignored project outputs include:

```gitignore
build/
compile_commands.json
sdkconfig.old
managed_components/
```

Commit `sdkconfig` when project configuration changes intentionally, especially
target, flash size, partition table, driver/component settings, or logging options.

`.clangd` is optional. Commit it only if the editor configuration should be shared.

## Working Style

- Read the existing code and config before changing behavior.
- Keep changes scoped and consistent with ESP-IDF project structure.
- Prefer ESP-IDF APIs and components over custom low-level code unless there is a
  clear reason.
- When touching hardware-facing code, state the assumed pins/peripherals and avoid
  hard-coding pin choices without confirming the wiring plan.
- Preserve user changes in the worktree; do not revert unrelated files.
