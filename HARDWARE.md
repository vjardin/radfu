# EK-RA4M2 Board Layout

ASCII diagram of the EK-RA4M2 evaluation board showing connectors used with radfu.

```
                         EK-RA4M2 Board (Top View)

          +-------------------------------------------------------+
          |                                                       |
          |   +-----+                                             |
  J11     |   |RESET|  Red Reset Button                           |     J10
  Device  |   +-----+                                             |     Debug
  USB  ===|                                                       |===  USB
  (DFU)   |   +---+                                               |     (J-Link)
          |   |J16|  Boot Mode Jumper (MD pin)                    |
          |   | o |  Open    = Normal mode (run user app)         |
          |   | o |  Shorted = Boot mode (ROM bootloader)         |
          |   +---+                                               |
          |                                                       |
          |                    +-----------+                      |
          |                    |   RA4M2   |                      |
          |                    |   MCU     |                      |
          |                    |           |                      |
          |                    +-----------+                      |
          |                                                       |
          |   +----------------------------------+                |
          |   |  P109  P110      GND             | Bootloader     |
          |   |  (TxD) (RxD)                     | UART (SCI9)    |
          |   +----------------------------------+                |
          |                                                       |
          |   +----------------------------------+                |
          |   |  P410  P411      GND             | App Console    |
          |   |  (TxD) (RxD)                     | UART (SCI0)    |
          |   +----------------------------------+                |
          |                                                       |
          +-------------------------------------------------------+
```

## Connector Reference

| Connector | Purpose                        | Usage with radfu                    |
|-----------|--------------------------------|-------------------------------------|
| J11       | Device USB (power + data)      | USB DFU mode, or app USB (ttyACM)   |
| J10       | Debug USB (J-Link)             | Power + debugging                   |
| J16       | Boot mode jumper               | Short to enter bootloader           |
| RESET     | Red reset button               | Press after changing J16            |
| P109/P110 | Bootloader UART (SCI9)         | Connect USB-UART adapter for -u mode|
| P410/P411 | Application console (SCI0)     | User application serial output      |

## Boot Mode Selection (J16)

```
    Normal Mode              Boot Mode (DFU)
    +---+                    +---+
    | o | <- open            |[o]| <- jumper installed
    | o |                    |[o]|
    +---+                    +---+
    Runs user app            ROM bootloader active
```

## USB Mode Wiring

Connect USB cable to J11 (Device USB). The board is powered via USB.

```
    PC                          EK-RA4M2
    +--------+                  +--------+
    |  USB   |==================|  J11   |
    |  Port  |   USB Cable      |  USB   |
    +--------+                  +--------+

    $ lsusb
    Bus 001 Device 042: ID 045b:0261 Hitachi, Ltd RA USB Boot

    $ radfu info
```

## UART Mode Wiring (Bootloader - SCI9)

Connect USB-UART adapter to P109/P110. Power board via J10 or J11.

```
    USB-UART Adapter            EK-RA4M2
    +------------+              +------------+
    | TX (blue)  |------------->| P110 (RxD) |
    | RX (green) |<-------------| P109 (TxD) |
    | GND (black)|--------------| GND        |
    +------------+              +------------+
    3.3V TTL levels

    radfu -u -p /dev/ttyUSB0 info
```

Important: Do NOT connect J11 when using UART mode for DFU. USB enumeration
interferes with UART synchronization. Use J10 for power instead.

## Application Console Wiring (SCI0)

Connect USB-UART adapter to P410/P411 for user application serial output.

```
    USB-UART Adapter            EK-RA4M2
    +------------+              +------------+
    | TX (blue)  |------------->| P411 (RxD) |
    | RX (green) |<-------------| P410 (TxD) |
    | GND (black)|--------------| GND        |
    +------------+              +------------+
    3.3V TTL levels, 115200 bps

    screen /dev/ttyUSB0 115200
```

## Application USB (ttyACM)

In normal mode (J16 open), the user application can use J11 for USB
communication with the host. The device typically appears as /dev/ttyACM0.

## Quick Start

1. Enter boot mode:
   - Install jumper on J16
   - Press RESET

2. Program via USB (recommended):
   - Connect USB to J11
   - radfu write -v firmware.bin

3. Return to normal mode:
   - Remove jumper from J16
   - Press RESET

