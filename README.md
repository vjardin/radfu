# RADFU - Renesas RA Device Firmware Update

[![CI](https://github.com/vjardin/radfu/actions/workflows/ci.yml/badge.svg)](https://github.com/vjardin/radfu/actions/workflows/ci.yml)

A flash programming tool for Renesas RA microcontroller series. Communicates with the
built-in ROM bootloader to perform firmware update operations via USB or UART/SCI interfaces.

## Features

- Read/write/erase flash memory
- Query device information (MCU type, firmware version, memory areas)
- Device Lifecycle Management (DLM) state control
- TrustZone boundary configuration
- CRC-32 calculation on flash regions
- Key management (wrapped keys and user keys)
- Factory reset (initialize command)
- ID authentication for protected devices
- Auto-detect Renesas USB devices or use plain UART
- Configurable baud rate (9600 to 4000000 bps)
- Verify after write option
- Minimal footprint - pure C with no external dependencies

## Protocol Support

Implements the Renesas RA Standard Boot Firmware protocol:

| Command          | Code | Description                    |
|------------------|------|--------------------------------|
| INQ (Inquire)    | 0x00 | Check connection status        |
| ERA (Erase)      | 0x12 | Erase flash sectors            |
| WRI (Write)      | 0x13 | Write data to flash            |
| REA (Read)       | 0x15 | Read data from flash           |
| CRC              | 0x06 | Calculate CRC-32               |
| DLM              | 0x05 | Get DLM state                  |
| DLM_TRN          | 0x09 | Transition DLM state           |
| BND (Boundary)   | 0x42 | Get TrustZone boundaries       |
| BND_SET          | 0x43 | Set TrustZone boundaries       |
| PRM (Param)      | 0x45 | Get initialization parameter   |
| PRM_SET          | 0x46 | Set initialization parameter   |
| INI (Initialize) | 0x44 | Factory reset to SSD           |
| BAU (Baudrate)   | 0x34 | Set UART baud rate             |
| SIG (Signature)  | 0x3A | Get device signature           |
| ARE (Area)       | 0x3B | Get memory area info           |
| IDA (ID Auth)    | 0x30 | ID code authentication         |
| KEY              | 0x0A | Inject wrapped key             |
| KEY_VFY          | 0x0C | Verify wrapped key             |
| UKEY             | 0x0B | Inject user wrapped key        |
| UKEY_VFY         | 0x0D | Verify user wrapped key        |

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

## Windows

### Download Prebuilt Binary

Download `radfu.exe` from the [latest release](https://github.com/vjardin/radfu/releases/latest).

### Building from Source on Windows

Requirements:
- Visual Studio 2019 or later (for MSVC compiler)
- Python 3 (for meson)
- Git

Open "Developer Command Prompt for VS" or "Developer PowerShell for VS", then:

```powershell
# Install build tools
pip install meson ninja

# Clone repository
git clone https://github.com/vjardin/radfu.git
cd radfu

# Build
meson setup build
meson compile -C build

# Run tests (optional, requires vcpkg cmocka)
vcpkg install cmocka:x64-windows
meson setup build --wipe --cmake-prefix-path="C:\vcpkg\installed\x64-windows"
meson test -C build
```

The resulting binary is `build\radfu.exe`.

### Usage on Windows

Renesas devices appear as COM ports (e.g., `COM3`). radfu auto-detects Renesas USB devices:

```powershell
# Auto-detect device
radfu.exe info

# Specify COM port explicitly
radfu.exe -p COM3 info

# Write firmware
radfu.exe write -a 0x0 -v firmware.bin

# UART mode with USB-serial adapter
radfu.exe -u -p COM5 info
```

To find which COM port the device is using, open Device Manager and look under
"Ports (COM & LPT)" for a Hitachi or Renesas device (VID 045B).

## Usage

```
Usage: radfu <command> [options] [file]

Commands:
  status                     Show comprehensive device status with ASCII diagram
  info                       Show device and memory information
  read <file>                Read flash memory to file
  write <file>               Write file to flash memory
  erase                      Erase flash sectors
  crc                        Calculate CRC-32 of flash region
  dlm                        Show Device Lifecycle Management state
  dlm-transit <state>        Transition DLM state (ssd/nsecsd/dpl/lck_dbg/lck_boot)
  boundary                   Show secure/non-secure boundary settings
  boundary-set               Set TrustZone boundaries
  param                      Show device parameter (initialization command)
  param-set <enable|disable> Enable/disable initialization command
  init                       Initialize device (factory reset to SSD state)
  osis                       Show OSIS (ID code protection) status
  key-set <type> <file>      Inject wrapped DLM key (secdbg|nonsecdbg|rma)
  key-verify <type>          Verify DLM key (secdbg|nonsecdbg|rma)
  ukey-set <idx> <file>      Inject user wrapped key from file at index
  ukey-verify <idx>          Verify user key at index

Options:
  -p, --port <dev>     Serial port (auto-detect if omitted)
  -a, --address <hex>  Start address (default: 0x0)
  -s, --size <hex>     Size in bytes
  -b, --baudrate <n>   Set UART baud rate (default: 9600)
  -i, --id <hex>       ID code for authentication (32 hex chars)
  -e, --erase-all      Erase all areas using ALeRASE magic ID
  -v, --verify         Verify after write
  -u, --uart           Use plain UART mode (P109/P110 pins)
      --cfs1 <KB>      Code flash secure region size without NSC
      --cfs2 <KB>      Code flash secure region size (total)
      --dfs <KB>       Data flash secure region size
      --srs1 <KB>      SRAM secure region size without NSC
      --srs2 <KB>      SRAM secure region size (total)
  -h, --help           Show this help message
  -V, --version        Show version

Examples:
  radfu status
  radfu info
  radfu read -a 0x0 -s 0x10000 firmware.bin
  radfu write -b 1000000 -a 0x0 -v firmware.bin
  radfu erase -a 0x0 -s 0x10000
  radfu crc -a 0x0 -s 0x10000
  radfu dlm
  radfu osis
  radfu -u -p /dev/ttyUSB0 info
```

## Device Status

The `radfu status` command provides a comprehensive overview of the device including
memory layout, flash usage, security configuration, and warnings for unsecured devices.
See [HARDWARE.md](HARDWARE.md) for complete output example and board documentation.

The bar symbols indicate protection status:
- `█`/`░` - Used/empty blocks without protection
- `▓`/`▒` - BPS (Block Protection Setting) protected blocks
- `◆`/`◇` - PBPS (Permanent Block Protection) - cannot be revoked
- `S`/`N` - TrustZone Secure/NSC regions

## Protocol Exploration

The `raw` command allows sending arbitrary bootloader commands for protocol analysis:

```sh
# Send signature request (0x3A)
radfu raw 0x3A

# Send area info request for area 0
radfu raw 0x3B 0x00
```

A scanning script is provided to iterate through all bootloader commands:

```sh
# Scan known commands with full details
./scripts/scan-commands.sh -k

# Scan all commands 0x00-0x7F
./scripts/scan-commands.sh -a

# Scan specific range with verbose output
./scripts/scan-commands.sh -v -r 0x30-0x40

# Query single command with data
./scripts/scan-commands.sh -c "0x3B 0x00"
```

## Supported Baud Rates

When using UART (not USB), the following baud rates are supported:

- RA4 Series (24 MHz SCI): 9600, 115200, 230400, 460800, 921600, 1000000, 1500000
- RA6 Series (60 MHz SCI): 9600 to 4000000 (including 2000000, 3000000, 3500000)

Note: USB communication is not affected by baud rate settings.

## DLM Key Management

The RA family uses Device Lifecycle Management (DLM) with cryptographic keys to control
debug access. These keys enable authenticated regression (unlocking) from locked states
while preserving flash contents. See [security/SECURITY.md](security/SECURITY.md) for detailed explanation.

### Key Types

| Keyword    | KYTY | Name           | Purpose                                    |
|------------|------|----------------|--------------------------------------------|
| secdbg     | 0x01 | SECDBG_KEY     | Secure debug authentication                |
| nonsecdbg  | 0x02 | NONSECDBG_KEY  | Non-secure debug authentication            |
| rma        | 0x03 | RMA_KEY        | Return Material Authorization              |

### SECDBG_KEY (secdbg)

The Secure Debug Key enables authenticated regression from locked states back to SSD
(Secure Software Development) state, allowing full debug access.

Benefits:
- Enables secure debug access on deployed devices
- Allows returning locked devices to development state
- Preserves flash contents during state transition

Limits:
- Requires knowledge of the injected key
- Must be injected before transitioning to locked state
- If lost, device cannot be unlocked without RMA

Usage:
```sh
radfu key-set secdbg secdbg.bin    # Inject before locking
radfu dlm-transit lck_dbg          # Lock device
# Later, use SECDBG_KEY to authenticate and regress to SSD
```

### NONSECDBG_KEY (nonsecdbg)

The Non-Secure Debug Key enables authenticated regression to NSECSD (Non-Secure Software
Development) state, allowing debug access to non-secure regions only.

Benefits:
- Enables limited debug access on deployed devices
- Protects secure code while allowing non-secure debugging
- Useful for field debugging of non-secure application code

Limits:
- Cannot access secure memory regions
- Must be injected before transitioning to locked state
- Provides less access than SECDBG_KEY

Usage:
```sh
radfu key-set nonsecdbg nonsecdbg.bin
```

### RMA_KEY (rma)

The Return Material Authorization Key is used for the RMA flow, allowing the manufacturer
to analyze failed devices returned from the field.

Benefits:
- Enables full device analysis for failure investigation
- Works even on fully locked devices
- Required for warranty/RMA processes

Limits:
- Typically only known to the manufacturer
- Intended for factory/lab use only
- May expose all device contents

Usage:
```sh
radfu key-set rma rma.bin
# Used during RMA process to regress device state
```

### Key Operations

```sh
# Inject wrapped keys (before locking the device)
radfu key-set secdbg secdbg.bin
radfu key-set nonsecdbg nonsecdbg.bin
radfu key-set rma rma.bin

# Verify key injection
radfu key-verify secdbg
radfu key-verify nonsecdbg
radfu key-verify rma
```

### Key Wrapping

DLM keys must be wrapped before injection. See [security/SECURITY.md](security/SECURITY.md) for the complete
key wrapping process using the `rawrapkey.sh` script or Renesas SKMT tool.

The wrapping process requires:
1. A UFPK (User Factory Programming Key) - generated locally
2. A W-UFPK (Wrapped UFPK) - obtained from Renesas DLM portal at https://dlm.renesas.com/

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
- Windows (supported)

## Supported Devices

- RA2 Series - Cortex-M23 (boot code 0xC3)
- RA4 Series - Cortex-M4/M23 (boot code 0xC3)
- RA4M2/RA4M3 - Cortex-M33 (tested on EK-RA4M2, boot code 0xC6)
- RA6 Series - Cortex-M33 (boot code 0xC6)
- RA8 Series - Cortex-M85 (boot code 0xC5)

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

### Board Documentation

- [HARDWARE.md](HARDWARE.md) - EK-RA4M2 board layout, pin connections, and wiring diagrams
- [EK-RA4M2 v1 User's Manual](https://www.renesas.com/en/document/man/ek-ra4m2-v1-users-manual) - See page 28 for J16 jumper details

## UART Mode

As an alternative to USB, you can use plain 2-wire UART communication via an external
USB-to-UART adapter connected to the MCU's SCI9 pins.

### Pin Connections (RA4M2 / GrpA / GrpB)

| Adapter | MCU Pin | Function             |
|---------|---------|----------------------|
| TX      | P110    | RxD (MCU receives)   |
| RX      | P109    | TxD (MCU transmits)  |
| GND     | GND     | Ground               |

### Important Notes

- J16 must be shorted (same as USB mode) to enter the ROM bootloader.
- Do NOT power the board via J11 (USB Device) when using UART mode.
  USB enumeration traffic interferes with UART synchronization. Use J10 or external power instead.
- Press the RESET button after powering on to enter boot mode.
- The RA4M2 uses 3.3V logic. Make sure your adapter supports 3.3V.

### Example

```sh
radfu -u -p /dev/ttyUSB0 info
```

### Tested Adapter

Tested with an FTDI FT232 adapter. Make sure the voltage jumper is set to 3.3V.
A compatible adapter can be purchased at:
https://fr.aliexpress.com/item/1005008296799409.html (select "USB to TTL (D)")

Note: In UART mode, radfu automatically queries the device's recommended maximum baud
rate and attempts to switch to the highest supported rate. If communication fails at
high speeds (common with cheaper adapters or long wires), use `-b <rate>` to select
a lower baud rate. The FT232 adapter reliably works up to 1 Mbps.

### Performance: USB vs UART

On the RA4M2, USB mode is significantly faster than UART mode due to higher throughput
and no baud rate negotiation overhead.

| Mode | Test Suite Time | Notes |
|------|-----------------|-------|
| USB  | ~3 seconds      | Direct USB CDC, no baud rate switching |
| UART | ~10 seconds     | 1.5 Mbps with FT232, includes baud negotiation |

USB mode is recommended when available. UART mode is useful when the USB port is
occupied by the application or when debugging requires a separate communication channel.

### Application Console (UART0)

The RA4M2 user application typically uses SCI0 (UART0) on pins P410/P411 for serial
console output. This is separate from the bootloader UART (SCI9 on P109/P110).

| Adapter      | MCU Pin | Function           |
|--------------|---------|---------------------|
| TX (blue)    | P411    | RxD (MCU receives)  |
| RX (green)   | P410    | TxD (MCU transmits) |
| GND (black)  | GND     | Ground              |

Default baud rate: 115200. Use 3.3V logic levels only.

## Documentation Sources

This implementation is based on the official Renesas documentation:

- [R01AN5372 - Renesas RA Family System Specifications for Standard Boot Firmware](https://www.renesas.com/en/document/apn/renesas-ra-family-system-specifications-standard-boot-firmware)

- [R01AN6347 - Renesas RA Family System Specifications (for newer devices)](https://www.renesas.com/en/document/apn/renesas-ra-family-system-specifications-standard-boot-firmware-0)

- [Standard Boot Firmware for RA family MCUs Based on Arm Cortex-M33](https://www.renesas.com/en/document/apn/standard-boot-firmware-ra-family-mcus-based-arm-cortex-m33)

## Acknowledgments

This project is inspired by [raflash](https://github.com/robinkrens/raflash), the original
Python implementation by Robin Krens.

RADFU is a C rewrite aimed at providing a minimal footprint suitable for embedded environments,
build systems, and resource-constrained scenarios where a Python runtime may not be available
or practical.

## Legal Notice

RADFU implements publicly documented Renesas boot firmware protocols.
No proprietary source code or confidential material is included. See LEGAL.md for details.

## License

AGPL-3.0-or-later

Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025-2026
