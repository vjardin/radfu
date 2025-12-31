# RADFU - Renesas RA Device Firmware Update

[![CI](https://github.com/vjardin/radfu/actions/workflows/ci.yml/badge.svg)](https://github.com/vjardin/radfu/actions/workflows/ci.yml)

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

## Installation

```sh
curl -fsSL https://raw.githubusercontent.com/vjardin/radfu/master/install.sh | sh
```

Or install system-wide (requires root):

```sh
curl -fsSL https://raw.githubusercontent.com/vjardin/radfu/master/install.sh | sudo sh
```

Or install a specific version:

```sh
curl -fsSL https://raw.githubusercontent.com/vjardin/radfu/master/install.sh | sh -s -- --version v0.0.1
```

## Building from Source

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
  osis                 Detect ID code protection status

Options:
  -p, --port <dev>     Serial port (auto-detect if omitted)
  -a, --address <hex>  Start address (default: 0x0)
  -s, --size <hex>     Size in bytes
  -b, --baudrate <n>   Set UART baud rate (default: 9600)
  -i, --id <hex>       ID code for authentication (32 hex chars)
  -e, --erase-all      Erase all areas using ALeRASE magic ID
  -v, --verify         Verify after write
  -U, --usb-reset      USB reset before connecting (Linux only)
  -h, --help           Show this help message
  -V, --version        Show version

Examples:
  radfu info
  radfu osis
  radfu read -a 0x0 -s 0x10000 firmware.bin
  radfu write -b 1000000 -a 0x0 -v firmware.bin
  radfu erase -a 0x0 -s 0x10000
```

## Supported Baud Rates

When using UART (not USB), the following baud rates are supported:

- RA4 Series (24 MHz SCI): 9600, 115200, 230400, 460800, 921600, 1000000, 1500000
- RA6 Series (60 MHz SCI): 9600 to 4000000 (including 2000000, 3000000, 3500000)

Note: USB communication is not affected by baud rate settings.

## ID Code Protection (OSIS)

Renesas RA MCUs have a 128-bit OCD/Serial Programmer ID Setting Register (OSIS) that controls
flash protection. The `radfu osis` command detects whether the device is accessible.

Note: The OSIS register cannot be read or written via the bootloader. Use J-Link or Renesas
Flash Programmer to configure ID code protection.

### Protection Status

A factory-fresh chip has all 0xFF in OSIS, meaning no protection is enabled. The `radfu osis`
command reports whether the device is accessible:

- **Unlocked**: Device responds without authentication (factory default)
- **Locked**: Connection fails unless correct ID is provided via `-i` option

### ALeRASE Total Area Erasure

The `--erase-all` option sends the magic ALeRASE ID code which triggers a complete erasure
of all flash areas, including the ID code itself. This only works on devices configured to
allow ALeRASE (OSIS[127:126] = 10b).

Important: If ALeRASE is disabled, the device cannot be recovered without the correct ID code.

## Supported Platforms

- Linux (tested)
- macOS (supported)
- Windows (not yet supported)

## Supported Devices

- RA4M2 - Cortex-M33 (tested on EK-RA4M2, boot code 0xC6)
- RA4 Series - Cortex-M4/M23 (should work, boot code 0xC3)
- RA2 Series - Cortex-M23 (should work, boot code 0xC3)
- RA6 Series - Cortex-M33 (should work, boot code 0xC6)

## DFU Mode on EK-RA4M2

The RA4M2 has a factory-programmed ROM bootloader that supports USB DFU programming,
independent of any user application.

### Hardware Configuration

The **MD pin (P201)** controls the boot mode. On the EK-RA4M2 board, **jumper J16**
controls this pin:

| J16 State | Mode | Description |
|-----------|------|-------------|
| Open (default) | Single-Chip Mode | Normal operation, runs user code |
| Jumper installed | SCI/USB Boot Mode | ROM bootloader active |

### Entering DFU Mode

1. Install jumper on J16 (shorts P201/MD to GND)
2. Connect USB cable to Device USB port (J11) - not the Debug USB
3. Press RESET button
4. MCU enters ROM bootloader

### Programming with radfu

```sh
# Get device info
radfu info

# Erase flash
radfu erase

# Write firmware at address 0x0 with verification
radfu write -a 0x0 -v firmware.bin

# Read flash to file
radfu read -a 0x0 -s 0x80000 dump.bin
```

### Returning to Normal Mode

1. Remove jumper from J16
2. Press RESET button
3. MCU runs your application

### Linux USB Permissions

If the device is not detected, create a udev rule:

```sh
sudo tee /etc/udev/rules.d/99-renesas-dfu.rules << 'EOF'
# Renesas RA USB Boot Mode
SUBSYSTEM=="usb", ATTR{idVendor}=="045b", MODE="0666"
EOF
sudo udevadm control --reload-rules
```

Check device detection:

```sh
lsusb | grep -i renesas
ls /dev/ttyACM*
```

### USB Reset for Reconnection

The bootloader only accepts connections immediately after a hardware reset. To reconnect
without unplugging the USB cable, use the --usb-reset option which performs a USB bus
reset and prompts for the RESET button press:

```sh
radfu --usb-reset info
```

This requires write access to sysfs. Grant the CAP_DAC_OVERRIDE capability:

```sh
sudo setcap cap_dac_override+ep $(which radfu)
```

### Board Documentation

- [EK-RA4M2 v1 User's Manual](https://www.renesas.com/en/document/man/ek-ra4m2-v1-users-manual) - See page 28 for J16 jumper details

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

## Legal Notice

RADFU is implemented using clean-room principles to achieve interoperability with Renesas RA
microcontrollers. Any reverse engineering involved was limited to what is strictly necessary for
interoperability and is compliant with EU Directive 2009/24/EC and Article L122-6-1 of the French
Intellectual Property Code.

No proprietary source code or confidential material is included. See LEGAL.md for details.

## License

AGPL-3.0-or-later

Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
