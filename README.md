trail mapper

# About

## Trail mapper usage:

- go into forest/park
- turn on the device
- walk
- after finishing walking stop the device
- come home and upload data to your PC
- parse data and create your own map of the trails

## How it works

When you turn on the device it should connect to the GPS and log and store the coordinates with some interval. Then those logs go into parser that converts the data into readable format for google maps, openstreetmaps etc. In the end you will see your own map of the trails that you've logged.


# List of components:

- TFT display 0.96" SPI 160x80 IPS (RGB) 
- 2 buttons DIP 4-pin
- NEO-6M GPS module GY-NEO6MV2
- 3 LED (red, yellow, green)
- Charge module TP4056 Type-C with battery protection
- Adjustable Boost Converter 2A 28V MT3608
- Battery Li-Po 1000 mAh 3.7V
- microSD card module
- ESP32-wroom (chip ESP32, 4mb flash)