4. Monitor application console:
   - Connect USB-UART adapter to P410/P411
   - screen /dev/ttyUSB0 115200

## Device Status

The `radfu status` command provides a comprehensive overview of the device:

```
$ radfu status

╔════════════════════════════════════════════════════════════════════════════╗
║                            RADFU DEVICE STATUS                             ║
╠════════════════════════════════════════════════════════════════════════════╣
║  MCU: R7FA4M2AD3CFP     Group: GrpA/GrpB   Core: Cortex-M33                ║
║  Boot FW: v1.6.25      Max Baud: 6.0 Mbps    Mode: Linear                  ║
╠════════════════════════════════════════════════════════════════════════════╣
║                               MEMORY LAYOUT                                ║
╠════════════════════════════════════════════════════════════════════════════╣
║                                                                            ║
║  ┌──────────────────────────────────────────────────────────────────────┐  ║
║  │ CODE FLASH     512 KB   0x00000000 - 0x0007FFFF                      │  ║
║  │ █████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░   23% used                  │  ║
║  │                                                                      │  ║
║  │ MCUboot Partitions:                                                  │  ║
║  │   0x00000000   64 KB  bootloader (40 KB used, 63%)                   │  ║
║  │   0x00010000   80 KB  v0.1.0 [Bank0] (39 KB used, 48%)               │  ║
║  │   0x00024000  320 KB  v1.0.0 [Bank1] (40 KB used, 12%)               │  ║
║  │   0x00074000   48 KB  storage (0 bytes used, 0%)                     │  ║
║  ├──────────────────────────────────────────────────────────────────────┤  ║
║  │ DATA FLASH       8 KB   0x08000000 - 0x08001FFF                      │  ║
║  │ (usage unknown - erased data reads undefined per HW spec)            │  ║
║  ├──────────────────────────────────────────────────────────────────────┤  ║
║  │ CONFIG AREA     512 B   0x0100A100 - 0x0100A2FF                      │  ║
║  └──────────────────────────────────────────────────────────────────────┘  ║
║                                                                            ║
║  Memory: [CODE ███████████████████] [DATA █] [CFG █]                       ║
║                                                                            ║
║  Legend: █=used ░=empty  ▓=BPS prot  ◆=PBPS perm  S=Secure N=NSC           ║
║          FSPR: UNLOCKED (BPS registers can be modified)                    ║
║                                                                            ║
║  ⚠  SECURITY WARNING: Device is NOT secured!                               ║
║                                                                            ║
║    ✗ No block protection: All flash blocks can be erased/written           ║
║    ✗ FSPR unlocked: Block protection settings can be modified              ║
║    ✗ No TrustZone: All memory is Non-Secure                                ║
║    ✗ DLM in SSD: Full debug and serial access enabled                      ║
║    ✗ No DLM keys: Cannot use authenticated state regression                ║
║                                                                            ║
╠════════════════════════════════════════════════════════════════════════════╣
║                              SECURITY STATUS                               ║
╠════════════════════════════════════════════════════════════════════════════╣
║  DLM State: SSD (0x02)                                                     ║
║  OSIS:      Unlocked (no ID protection)                                    ║
║  Init Cmd:  Disabled                                                       ║
║                                                                            ║
║  TrustZone Boundaries:                                                     ║
║  ┌────────────────┬─────────┬─────────┬─────────────┐                      ║
║  │ Region         │ Secure  │   NSC   │ Non-Secure  │                      ║
║  ├────────────────┼─────────┼─────────┼─────────────┤                      ║
║  │ Code Flash     │    0 KB │    0 KB │     512 KB  │                      ║
║  │ Data Flash     │    0 KB │    -    │       8 KB  │                      ║
║  │ SRAM           │    0 KB │    0 KB │      -      │                      ║
║  └────────────────┴─────────┴─────────┴─────────────┘                      ║
║                                                                            ║
║  DLM Keys:                                                                 ║
║    SECDBG: [ ] N/A         NONSECDBG: [ ] N/A         RMA: [ ] N/A         ║
║                                                                            ║
║  Block Protection (BPS):                                                   ║
║    Blocks: [░░░░░░░░░░░░░░░░] 0/144 protected                              ║
║    PBPS:   0 blocks permanently protected                                  ║
║    FSPR:   1 (unlocked)                                                    ║
║                                                                            ║
╚════════════════════════════════════════════════════════════════════════════╝
```
