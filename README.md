# RADFU - Renesas RA Device Firmware Update

A flash programming tool for Renesas RA microcontroller series. Communicates with the
built-in ROM bootloader to perform firmware update operations via USB or UART/SCI interfaces.

## Features

- Read flash memory contents to file
- Write firmware images to flash memory
- Erase flash sectors
- Query device information (MCU type, firmware version, memory areas)
- ID authentication for protected devices
- Auto-detect Renesas USB devices
- Display USB device information (vendor, product, serial number)
- Configurable UART baud rate (9600 to 4000000 bps)
- Verify after write option
- Descriptive MCU error reporting with error codes and messages
- Progress callback API for library integration
- Minimal footprint - pure C with no external dependencies

## Protocol Support

Implements the Renesas RA Standard Boot Firmware protocol:

| Command       | Code | Status      |
|---------------|------|-------------|
| INQ (Inquire) | 0x00 | Implemented |
| ERA (Erase)   | 0x12 | Implemented |
| WRI (Write)   | 0x13 | Implemented |
| REA (Read)    | 0x15 | Implemented |
| BAU (Baudrate)| 0x34 | Implemented |
| SIG (Signature)| 0x3A| Implemented |
| ARE (Area)    | 0x3B | Implemented |
| IDA (ID Auth) | 0x30 | Implemented |

## Building

Requires: meson, ninja, and optionally cmocka for tests.

```sh
meson setup build
meson compile -C build
```

To run tests:

```sh
meson test -C build
```

## Usage

```
Usage: radfu <command> [options] [file]

Commands:
  info                 Show device and memory information
  read  <file>         Read flash memory to file
  write <file>         Write file to flash memory
  erase                Erase flash sectors

Options:
  -p, --port <dev>     Serial port (auto-detect if omitted)
  -a, --address <hex>  Start address (default: 0x0)
  -s, --size <hex>     Size in bytes
  -b, --baudrate <n>   Set UART baud rate (default: 9600)
  -i, --id <hex>       ID code for authentication (32 hex chars)
  -e, --erase-all      Erase all areas using ALeRASE magic ID
  -v, --verify         Verify after write
  -h, --help           Show this help message
  -V, --version        Show version

Examples:
  radfu info
  radfu read -a 0x0 -s 0x10000 firmware.bin
  radfu write -b 1000000 -a 0x0 -v firmware.bin
  radfu erase -a 0x0 -s 0x10000
```

## Supported Baud Rates

When using UART (not USB), the following baud rates are supported:

- RA4 Series (24 MHz SCI): 9600, 115200, 230400, 460800, 921600, 1000000, 1500000
- RA6 Series (60 MHz SCI): 9600 to 4000000 (including 2000000, 3000000, 3500000)

Note: USB communication is not affected by baud rate settings.

## Supported Platforms

- Linux (tested)
- Windows (not yet supported)
- macOS (not yet supported)

## Supported Devices

- RA4 Series - Cortex-M4/M23 (tested)
- RA2 Series - Cortex-M23 (should work)
- RA6 Series - Cortex-M33 (should work, accepts 0xC6 boot code)

## Documentation Sources

This implementation is based on the official Renesas documentation:

- [R01AN5372 - Renesas RA Family System Specifications for Standard Boot Firmware](https://www.renesas.com/en/document/apn/renesas-ra-family-system-specifications-standard-boot-firmware)

- [R01AN6347 - Renesas RA Family System Specifications (for newer devices)](https://www.renesas.com/en/document/apn/renesas-ra-family-system-specifications-standard-boot-firmware-0)

- [Standard Boot Firmware for RA family MCUs Based on Arm Cortex-M33](https://www.renesas.com/en/document/apn/standard-boot-firmware-ra-family-mcus-based-arm-cortex-m33)

## Acknowledgments

This project is inspired by [raflash](https://github.com/robinkrens/raflash), the original
Python implementation by Robin Krens. Many thanks for the excellent work on reverse-engineering
the Renesas RA bootloader protocol and providing a working reference implementation.

RADFU is a C rewrite aimed at providing a minimal footprint suitable for embedded environments,
build systems, and resource-constrained scenarios where a Python runtime may not be available
or practical.

## License

AGPL-3.0-or-later

Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
