# ESP32 SD READER

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-6.0.1-red)](https://docs.espressif.com/projects/esp-idf/)
[![Target](https://img.shields.io/badge/Target-ESP32--S3-blue)](https://www.espressif.com/en/products/socs/esp32-s3)
[![Language](https://img.shields.io/badge/Language-C-green)](https://en.wikipedia.org/wiki/C_(programming_language))

![Device Case](stl/default.png)

## Example Output
```bash
═══════════════════════════════════════════════
 ESP32 SD READER v2.1 by @minitwiks
═══════════════════════════════════════════════

Model           : SD16G
Manufacturer ID : 0x03
Manufacturer    : SanDisk / Western Digital
OEM ID          : 0x5344
Revision        : 128
Serial          : 0x12345678
Manufactured    : 05/2024
Full CID        : XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX

Filesystem      : FAT/exFAT
Capacity        : 14.84 GB
Used            : 2.10 GB
Free            : 12.74 GB
Usage           :  14% |====--------------------------|

Bus             : SDMMC 1-bit
Mount point     : /sdcard

═══════════════════════════════════════════════
```

An ESP32-S3 project that detects a microSD/SD card, mounts its FAT/exFAT filesystem, and prints technical card information to the serial monitor: CID, manufacturer, serial number, manufacturing date, total capacity, used space, and free space.

## Features

- Automatic card insert and removal detection.
- CID reading with full CID hex string generation.
- Known manufacturer lookup by Manufacturer ID.
- Capacity, used space, and free space reporting.
- FAT/exFAT support through ESP-IDF FatFs.
- Status indication with a WS2812 addressable RGB LED.
- SDMMC 1-bit bus mode.

## 3D Printed Enclosure Models

This project also includes 3D-printable enclosure models for the device. You can find the STL files and a short description in the [stl](stl) folder.

## Hardware

The project uses SDMMC in 1-bit mode and a dedicated card detect pin.

| Signal | ESP32-S3 GPIO |
| --- | ---: |
| SD CLK | GPIO 12 |
| SD CMD | GPIO 11 |
| SD D0 | GPIO 10 |
| Card Detect | GPIO 9 |
| WS2812 RGB LED | GPIO 21 |

Card Detect is active-low: when a card is inserted, GPIO 9 is expected to read `LOW`.

## LED Status

| Color | State |
| --- | --- |
| Blue | Device startup |
| Orange | Card is not inserted or was removed |
| Yellow | Card detected, mounting in progress |
| Green | Card mounted successfully |
| Red | Mount failed |

## Requirements

- ESP32-S3-based board.
- microSD/SD module wired through SDMMC.
- WS2812 addressable RGB LED, if status indication is needed.
- ESP-IDF compatible with this project.

## Quick Start

Activate the ESP-IDF environment:

```bash
export IDF_PATH="$HOME/.espressif/v6.0.1/esp-idf"
. $IDF_PATH/export.sh
```

Set the target chip:

```bash
idf.py set-target esp32s3
```

Build the project:

```bash
idf.py build
```

Flash the board and open the serial monitor:

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

On macOS, the serial port may look like this:

```bash
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

To exit the monitor:

```text
Ctrl + ]
```

## Usage

1. Wire the SD module to the ESP32-S3 according to the pin table.
2. Build and flash the project.
3. Open the serial monitor.
4. Insert an SD card.
5. Read the card information printed in the serial monitor.
6. When the card is removed, the project unmounts the filesystem and waits for the next card.

## Board Pin Customization

If your board uses different pins, update these values in `main/sd_cid_reader.c`:

```c
#define PIN_SD_CLK      12
#define PIN_SD_CMD      11
#define PIN_SD_D0       10
#define PIN_CARD_DETECT 9
#define PIN_RGB_LED     21
```

After changing the pins, rebuild and flash the project again:

```bash
idf.py build flash monitor
```

## Troubleshooting

### Card Is Not Detected

- Check `PIN_CARD_DETECT`.
- Make sure card detect is actually pulled `LOW` when a card is inserted.
- Check the SD module power supply.

### Mount Failed

- Check the filesystem format: the project expects FAT/exFAT.
- Check the `CLK`, `CMD`, and `D0` lines.
- Make sure the card is healthy and readable on a computer.

### RGB LED Does Not Light Up

- Check that the WS2812 data line is connected to GPIO 21.
- Check LED power and common GND with the ESP32-S3.
- If the LED is not needed, the firmware can still run, but there will be no visual status indication.
