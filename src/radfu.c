/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * High-level flash operations for Renesas RA bootloader
 */

#define _DEFAULT_SOURCE

#include "compat.h"
#include "radfu.h"
#include "rapacker.h"
#include "progress.h"

#ifdef HAVE_OPENSSL
#include <openssl/evp.h>
#include <openssl/hmac.h>
#endif

/* Make static functions visible for testing */
#ifdef TESTING
#define STATIC
#else
#define STATIC static
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CHUNK_SIZE 1024

/* Protocol-defined field lengths (per spec 6.15.2.2) */
#define DEVICE_ID_LEN 16    /* DID: Device Identification */
#define PRODUCT_NAME_LEN 16 /* PTN: Product Type Name */

/*
 * Unpack packet and print MCU error if present
 * Returns: data length on success, -1 on error
 *
 * Error response format per spec 6.18.2.3/6.19.2.4:
 *   STS (1 byte)  - status code (ERR_ADDR, ERR_PROT, etc.)
 *   ST2 (4 bytes) - status details (FSTATR for flash errors)
 *   ADR (4 bytes) - failure address
 */
static ssize_t
unpack_with_error(
    const uint8_t *buf, size_t buflen, uint8_t *data, size_t *data_len, const char *context) {
  uint8_t cmd;
  size_t dlen = 0;
  ssize_t ret = ra_unpack_pkt(buf, buflen, data, &dlen, &cmd);
  if (data_len != NULL)
    *data_len = dlen;
  if (ret < 0) {
    if (cmd & STATUS_ERR) {
      /* Error code is in data[0], details in data[1-4], address in data[5-8] */
      uint8_t err_code = (dlen > 0 && data != NULL) ? data[0] : 0;
      warnx("%s: MCU error 0x%02X (%s: %s)",
          context,
          err_code,
          ra_strerror(err_code),
          ra_strdesc(err_code));
      /* Show failure address for flash access errors */
      if (dlen >= 9 && data != NULL) {
        uint32_t st2 = be_to_uint32(&data[1]);
        uint32_t adr = be_to_uint32(&data[5]);
        if (st2 != 0xFFFFFFFF || adr != 0xFFFFFFFF) {
          warnx("%s: flash status=0x%08X, failure address=0x%08X", context, st2, adr);
        }
      }
    } else {
      warnx("%s: unpack failed (cmd=0x%02X)", context, cmd);
    }
  }
  return ret;
}

/*
 * Find area index containing the given address
 * Returns: area index (0-3) on success, -1 if not found
 */
STATIC int
find_area_for_address(ra_device_t *dev, uint32_t addr) {
  for (int i = 0; i < MAX_AREAS; i++) {
    if (dev->chip_layout[i].sad == 0 && dev->chip_layout[i].ead == 0)
      continue;
    if (addr >= dev->chip_layout[i].sad && addr <= dev->chip_layout[i].ead)
      return i;
  }
  return -1;
}

int
ra_find_area_by_koa(ra_device_t *dev, uint8_t koa, uint32_t *sad_out, uint32_t *ead_out) {
  uint32_t combined_sad = 0xFFFFFFFF;
  uint32_t combined_ead = 0;
  int found = 0;

  /* Find all areas matching KOA and combine their ranges */
  for (int i = 0; i < MAX_AREAS; i++) {
    if (dev->chip_layout[i].sad == 0 && dev->chip_layout[i].ead == 0)
      continue;
    if (dev->chip_layout[i].koa == koa) {
      if (dev->chip_layout[i].sad < combined_sad)
        combined_sad = dev->chip_layout[i].sad;
      if (dev->chip_layout[i].ead > combined_ead)
        combined_ead = dev->chip_layout[i].ead;
      found = 1;
    }
  }

  if (!found) {
    warnx("no area found with KOA 0x%02X", koa);
    return -1;
  }

  if (sad_out)
    *sad_out = combined_sad;
  if (ead_out)
    *ead_out = combined_ead;

  return 0;
}

/*
 * Set boundaries for erase operations (requires EAU alignment)
 */
STATIC int
set_erase_boundaries(ra_device_t *dev, uint32_t start, uint32_t size, uint32_t *end_out) {
  int area = find_area_for_address(dev, start);
  if (area < 0) {
    warnx("address 0x%x not in any known area", start);
    return -1;
  }
  dev->sel_area = area;

  uint32_t eau = dev->chip_layout[area].eau;
  uint32_t ead = dev->chip_layout[area].ead;

  if (eau == 0) {
    warnx("area %d does not support erase operations", area);
    return -1;
  }

  if (start % eau != 0) {
    warnx("start address 0x%x not aligned on erase block size 0x%x", start, eau);
    return -1;
  }

  if (size < eau)
    warnx("warning: size less than erase block size, padding with zeros");

  uint32_t blocks = (size + eau - 1) / eau;
  uint32_t end = blocks * eau + start - 1;

  if (end <= start) {
    warnx("end address smaller or equal to start address");
    return -1;
  }

  if (end > ead) {
    warnx("size exceeds available ROM space (max 0x%x)", ead);
    return -1;
  }

  *end_out = end;
  return 0;
}

/*
 * Set boundaries for read operations (requires RAU alignment per spec 6.20)
 */
STATIC int
set_read_boundaries(ra_device_t *dev, uint32_t start, uint32_t size, uint32_t *end_out) {
  int area = find_area_for_address(dev, start);
  if (area < 0) {
    warnx("address 0x%x not in any known area", start);
    return -1;
  }
  dev->sel_area = area;

  uint32_t rau = dev->chip_layout[area].rau;
  uint32_t ead = dev->chip_layout[area].ead;

  if (rau == 0) {
    warnx("area %d does not support read operations (RAU=0)", area);
    return -1;
  }

  if (start % rau != 0) {
    warnx("start address 0x%x not aligned on read unit 0x%x", start, rau);
    return -1;
  }

  uint32_t end = start + size - 1;

  if (end <= start && size > 1) {
    warnx("end address smaller or equal to start address");
    return -1;
  }

  if (end > ead) {
    warnx("size exceeds area boundary (max 0x%x)", ead);
    return -1;
  }

  /* Align end address to RAU boundary if needed (per spec 6.20) */
  if ((end + 1) % rau != 0) {
    uint32_t aligned_end = ((end / rau) + 1) * rau - 1;
    if (aligned_end > ead)
      aligned_end = ead;
    if (aligned_end != end) {
      warnx("note: end address aligned from 0x%x to 0x%x (RAU=%u)", end, aligned_end, rau);
      end = aligned_end;
    }
  }

  *end_out = end;
  return 0;
}

/*
 * Set boundaries for write operations (requires WAU alignment)
 */
STATIC int
set_write_boundaries(ra_device_t *dev, uint32_t start, uint32_t size, uint32_t *end_out) {
  int area = find_area_for_address(dev, start);
  if (area < 0) {
    warnx("address 0x%x not in any known area", start);
    return -1;
  }
  dev->sel_area = area;

  uint32_t wau = dev->chip_layout[area].wau;
  uint32_t ead = dev->chip_layout[area].ead;

  if (wau == 0) {
    warnx("area %d does not support write operations", area);
    return -1;
  }

  if (start % wau != 0) {
    warnx("start address 0x%x not aligned on write block size 0x%x", start, wau);
    return -1;
  }

  uint32_t blocks = (size + wau - 1) / wau;
  uint32_t end = blocks * wau + start - 1;

  if (end <= start) {
    warnx("end address smaller or equal to start address");
    return -1;
  }

  if (end > ead) {
    warnx("size exceeds available ROM space (max 0x%x)", ead);
    return -1;
  }

  *end_out = end;
  return 0;
}

/*
 * Set boundaries for CRC operations (requires CAU alignment)
 */
STATIC int
set_crc_boundaries(ra_device_t *dev, uint32_t start, uint32_t size, uint32_t *end_out) {
  int area = find_area_for_address(dev, start);
  if (area < 0) {
    warnx("address 0x%x not in any known area", start);
    return -1;
  }
  dev->sel_area = area;

  uint32_t cau = dev->chip_layout[area].cau;
  uint32_t ead = dev->chip_layout[area].ead;

  if (cau == 0) {
    warnx("area %d does not support CRC operations", area);
    return -1;
  }

  if (start % cau != 0) {
    warnx("start address 0x%x not aligned on CRC unit 0x%x", start, cau);
    return -1;
  }

  uint32_t blocks = (size + cau - 1) / cau;
  uint32_t end = blocks * cau + start - 1;

  if (end <= start && size > cau) {
    warnx("end address smaller or equal to start address");
    return -1;
  }

  if (end > ead) {
    warnx("size exceeds area boundary (max 0x%x)", ead);
    return -1;
  }

  *end_out = end;
  return 0;
}

/*
 * Get area type name based on address range
 */
STATIC const char *
get_area_type(uint32_t sad) {
  if (sad < ADDR_CODE_FLASH_END)
    return "Code Flash";
  else if (sad >= ADDR_DATA_FLASH_START && sad < ADDR_DATA_FLASH_END)
    return "Data Flash";
  else if (sad >= ADDR_CONFIG_START && sad < ADDR_CONFIG_END)
    return "Config";
  else
    return "Unknown";
}

/*
 * Get area type from KOA (Kind of Area) field per spec 6.16.2.2
 * KOA format: 0xTN where T=type (0=User/Code, 1=Data, 2=Config), N=area index
 */
STATIC const char *
get_area_type_koa(uint8_t koa) {
  uint8_t type = (koa >> 4) & 0x0F;
  switch (type) {
  case KOA_TYPE_CODE:
    return "User/Code";
  case KOA_TYPE_DATA:
    return "Data";
  case KOA_TYPE_CONFIG:
    return "Config";
  default:
    return "Unknown";
  }
}

/*
 * Print device group info from TYP field per spec 6.15.2.2
 */
STATIC void
print_device_group(uint8_t typ) {
  const char *name;
  const char *devices;
  switch (typ) {
  case TYP_GRP_AB:
    name = "GrpA/GrpB";
    devices = "RA4M2/3, RA6M4/5, RA4E1, RA6E1";
    break;
  case TYP_GRP_C:
    name = "GrpC";
    devices = "RA6T2";
    break;
  case TYP_GRP_D:
    name = "GrpD";
    devices = "RA4E2, RA6E2, RA4T1, RA6T3";
    break;
  default:
    printf("Device Group:       Unknown (TYP=0x%02X)\n", typ);
    return;
  }
  printf("Device Group:       %s (TYP=0x%02X)\n", name, typ);
  printf("  Devices:          %s\n", devices);
}

/*
 * Print NOA (Number of Areas) with mode interpretation
 * Standard: 4 areas (User, Data, Config, + reserved)
 * Dual bank: 5+ areas (User0, User1, Data, Config, etc.)
 */
STATIC void
print_noa_info(uint8_t noa) {
  printf("Number of Areas:    %d", noa);
  if (noa == 4) {
    printf(" (linear mode)\n");
  } else if (noa > 4) {
    printf(" (dual bank mode)\n");
  } else {
    printf("\n");
  }
}

/*
 * Format size with appropriate unit (KB/MB)
 */
STATIC void
format_size(uint32_t bytes, char *buf, size_t buflen) {
  if (bytes >= 1024 * 1024)
    snprintf(buf, buflen, "%u MB", bytes / (1024 * 1024));
  else if (bytes >= 1024)
    snprintf(buf, buflen, "%u KB", bytes / 1024);
  else
    snprintf(buf, buflen, "%u bytes", bytes);
}

int
ra_get_area_info(ra_device_t *dev, bool print) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[64];
  uint8_t data[64];
  size_t data_len;
  ssize_t pkt_len, n;
  uint32_t code_flash_size = 0;
  uint32_t data_flash_size = 0;
  uint32_t config_size = 0;

  for (int i = 0; i < MAX_AREAS; i++) {
    uint8_t area = (uint8_t)i;
    pkt_len = ra_pack_pkt(pkt, sizeof(pkt), ARE_CMD, &area, 1, false);
    if (pkt_len < 0)
      return -1;

    if (ra_send(dev, pkt, pkt_len) < 0)
      return -1;

    n = ra_recv(dev, resp, sizeof(resp), 500);
    if (n < 7) {
      warnx("short response for area %d (got %zd bytes)", i, n);
      return -1;
    }

    if (unpack_with_error(resp, n, data, &data_len, "area info") < 0)
      return -1;

    /* Parse: KOA(1) + SAD(4) + EAD(4) + EAU(4) + WAU(4) + RAU(4) + CAU(4) = 25 bytes */
    if (data_len < 25) {
      warnx("invalid area info length: got %zu, expected 25", data_len);
      return -1;
    }

    uint8_t koa = data[0];
    uint32_t sad = be_to_uint32(&data[1]);
    uint32_t ead = be_to_uint32(&data[5]);
    uint32_t eau = be_to_uint32(&data[9]);
    uint32_t wau = be_to_uint32(&data[13]);
    uint32_t rau = be_to_uint32(&data[17]);
    uint32_t cau = be_to_uint32(&data[21]);

    dev->chip_layout[i].koa = koa;
    dev->chip_layout[i].sad = sad;
    dev->chip_layout[i].ead = ead;
    dev->chip_layout[i].eau = eau;
    dev->chip_layout[i].wau = wau;
    dev->chip_layout[i].rau = rau;
    dev->chip_layout[i].cau = cau;

    /* Calculate sizes by area type */
    uint32_t area_size = (ead >= sad) ? (ead - sad + 1) : 0;
    if (sad < ADDR_CODE_FLASH_END)
      code_flash_size += area_size;
    else if (sad >= ADDR_DATA_FLASH_START && sad < ADDR_DATA_FLASH_END)
      data_flash_size += area_size;
    else if (sad >= ADDR_CONFIG_START && sad < ADDR_CONFIG_END)
      config_size += area_size;

    if (print) {
      char size_str[32], erase_str[32], write_str[32], read_str[32], crc_str[32];
      format_size(area_size, size_str, sizeof(size_str));
      if (eau > 0)
        format_size(eau, erase_str, sizeof(erase_str));
      else
        snprintf(erase_str, sizeof(erase_str), "n/a");
      format_size(wau, write_str, sizeof(write_str));
      if (rau > 0)
        format_size(rau, read_str, sizeof(read_str));
      else
        snprintf(read_str, sizeof(read_str), "n/a");
      if (cau > 0)
        format_size(cau, crc_str, sizeof(crc_str));
      else
        snprintf(crc_str, sizeof(crc_str), "n/a");
      /* Use KOA for area type (spec 6.16.2.2), fallback to address-based */
      const char *area_type = (koa != 0) ? get_area_type_koa(koa) : get_area_type(sad);
      printf("Area %d [%s] (KOA=0x%02X): 0x%08X - 0x%08X\n", i, area_type, koa, sad, ead);
      printf("       Size: %-8s  Erase: %-8s  Write: %-8s  Read: %-8s  CRC: %s\n",
          size_str,
          erase_str,
          write_str,
          read_str,
          crc_str);
    }
  }

  /* Print summary */
  if (print) {
    char size_buf[32];
    printf("Memory:\n");
    if (code_flash_size > 0) {
      format_size(code_flash_size, size_buf, sizeof(size_buf));
      printf("  Code Flash: %s\n", size_buf);
    }
    if (data_flash_size > 0) {
      format_size(data_flash_size, size_buf, sizeof(size_buf));
      printf("  Data Flash: %s\n", size_buf);
    }
    if (config_size > 0) {
      format_size(config_size, size_buf, sizeof(size_buf));
      printf("  Config: %s\n", size_buf);
    }
  }

  return 0;
}

int
ra_get_dev_info(ra_device_t *dev) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[64]; /* Signature response can be up to 47 bytes */
  uint8_t data[64];
  size_t data_len;
  ssize_t pkt_len, n;

  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), SIG_CMD, NULL, 0, false);
  if (pkt_len < 0)
    return -1;

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  n = ra_recv(dev, resp, sizeof(resp), 500);
  if (n < 7) {
    warnx("short response for device info");
    return -1;
  }

  if (unpack_with_error(resp, n, data, &data_len, "signature") < 0)
    return -1;

  printf("==================== Device Information ====================\n");

  /*
   * Signature response format per spec 6.15.2.2:
   * - RMB (4 bytes): Recommended maximum UART baudrate [bps]
   * - NOA (1 byte): Number of accessible areas
   * - TYP (1 byte): Type code (device group)
   * - BFV (3 bytes): Boot firmware version (Major.Minor.Build)
   * - DID (16 bytes): Device unique ID
   * - PTN (16 bytes): Product type name
   * Total: 41 bytes
   */

  if (data_len >= 9) {
    /* Parse fields available in all formats */
    uint32_t rmb = be_to_uint32(&data[0]);
    uint8_t noa = data[4];
    uint8_t typ = data[5];
    uint8_t bfv_major = data[6];
    uint8_t bfv_minor = data[7];
    uint8_t bfv_build = data[8];

    print_device_group(typ);
    printf("Boot Firmware:      v%d.%d.%d\n", bfv_major, bfv_minor, bfv_build);
    if (rmb >= 1000000)
      printf("Max UART Baudrate:  %u bps (%.1f Mbps)\n", rmb, rmb / 1000000.0);
    else if (rmb >= 1000)
      printf("Max UART Baudrate:  %u bps (%.1f Kbps)\n", rmb, rmb / 1000.0);
    else
      printf("Max UART Baudrate:  %u bps\n", rmb);
    print_noa_info(noa);

    /* Parse Device ID if available (16 bytes starting at offset 9) */
    if (data_len >= 25) {
      printf("Device ID:          ");
      for (int i = 0; i < DEVICE_ID_LEN; i++)
        printf("%02X", data[9 + i]);
      printf("\n");

      /* Decode DID per spec 6.15.2.2:
       * - Wafer Fab (2 bytes): ASCII chars (e.g., "TT")
       * - Date info (packed)
       * - CRC16
       * - Lot Number (6 chars)
       * - Wafer Number (1 byte)
       * - X/Y address
       */
      char wafer_fab[3] = { data[9], data[10], '\0' };
      /* Year is in bits [7:4] of byte 11, month in bits [3:0], etc. */
      uint8_t year = (data[11] >> 4) & 0x0F;
      uint8_t month = data[11] & 0x0F;
      uint8_t day = data[12];
      uint16_t crc16 = ((uint16_t)data[13] << 8) | data[14];
      char lot[7] = { data[15], data[16], data[17], data[18], data[19], data[20], '\0' };
      uint8_t wafer_num = data[21];
      uint8_t x_addr = data[22];
      uint8_t y_addr = data[23];

      printf("  Wafer Fab:        %s\n", wafer_fab);
      printf("  Manufacturing:    20%02d-%02d-%02d\n", year + 10, month, day);
      printf("  CRC16:            0x%04X\n", crc16);
      printf("  Lot Number:       %s\n", lot);
      printf("  Wafer/X/Y:        %d / %d / %d\n", wafer_num, x_addr, y_addr);
    }

    /* Parse Product Type Name if available (16 bytes starting at offset 25) */
    if (data_len >= 41) {
      char product[PRODUCT_NAME_LEN + 1] = { 0 };
      memcpy(product, &data[25], PRODUCT_NAME_LEN);
      /* Trim trailing spaces */
      for (int i = PRODUCT_NAME_LEN - 1; i >= 0 && product[i] == ' '; i--)
        product[i] = '\0';

      printf("Product Name:       %s\n", product);

      /* Determine CPU core from product name (R7FAxxxx) */
      if (product[0] == 'R' && product[1] == '7' && product[2] == 'F' && product[3] == 'A') {
        char series = product[4];
        switch (series) {
        case '2':
          printf("CPU Core:           ARM Cortex-M23\n");
          break;
        case '4':
          printf("CPU Core:           ARM Cortex-M33\n");
          break;
        case '6':
          printf("CPU Core:           ARM Cortex-M33/M4\n");
          break;
        case '8':
          printf("CPU Core:           ARM Cortex-M85\n");
          break;
        default:
          printf("CPU Core:           Unknown\n");
        }
      }
    }
  } else {
    /* Minimal response - just print raw data */
    printf("Raw signature data (%zu bytes): ", data_len);
    for (size_t i = 0; i < data_len; i++)
      printf("%02X ", data[i]);
    printf("\n");
  }

  printf("=============================================================\n");

  return 0;
}

int
ra_get_rmb(ra_device_t *dev, uint32_t *rmb_out) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[64];
  uint8_t data[64];
  size_t data_len;
  ssize_t pkt_len, n;

  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), SIG_CMD, NULL, 0, false);
  if (pkt_len < 0)
    return -1;

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  n = ra_recv(dev, resp, sizeof(resp), 500);
  if (n < 7) {
    warnx("short response for signature");
    return -1;
  }

  if (unpack_with_error(resp, n, data, &data_len, "signature") < 0)
    return -1;

  if (data_len < 4) {
    warnx("signature response too short for RMB field");
    return -1;
  }

  /* RMB is first 4 bytes of signature response */
  *rmb_out = be_to_uint32(&data[0]);
  return 0;
}

uint32_t
ra_get_device_max_baudrate(ra_device_t *dev) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[64];
  uint8_t data[64];
  size_t data_len;
  ssize_t pkt_len, n;

  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), SIG_CMD, NULL, 0, false);
  if (pkt_len < 0)
    return 115200;

  if (ra_send(dev, pkt, pkt_len) < 0)
    return 115200;

  n = ra_recv(dev, resp, sizeof(resp), 500);
  if (n < 7)
    return 115200;

  if (ra_unpack_pkt(resp, n, data, &data_len, NULL) < 0)
    return 115200;

  /* Product name is at offset 25, 16 bytes (requires data_len >= 41) */
  if (data_len < 41)
    return 115200;

  char product[PRODUCT_NAME_LEN + 1] = { 0 };
  memcpy(product, &data[25], PRODUCT_NAME_LEN);

  /*
   * Determine max baud rate from product name (R7FAxxxx format)
   * - RA2 series (R7FA2xxx): 24 MHz SCI, max 1.5 Mbps
   * - RA4 series (R7FA4xxx): 24 MHz SCI, max 1.5 Mbps
   * - RA6 series (R7FA6xxx): 60 MHz SCI, max 4 Mbps
   * - RA8 series (R7FA8xxx): assumed similar to RA6
   */
  if (product[0] == 'R' && product[1] == '7' && product[2] == 'F' && product[3] == 'A') {
    char series = product[4];
    switch (series) {
    case '2':
    case '4':
      fprintf(stderr, "Device: RA%c series (24 MHz SCI, max 1.5 Mbps)\n", series);
      return 1500000;
    case '6':
    case '8':
      fprintf(stderr, "Device: RA%c series (60 MHz SCI, max 4 Mbps)\n", series);
      return 4000000;
    }
  }

  return 115200;
}

#define ID_CODE_LEN 16

int
ra_authenticate(ra_device_t *dev, const uint8_t *id_code) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[16];
  ssize_t pkt_len, n;

  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), IDA_CMD, id_code, ID_CODE_LEN, false);
  if (pkt_len < 0)
    return -1;

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  n = ra_recv(dev, resp, sizeof(resp), 500);
  if (n < 7) {
    warnx("short response for ID authentication");
    return -1;
  }

  uint8_t data[16];
  size_t data_len;
  if (unpack_with_error(resp, n, data, &data_len, "ID authentication") < 0)
    return -1;

  dev->authenticated = true;
  fprintf(stderr, "ID authentication successful\n");
  return 0;
}

int
ra_erase(ra_device_t *dev, uint32_t start, uint32_t size) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[16];
  uint8_t cmd_data[8];
  uint8_t resp_data[16]; /* For error details: STS(1) + ST2(4) + ADR(4) */
  ssize_t pkt_len, n;
  uint32_t end;

  if (set_erase_boundaries(dev, start, size == 0 ? 1 : size, &end) < 0)
    return -1;

  printf("Erasing 0x%08x:0x%08x\n", start, end);

  uint32_to_be(start, &cmd_data[0]);
  uint32_to_be(end, &cmd_data[4]);

  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), ERA_CMD, cmd_data, 8, false);
  if (pkt_len < 0)
    return -1;

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  n = ra_recv(dev, resp, sizeof(resp), 5000); /* Erase takes longer */
  if (n < 7) {
    warnx("short response for erase");
    return -1;
  }

  size_t data_len;
  if (unpack_with_error(resp, n, resp_data, &data_len, "erase") < 0)
    return -1;

  printf("Erase complete\n");
  return 0;
}

int
ra_read(ra_device_t *dev, const char *file, uint32_t start, uint32_t size, output_format_t format) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[CHUNK_SIZE + 6];
  uint8_t chunk[CHUNK_SIZE];
  uint8_t data[8];
  uint8_t ack_data[1] = { STATUS_OK };
  ssize_t pkt_len, n;
  uint32_t end;
  uint8_t *buffer = NULL;
  size_t buffer_offset = 0;

  if (set_read_boundaries(dev, start, size == 0 ? 0x3FFFF - start : size, &end) < 0)
    return -1;

  uint32_t total_size = end - start + 1;
  buffer = malloc(total_size);
  if (!buffer) {
    warnx("failed to allocate read buffer");
    return -1;
  }

  uint32_to_be(start, &data[0]);
  uint32_to_be(end, &data[4]);

  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), REA_CMD, data, 8, false);
  if (pkt_len < 0) {
    free(buffer);
    return -1;
  }

  if (ra_send(dev, pkt, pkt_len) < 0) {
    free(buffer);
    return -1;
  }

  uint32_t nr_packets = (end - start) / CHUNK_SIZE;
  progress_t prog;
  progress_init(&prog, nr_packets + 1, "Reading");

  for (uint32_t i = 0; i <= nr_packets; i++) {
    n = ra_recv(dev, resp, CHUNK_SIZE + 6, 1000);
    if (n < 7) {
      warnx("short response during read (%zd bytes)", n);
      free(buffer);
      return -1;
    }

    size_t chunk_len;
    if (unpack_with_error(resp, n, chunk, &chunk_len, "read") < 0) {
      free(buffer);
      return -1;
    }

    memcpy(buffer + buffer_offset, chunk, chunk_len);
    buffer_offset += chunk_len;

    /* Send ACK for all packets except the last one (per spec 6.20.3) */
    if (i < nr_packets) {
      pkt_len = ra_pack_pkt(pkt, sizeof(pkt), REA_CMD, ack_data, 1, true);
      if (pkt_len < 0) {
        free(buffer);
        return -1;
      }
      ra_send(dev, pkt, pkt_len);
    }

    progress_update(&prog, i + 1);
  }

  progress_finish(&prog);

  /* Write buffer to file in specified format */
  int ret = format_write(file, format, buffer, buffer_offset, start);
  free(buffer);

  return ret;
}

int
ra_verify(
    ra_device_t *dev, const char *file, uint32_t start, uint32_t size, input_format_t format) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[CHUNK_SIZE + 6];
  uint8_t flash_chunk[CHUNK_SIZE];
  uint8_t data[8];
  uint8_t ack_data[1] = { STATUS_OK };
  ssize_t pkt_len, n;
  uint32_t end;
  parsed_file_t parsed;

  if (format_parse(file, format, &parsed) < 0)
    return -1;

  /* Use embedded address if available and no explicit address given */
  if (start == 0 && parsed.has_addr)
    start = parsed.base_addr;

  uint32_t file_size = (uint32_t)parsed.size;
  if (size == 0)
    size = file_size;

  if (size > file_size) {
    warnx("verify size (%u) > file size (%u)", size, file_size);
    free(parsed.data);
    return -1;
  }

  if (set_read_boundaries(dev, start, size, &end) < 0) {
    free(parsed.data);
    return -1;
  }

  uint32_to_be(start, &data[0]);
  uint32_to_be(end, &data[4]);

  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), REA_CMD, data, 8, false);
  if (pkt_len < 0) {
    free(parsed.data);
    return -1;
  }

  if (ra_send(dev, pkt, pkt_len) < 0) {
    free(parsed.data);
    return -1;
  }

  uint32_t total_size = end - start + 1;
  uint32_t nr_packets = (end - start) / CHUNK_SIZE;
  uint32_t current_addr = start;
  uint32_t file_offset = 0;
  progress_t prog;
  progress_init(&prog, nr_packets + 1, "Verifying");

  for (uint32_t i = 0; i <= nr_packets; i++) {
    n = ra_recv(dev, resp, CHUNK_SIZE + 6, 1000);
    if (n < 7) {
      warnx("short response during verify (%zd bytes)", n);
      free(parsed.data);
      return -1;
    }

    size_t chunk_len;
    if (unpack_with_error(resp, n, flash_chunk, &chunk_len, "verify read") < 0) {
      free(parsed.data);
      return -1;
    }

    /* Compare flash data with file data from parsed buffer */
    size_t remaining = parsed.size - file_offset;
    size_t cmp_len = remaining < chunk_len ? remaining : chunk_len;
    for (size_t j = 0; j < cmp_len; j++) {
      if (flash_chunk[j] != parsed.data[file_offset + j]) {
        progress_finish(&prog);
        warnx("verify FAILED at 0x%08X: flash=0x%02X, file=0x%02X",
            current_addr + (uint32_t)j,
            flash_chunk[j],
            parsed.data[file_offset + j]);
        free(parsed.data);
        return -1;
      }
    }

    /* If file is shorter than flash region, remaining flash bytes should be 0xFF */
    if (cmp_len < chunk_len) {
      for (size_t j = cmp_len; j < chunk_len; j++) {
        if (flash_chunk[j] != 0xFF) {
          progress_finish(&prog);
          warnx("verify FAILED at 0x%08X: flash=0x%02X, expected=0xFF (beyond file)",
              current_addr + (uint32_t)j,
              flash_chunk[j]);
          free(parsed.data);
          return -1;
        }
      }
    }

    file_offset += cmp_len;
    current_addr += chunk_len;

    /* Send ACK for all packets except the last one (per spec 6.20.3) */
    if (i < nr_packets) {
      pkt_len = ra_pack_pkt(pkt, sizeof(pkt), REA_CMD, ack_data, 1, true);
      if (pkt_len < 0) {
        free(parsed.data);
        return -1;
      }
      ra_send(dev, pkt, pkt_len);
    }

    progress_update(&prog, i + 1);
  }

  progress_finish(&prog);
  free(parsed.data);

  printf("Verify OK: %u bytes at 0x%08X match file\n", total_size, start);
  return 0;
}

int
ra_blank_check(ra_device_t *dev, uint32_t start, uint32_t size) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[CHUNK_SIZE + 6];
  uint8_t flash_chunk[CHUNK_SIZE];
  uint8_t data[8];
  uint8_t ack_data[1] = { STATUS_OK };
  ssize_t pkt_len, n;
  uint32_t end;

  if (size == 0) {
    warnx("blank-check requires size (-s option)");
    return -1;
  }

  if (set_read_boundaries(dev, start, size, &end) < 0)
    return -1;

  uint32_to_be(start, &data[0]);
  uint32_to_be(end, &data[4]);

  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), REA_CMD, data, 8, false);
  if (pkt_len < 0)
    return -1;

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  uint32_t total_size = end - start + 1;
  uint32_t nr_packets = (end - start) / CHUNK_SIZE;
  uint32_t current_addr = start;
  progress_t prog;
  progress_init(&prog, nr_packets + 1, "Checking");

  for (uint32_t i = 0; i <= nr_packets; i++) {
    n = ra_recv(dev, resp, CHUNK_SIZE + 6, 1000);
    if (n < 7) {
      warnx("short response during blank check (%zd bytes)", n);
      return -1;
    }

    size_t chunk_len;
    if (unpack_with_error(resp, n, flash_chunk, &chunk_len, "blank check") < 0)
      return -1;

    /* Check all bytes are 0xFF (erased state) */
    for (size_t j = 0; j < chunk_len; j++) {
      if (flash_chunk[j] != 0xFF) {
        progress_finish(&prog);
        warnx("blank check FAILED at 0x%08X: found 0x%02X (expected 0xFF)",
            current_addr + (uint32_t)j,
            flash_chunk[j]);
        return -1;
      }
    }

    current_addr += chunk_len;

    /* Send ACK for all packets except the last one (per spec 6.20.3) */
    if (i < nr_packets) {
      pkt_len = ra_pack_pkt(pkt, sizeof(pkt), REA_CMD, ack_data, 1, true);
      if (pkt_len < 0)
        return -1;
      ra_send(dev, pkt, pkt_len);
    }

    progress_update(&prog, i + 1);
  }

  progress_finish(&prog);

  printf("Blank check OK: %u bytes at 0x%08X are erased\n", total_size, start);
  return 0;
}

int
ra_write(ra_device_t *dev,
    const char *file,
    uint32_t start,
    uint32_t size,
    bool verify,
    input_format_t format) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[16];
  uint8_t cmd_data[8];
  uint8_t resp_data[16]; /* For error details: STS(1) + ST2(4) + ADR(4) */
  uint8_t chunk[CHUNK_SIZE];
  ssize_t pkt_len, n;
  uint32_t end;
  parsed_file_t parsed;

  if (format_parse(file, format, &parsed) < 0)
    return -1;

  /* Use address from file if not specified on command line */
  if (start == 0 && parsed.has_addr)
    start = parsed.base_addr;

  uint32_t file_size = (uint32_t)parsed.size;
  if (size == 0)
    size = file_size;

  if (size > file_size) {
    warnx("write size > file size");
    free(parsed.data);
    return -1;
  }

  if (set_write_boundaries(dev, start, size, &end) < 0) {
    free(parsed.data);
    return -1;
  }

  /* Calculate actual write size (WAU-aligned range) */
  uint32_t write_size = end - start + 1;

  /* Send write command */
  uint32_to_be(start, &cmd_data[0]);
  uint32_to_be(end, &cmd_data[4]);

  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), WRI_CMD, cmd_data, 8, false);
  if (pkt_len < 0) {
    free(parsed.data);
    return -1;
  }

  if (ra_send(dev, pkt, pkt_len) < 0) {
    free(parsed.data);
    return -1;
  }

  n = ra_recv(dev, resp, sizeof(resp), 1000);
  if (n < 7) {
    warnx("short response for write init");
    free(parsed.data);
    return -1;
  }

  size_t data_len;
  if (unpack_with_error(resp, n, resp_data, &data_len, "write init") < 0) {
    free(parsed.data);
    return -1;
  }

  progress_t prog;
  progress_init(&prog, write_size, "Writing");

  uint32_t total = 0;
  uint32_t buf_offset = 0;
  while (total < write_size) {
    /* Calculate chunk size: min(CHUNK_SIZE, remaining) per spec 6.19 */
    uint32_t remaining = write_size - total;
    uint32_t chunk_size = remaining < CHUNK_SIZE ? remaining : CHUNK_SIZE;

    /* Copy from parsed buffer, pad with zeros if smaller than write range */
    uint32_t copy_size = (buf_offset + chunk_size <= file_size)
                             ? chunk_size
                             : (file_size > buf_offset ? file_size - buf_offset : 0);
    if (copy_size > 0)
      memcpy(chunk, parsed.data + buf_offset, copy_size);
    if (copy_size < chunk_size)
      memset(chunk + copy_size, 0, chunk_size - copy_size);
    buf_offset += copy_size;

    pkt_len = ra_pack_pkt(pkt, sizeof(pkt), WRI_CMD, chunk, chunk_size, true);
    if (pkt_len < 0) {
      free(parsed.data);
      return -1;
    }

    if (ra_send(dev, pkt, pkt_len) < 0) {
      free(parsed.data);
      return -1;
    }

    n = ra_recv(dev, resp, sizeof(resp), 2000);
    if (n < 7) {
      warnx("short response during write");
      free(parsed.data);
      return -1;
    }

    if (unpack_with_error(resp, n, resp_data, &data_len, "write") < 0) {
      free(parsed.data);
      return -1;
    }

    total += chunk_size;
    progress_update(&prog, total);
  }

  progress_finish(&prog);

  if (verify) {
    const char *tmpdir = get_temp_dir();
    char tmpfile[PATH_MAX];
    snprintf(tmpfile, sizeof(tmpfile), "%s%cradfu_verify_XXXXXX", tmpdir, path_separator());
    int tmpfd = mkstemp(tmpfile);
    if (tmpfd < 0) {
      warn("failed to create temp file for verify");
      free(parsed.data);
      return -1;
    }
    close(tmpfd);

    if (ra_read(dev, tmpfile, start, size, FORMAT_BIN) < 0) {
      unlink(tmpfile);
      free(parsed.data);
      return -1;
    }

    /* Compare parsed data with read-back */
    tmpfd = open(tmpfile, O_RDONLY);
    if (tmpfd < 0) {
      unlink(tmpfile);
      free(parsed.data);
      return -1;
    }

    uint8_t buf[CHUNK_SIZE];
    bool match = true;
    size_t compared = 0;

    while (compared < file_size) {
      ssize_t n_read = read(tmpfd, buf, CHUNK_SIZE);
      if (n_read < 0) {
        match = false;
        break;
      }
      if (n_read == 0)
        break;

      size_t cmp_len = (size_t)n_read;
      if (memcmp(parsed.data + compared, buf, cmp_len) != 0) {
        match = false;
        break;
      }
      compared += cmp_len;
    }

    close(tmpfd);
    unlink(tmpfile);

    if (match)
      printf("Verify complete\n");
    else
      printf("Verify failed\n");
  }

  free(parsed.data);
  return 0;
}

int
ra_crc(ra_device_t *dev, uint32_t start, uint32_t size, uint32_t *crc_out) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[16];
  uint8_t data[8];
  uint8_t resp_data[16];
  ssize_t pkt_len, n;
  uint32_t end;

  if (set_crc_boundaries(dev, start, size == 0 ? 1 : size, &end) < 0)
    return -1;

  printf("Calculating CRC for 0x%08x-0x%08x\n", start, end);

  uint32_to_be(start, &data[0]);
  uint32_to_be(end, &data[4]);

  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), CRC_CMD, data, 8, false);
  if (pkt_len < 0)
    return -1;

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  /* CRC calculation can take time for large areas */
  n = ra_recv(dev, resp, sizeof(resp), 5000);
  if (n < 7) {
    warnx("short response for CRC command");
    return -1;
  }

  size_t data_len;
  if (unpack_with_error(resp, n, resp_data, &data_len, "CRC") < 0)
    return -1;

  if (data_len < 4) {
    warnx("invalid CRC response length: %zu", data_len);
    return -1;
  }

  uint32_t crc = be_to_uint32(resp_data);
  printf("CRC-32: 0x%08X\n", crc);

  if (crc_out != NULL)
    *crc_out = crc;

  return 0;
}

/* DLM state code definitions */
static const struct {
  uint8_t code;
  const char *name;
  const char *desc;
} dlm_states[] = {
  { DLM_STATE_CM,       "CM",       "Chip Manufacturing"                         },
  { DLM_STATE_SSD,      "SSD",      "Secure Software Development"                },
  { DLM_STATE_NSECSD,   "NSECSD",   "Non-Secure Software Development"            },
  { DLM_STATE_DPL,      "DPL",      "Deployed"                                   },
  { DLM_STATE_LCK_DBG,  "LCK_DBG",  "Locked Debug"                               },
  { DLM_STATE_LCK_BOOT, "LCK_BOOT", "Locked Boot Interface"                      },
  { DLM_STATE_RMA_REQ,  "RMA_REQ",  "Return Material Authorization Request"      },
  { DLM_STATE_RMA_ACK,  "RMA_ACK",  "Return Material Authorization Acknowledged" },
  { 0,                  NULL,       NULL                                         },
};

const char *
ra_dlm_state_name(uint8_t code) {
  for (size_t i = 0; dlm_states[i].name != NULL; i++) {
    if (dlm_states[i].code == code)
      return dlm_states[i].name;
  }
  return "UNKNOWN";
}

static const char *
dlm_state_desc(uint8_t code) {
  for (size_t i = 0; dlm_states[i].name != NULL; i++) {
    if (dlm_states[i].code == code)
      return dlm_states[i].desc;
  }
  return "Unknown state";
}

int
ra_get_dlm(ra_device_t *dev, uint8_t *dlm_out) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[16];
  uint8_t resp_data[8];
  ssize_t pkt_len, n;

  /* DLM state request command has no data payload */
  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), DLM_CMD, NULL, 0, false);
  if (pkt_len < 0)
    return -1;

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  n = ra_recv(dev, resp, sizeof(resp), 500);
  if (n < 7) {
    warnx("short response for DLM state request");
    return -1;
  }

  size_t data_len;
  if (unpack_with_error(resp, n, resp_data, &data_len, "DLM state") < 0)
    return -1;

  if (data_len < 1) {
    warnx("invalid DLM response length: %zu", data_len);
    return -1;
  }

  uint8_t dlm = resp_data[0];

  if (dlm_out != NULL) {
    *dlm_out = dlm;
  } else {
    /* Print only when no output pointer provided (standalone dlm command) */
    printf("DLM State: 0x%02X (%s: %s)\n", dlm, ra_dlm_state_name(dlm), dlm_state_desc(dlm));
  }

  return 0;
}

int
ra_dlm_transit(ra_device_t *dev, uint8_t dest_dlm) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[32];
  uint8_t resp_data[16];
  uint8_t data[2];
  ssize_t pkt_len, n;

  /* First, get current DLM state */
  uint8_t current_dlm;
  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), DLM_CMD, NULL, 0, false);
  if (pkt_len < 0)
    return -1;

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  n = ra_recv(dev, resp, sizeof(resp), 500);
  if (n < 7) {
    warnx("short response for DLM state request");
    return -1;
  }

  size_t data_len;
  if (unpack_with_error(resp, n, resp_data, &data_len, "DLM state") < 0)
    return -1;

  if (data_len < 1) {
    warnx("invalid DLM response length: %zu", data_len);
    return -1;
  }

  current_dlm = resp_data[0];

  printf("DLM state transition: %s (0x%02X) -> %s (0x%02X)\n",
      ra_dlm_state_name(current_dlm),
      current_dlm,
      ra_dlm_state_name(dest_dlm),
      dest_dlm);

  if (current_dlm == dest_dlm) {
    printf("Already in target state\n");
    return 0;
  }

  /* Warn about LCK_BOOT - bootloader will hang after transition */
  if (dest_dlm == DLM_STATE_LCK_BOOT) {
    printf("WARNING: Transitioning to LCK_BOOT will cause bootloader to hang!\n");
    printf("         Device will no longer accept commands until power cycle.\n");
  }

  /* Send DLM state transit command: SDLM = current state, DDLM = dest state */
  data[0] = current_dlm; /* SDLM: source DLM state */
  data[1] = dest_dlm;    /* DDLM: destination DLM state */

  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), DLM_TRANSIT_CMD, data, 2, false);
  if (pkt_len < 0)
    return -1;

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  /* DLM transit may involve flash writes */
  n = ra_recv(dev, resp, sizeof(resp), 5000);
  if (n < 7) {
    /* If transitioning to LCK_BOOT, device won't respond after sending OK */
    if (dest_dlm == DLM_STATE_LCK_BOOT) {
      printf("DLM transit to LCK_BOOT complete (bootloader is now hung)\n");
      return 0;
    }
    warnx("short response for DLM state transit");
    return -1;
  }

  if (unpack_with_error(resp, n, resp_data, &data_len, "DLM transit") < 0)
    return -1;

  printf("DLM transit complete: %s -> %s\n",
      ra_dlm_state_name(current_dlm),
      ra_dlm_state_name(dest_dlm));
  return 0;
}

/*
 * Convert big-endian byte array to uint16_t
 */
static inline uint16_t
be_to_uint16(const uint8_t *buf) {
  return ((uint16_t)buf[0] << 8) | (uint16_t)buf[1];
}

int
ra_get_boundary(ra_device_t *dev, ra_boundary_t *bnd_out) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[32];
  uint8_t resp_data[16];
  ssize_t pkt_len, n;

  /* Boundary request command has no data payload */
  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), BND_CMD, NULL, 0, false);
  if (pkt_len < 0)
    return -1;

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  n = ra_recv(dev, resp, sizeof(resp), 500);
  if (n < 7) {
    warnx("short response for boundary request");
    return -1;
  }

  size_t data_len;
  if (unpack_with_error(resp, n, resp_data, &data_len, "boundary") < 0)
    return -1;

  /* Response: CFS1(2) + CFS2(2) + DFS1(2) + SRS1(2) + SRS2(2) = 10 bytes */
  if (data_len < 10) {
    warnx("invalid boundary response length: %zu", data_len);
    return -1;
  }

  uint16_t cfs1 = be_to_uint16(&resp_data[0]);
  uint16_t cfs2 = be_to_uint16(&resp_data[2]);
  uint16_t dfs = be_to_uint16(&resp_data[4]);
  uint16_t srs1 = be_to_uint16(&resp_data[6]);
  uint16_t srs2 = be_to_uint16(&resp_data[8]);

  printf("Secure/Non-secure Boundary Settings:\n");
  printf("  Code Flash secure (without NSC): %u KB\n", cfs1);
  printf("  Code Flash secure (total):       %u KB\n", cfs2);
  printf("  Data Flash secure:               %u KB\n", dfs);
  printf("  SRAM secure (without NSC):       %u KB\n", srs1);
  printf("  SRAM secure (total):             %u KB\n", srs2);

  if (cfs2 > cfs1)
    printf("  Code Flash NSC region:           %u KB\n", cfs2 - cfs1);
  if (srs2 > srs1)
    printf("  SRAM NSC region:                 %u KB\n", srs2 - srs1);

  if (bnd_out != NULL) {
    bnd_out->cfs1 = cfs1;
    bnd_out->cfs2 = cfs2;
    bnd_out->dfs = dfs;
    bnd_out->srs1 = srs1;
    bnd_out->srs2 = srs2;
  }

  return 0;
}

/*
 * Convert uint16_t to big-endian byte array
 */
static inline void
uint16_to_be(uint16_t val, uint8_t *buf) {
  buf[0] = (val >> 8) & 0xFF;
  buf[1] = val & 0xFF;
}

int
ra_set_boundary(ra_device_t *dev, const ra_boundary_t *bnd) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[32];
  uint8_t resp_data[16];
  uint8_t data[10];
  ssize_t pkt_len, n;

  /* Validate constraints */
  if (bnd->cfs1 > bnd->cfs2) {
    warnx("invalid boundary: CFS1 (%u KB) > CFS2 (%u KB)", bnd->cfs1, bnd->cfs2);
    return -1;
  }
  if (bnd->srs1 > bnd->srs2) {
    warnx("invalid boundary: SRS1 (%u KB) > SRS2 (%u KB)", bnd->srs1, bnd->srs2);
    return -1;
  }

  printf("Setting TrustZone boundaries:\n");
  printf("  Code Flash secure (without NSC): %u KB\n", bnd->cfs1);
  printf("  Code Flash secure (total):       %u KB\n", bnd->cfs2);
  printf("  Data Flash secure:               %u KB\n", bnd->dfs);
  printf("  SRAM secure (without NSC):       %u KB\n", bnd->srs1);
  printf("  SRAM secure (total):             %u KB\n", bnd->srs2);

  if (bnd->cfs2 > bnd->cfs1)
    printf("  Code Flash NSC region:           %u KB\n", bnd->cfs2 - bnd->cfs1);
  if (bnd->srs2 > bnd->srs1)
    printf("  SRAM NSC region:                 %u KB\n", bnd->srs2 - bnd->srs1);

  /* Pack boundary data: CFS1(2) + CFS2(2) + DFS(2) + SRS1(2) + SRS2(2) = 10 bytes */
  uint16_to_be(bnd->cfs1, &data[0]);
  uint16_to_be(bnd->cfs2, &data[2]);
  uint16_to_be(bnd->dfs, &data[4]);
  uint16_to_be(bnd->srs1, &data[6]);
  uint16_to_be(bnd->srs2, &data[8]);

  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), BND_SET_CMD, data, 10, false);
  if (pkt_len < 0)
    return -1;

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  /* Boundary setting involves flash writes */
  n = ra_recv(dev, resp, sizeof(resp), 5000);
  if (n < 7) {
    warnx("short response for boundary setting");
    return -1;
  }

  size_t data_len;
  if (unpack_with_error(resp, n, resp_data, &data_len, "boundary setting") < 0)
    return -1;

  printf("Boundary settings stored successfully\n");
  printf("Note: Settings become effective after device reset\n");
  return 0;
}

int
ra_get_param(ra_device_t *dev, uint8_t param_id, uint8_t *value_out) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[32];
  uint8_t resp_data[16];
  ssize_t pkt_len, n;

  /* Parameter request command with PMID (1 byte) */
  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), PRM_CMD, &param_id, 1, false);
  if (pkt_len < 0)
    return -1;

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  n = ra_recv(dev, resp, sizeof(resp), 500);
  if (n < 7) {
    warnx("short response for parameter request");
    return -1;
  }

  size_t data_len;
  if (unpack_with_error(resp, n, resp_data, &data_len, "parameter") < 0)
    return -1;

  if (data_len < 1) {
    warnx("invalid parameter response length: %zu", data_len);
    return -1;
  }

  uint8_t value = resp_data[0];

  /* Interpret based on parameter ID */
  if (param_id == PARAM_ID_INIT) {
    const char *state;
    if (value == PARAM_INIT_DISABLED)
      state = "disabled";
    else if (value == PARAM_INIT_ENABLED)
      state = "enabled";
    else
      state = "unknown";
    printf("Initialization command: 0x%02X (%s)\n", value, state);
  } else {
    printf("Parameter 0x%02X: 0x%02X\n", param_id, value);
  }

  if (value_out != NULL)
    *value_out = value;

  return 0;
}

int
ra_set_param(ra_device_t *dev, uint8_t param_id, uint8_t value) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[32];
  uint8_t resp_data[16];
  uint8_t data[2];
  ssize_t pkt_len, n;

  /* Display what we're setting */
  if (param_id == PARAM_ID_INIT) {
    const char *state;
    if (value == PARAM_INIT_DISABLED) {
      state = "disabled";
      warnx("WARNING: Disabling initialization prevents factory reset capability");
    } else if (value == PARAM_INIT_ENABLED) {
      state = "enabled";
    } else {
      warnx("invalid value 0x%02X for initialization parameter (use 0x00 or 0x07)", value);
      return -1;
    }
    printf("Setting initialization command: %s (0x%02X)\n", state, value);
  } else {
    printf("Setting parameter 0x%02X to 0x%02X\n", param_id, value);
  }

  /* Parameter setting command: PMID (1 byte) + PRMT (1 byte for INIT) */
  data[0] = param_id;
  data[1] = value;

  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), PRM_SET_CMD, data, 2, false);
  if (pkt_len < 0)
    return -1;

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  /* Parameter setting may involve flash writes */
  n = ra_recv(dev, resp, sizeof(resp), 5000);
  if (n < 7) {
    warnx("short response for parameter setting");
    return -1;
  }

  size_t data_len;
  if (unpack_with_error(resp, n, resp_data, &data_len, "parameter setting") < 0)
    return -1;

  printf("Parameter set successfully\n");
  return 0;
}

int
ra_initialize(ra_device_t *dev) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[32];
  uint8_t resp_data[16];
  uint8_t data[2];
  ssize_t pkt_len, n;

  /* First, get current DLM state */
  uint8_t current_dlm;
  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), DLM_CMD, NULL, 0, false);
  if (pkt_len < 0)
    return -1;

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  n = ra_recv(dev, resp, sizeof(resp), 500);
  if (n < 7) {
    warnx("short response for DLM state request");
    return -1;
  }

  size_t data_len;
  if (unpack_with_error(resp, n, resp_data, &data_len, "DLM state") < 0)
    return -1;

  if (data_len < 1) {
    warnx("invalid DLM response length: %zu", data_len);
    return -1;
  }

  current_dlm = resp_data[0];

  /* Check if we can execute initialize from current state */
  if (current_dlm == DLM_STATE_CM) {
    warnx("cannot initialize from CM state (0x01)");
    warnx("initialize command requires SSD, NSECSD, or DPL state");
    return -1;
  }

  if (current_dlm != DLM_STATE_SSD && current_dlm != DLM_STATE_NSECSD &&
      current_dlm != DLM_STATE_DPL) {
    warnx("cannot initialize from DLM state 0x%02X", current_dlm);
    warnx("initialize command requires SSD (0x02), NSECSD (0x03), or DPL (0x04) state");
    return -1;
  }

  printf("DLM state transition: %s (0x%02X) -> SSD (0x02)\n",
      ra_dlm_state_name(current_dlm),
      current_dlm);
  printf("Initializing device (factory reset)...\n");
  printf("WARNING: This will erase all flash areas and reset boundaries!\n");

  /* Send initialize command: SDLM = current state, DDLM = SSD */
  data[0] = current_dlm;   /* SDLM: source DLM state */
  data[1] = DLM_STATE_SSD; /* DDLM: destination DLM state (always SSD) */

  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), INI_CMD, data, 2, false);
  if (pkt_len < 0)
    return -1;

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  /* Initialize can take a long time due to flash erase */
  n = ra_recv(dev, resp, sizeof(resp), 30000);
  if (n < 7) {
    warnx("short response for initialize command");
    return -1;
  }

  if (unpack_with_error(resp, n, resp_data, &data_len, "initialize") < 0)
    return -1;

  printf("Initialize complete: %s -> SSD\n", ra_dlm_state_name(current_dlm));
  return 0;
}

int
ra_key_set(ra_device_t *dev, uint8_t key_index, const uint8_t *wrapped_key, size_t key_len) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[32];
  uint8_t resp_data[16];
  uint8_t data[64]; /* KYID (1) + WKDT (up to 48) */
  ssize_t pkt_len, n;

  if (key_len > 48) {
    warnx("wrapped key too long: %zu bytes (max 48)", key_len);
    return -1;
  }

  printf("Setting key at index %u (%zu bytes wrapped key)\n", key_index, key_len);

  /* Pack key data: KYID (1 byte) + WKDT (key_len bytes) */
  data[0] = key_index;
  memcpy(&data[1], wrapped_key, key_len);

  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), KEY_CMD, data, 1 + key_len, false);
  if (pkt_len < 0)
    return -1;

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  /* Key setting involves flash writes */
  n = ra_recv(dev, resp, sizeof(resp), 5000);
  if (n < 7) {
    warnx("short response for key setting");
    return -1;
  }

  size_t data_len;
  if (unpack_with_error(resp, n, resp_data, &data_len, "key setting") < 0)
    return -1;

  printf("Key set successfully at index %u\n", key_index);
  return 0;
}

int
ra_key_verify(ra_device_t *dev, uint8_t key_index, int *valid_out) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[32];
  uint8_t resp_data[16];
  ssize_t pkt_len, n;

  printf("Verifying key at index %u\n", key_index);

  /* Key verify command: KYID (1 byte) */
  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), KEY_VFY_CMD, &key_index, 1, false);
  if (pkt_len < 0)
    return -1;

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  n = ra_recv(dev, resp, sizeof(resp), 1000);
  if (n < 7) {
    warnx("short response for key verify");
    return -1;
  }

  size_t data_len;
  if (unpack_with_error(resp, n, resp_data, &data_len, "key verify") < 0)
    return -1;

  /* Response contains KVST (key verification status): STATUS_OK = valid */
  int valid = (data_len >= 1 && resp_data[0] == STATUS_OK) ? 1 : 0;

  if (valid)
    printf("Key at index %u: VALID\n", key_index);
  else
    printf("Key at index %u: INVALID or EMPTY\n", key_index);

  if (valid_out != NULL)
    *valid_out = valid;

  return 0;
}

int
ra_ukey_set(ra_device_t *dev, uint8_t key_index, const uint8_t *wrapped_key, size_t key_len) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[32];
  uint8_t resp_data[16];
  uint8_t data[64]; /* KYID (1) + WKDT (up to 48) */
  ssize_t pkt_len, n;

  if (key_len > 48) {
    warnx("wrapped key too long: %zu bytes (max 48)", key_len);
    return -1;
  }

  printf("Setting user key at index %u (%zu bytes wrapped key)\n", key_index, key_len);

  /* Pack key data: KYID (1 byte) + WKDT (key_len bytes) */
  data[0] = key_index;
  memcpy(&data[1], wrapped_key, key_len);

  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), UKEY_CMD, data, 1 + key_len, false);
  if (pkt_len < 0)
    return -1;

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  /* Key setting involves flash writes */
  n = ra_recv(dev, resp, sizeof(resp), 5000);
  if (n < 7) {
    warnx("short response for user key setting");
    return -1;
  }

  size_t data_len;
  if (unpack_with_error(resp, n, resp_data, &data_len, "user key setting") < 0)
    return -1;

  printf("User key set successfully at index %u\n", key_index);
  return 0;
}

int
ra_ukey_verify(ra_device_t *dev, uint8_t key_index, int *valid_out) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[32];
  uint8_t resp_data[16];
  ssize_t pkt_len, n;

  printf("Verifying user key at index %u\n", key_index);

  /* User key verify command: KYID (1 byte) */
  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), UKEY_VFY_CMD, &key_index, 1, false);
  if (pkt_len < 0)
    return -1;

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  n = ra_recv(dev, resp, sizeof(resp), 1000);
  if (n < 7) {
    warnx("short response for user key verify");
    return -1;
  }

  size_t data_len;
  if (unpack_with_error(resp, n, resp_data, &data_len, "user key verify") < 0)
    return -1;

  /* Response contains KVST (key verification status): STATUS_OK = valid */
  int valid = (data_len >= 1 && resp_data[0] == STATUS_OK) ? 1 : 0;

  if (valid)
    printf("User key at index %u: VALID\n", key_index);
  else
    printf("User key at index %u: INVALID or EMPTY\n", key_index);

  if (valid_out != NULL)
    *valid_out = valid;

  return 0;
}

/*
 * Fixed value for HMAC-SHA256 authentication (256 bits / 32 bytes)
 * Per R01AN5562: Response = HMAC-SHA256(Key, challenge || Fixed value)
 * The fixed value is defined by Renesas specification.
 * This is all zeros as per SKMT default behavior.
 */
static const uint8_t DLM_AUTH_FIXED_VALUE[32] = { 0x00 };

#ifdef HAVE_OPENSSL
/*
 * Compute HMAC-SHA256(key, data)
 * out: 32-byte output buffer
 * Returns: 0 on success, -1 on error
 */
static int
compute_hmac_sha256(
    const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t *out) {
  unsigned int out_len = 32;
  uint8_t *result = HMAC(EVP_sha256(), key, (int)key_len, data, data_len, out, &out_len);
  if (result == NULL || out_len != 32) {
    warnx("HMAC-SHA256 computation failed");
    return -1;
  }
  return 0;
}
#endif

int
ra_dlm_auth(ra_device_t *dev, uint8_t dest_dlm, const uint8_t *key) {
#ifndef HAVE_OPENSSL
  (void)dev;
  (void)dest_dlm;
  (void)key;
  warnx("dlm-auth requires OpenSSL support (rebuild with OpenSSL)");
  return -1;
#else
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[64];
  uint8_t resp_data[32];
  uint8_t data[4];
  ssize_t pkt_len, n;

  /* First, get current DLM state */
  uint8_t current_dlm;
  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), DLM_CMD, NULL, 0, false);
  if (pkt_len < 0)
    return -1;

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  n = ra_recv(dev, resp, sizeof(resp), 500);
  if (n < 7) {
    warnx("short response for DLM state request");
    return -1;
  }

  size_t data_len;
  if (unpack_with_error(resp, n, resp_data, &data_len, "DLM state") < 0)
    return -1;

  if (data_len < 1) {
    warnx("invalid DLM response length: %zu", data_len);
    return -1;
  }

  current_dlm = resp_data[0];

  printf("DLM state transition: %s (0x%02X) -> %s (0x%02X)\n",
      ra_dlm_state_name(current_dlm),
      current_dlm,
      ra_dlm_state_name(dest_dlm),
      dest_dlm);

  if (current_dlm == dest_dlm) {
    printf("Already in target state\n");
    return 0;
  }

  /* Validate authenticated transition is allowed */
  bool valid_transition = false;
  const char *key_name = "unknown";

  if (current_dlm == DLM_STATE_NSECSD && dest_dlm == DLM_STATE_SSD) {
    valid_transition = true;
    key_name = "SECDBG_KEY";
  } else if (current_dlm == DLM_STATE_DPL && dest_dlm == DLM_STATE_NSECSD) {
    valid_transition = true;
    key_name = "NONSECDBG_KEY";
  } else if ((current_dlm == DLM_STATE_SSD || current_dlm == DLM_STATE_DPL) &&
             dest_dlm == DLM_STATE_RMA_REQ) {
    valid_transition = true;
    key_name = "RMA_KEY";
    printf("WARNING: Transition to RMA_REQ will ERASE flash memory!\n");
  }

  if (!valid_transition) {
    warnx("invalid authenticated transition: %s -> %s",
        ra_dlm_state_name(current_dlm),
        ra_dlm_state_name(dest_dlm));
    warnx("valid authenticated transitions:");
    warnx("  NSECSD -> SSD (using SECDBG_KEY)");
    warnx("  DPL -> NSECSD (using NONSECDBG_KEY)");
    warnx("  SSD/DPL -> RMA_REQ (using RMA_KEY, erases flash!)");
    return -1;
  }

  printf("Authenticating with %s...\n", key_name);

  /*
   * Send authentication command packet:
   * CMD(0x30) + SDLM(1) + DDLM(1) + CHCT(1)
   * CHCT: 0x00 = random challenge, 0x01 = MCU unique ID (RMA_REQ only)
   */
  data[0] = current_dlm; /* SDLM: source DLM state */
  data[1] = dest_dlm;    /* DDLM: destination DLM state */
  data[2] = 0x00;        /* CHCT: use random challenge */

  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), DLM_AUTH_CMD, data, 3, false);
  if (pkt_len < 0)
    return -1;

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  /* Receive challenge (16 bytes) */
  n = ra_recv(dev, resp, sizeof(resp), 5000);
  if (n < 7) {
    warnx("short response for authentication challenge");
    return -1;
  }

  uint8_t challenge[16];
  if (unpack_with_error(resp, n, challenge, &data_len, "challenge") < 0)
    return -1;

  if (data_len < 16) {
    warnx("invalid challenge length: %zu (expected 16)", data_len);
    return -1;
  }

  printf("Received challenge: ");
  for (int i = 0; i < 16; i++)
    printf("%02X", challenge[i]);
  printf("\n");

  /*
   * Compute response:
   * GrpA/GrpB: HMAC-SHA256(key, challenge || fixed_value)
   * GrpC: AES-128-CMAC(key, challenge) - not implemented yet
   *
   * TODO: Add device type detection to switch between HMAC and CMAC
   * For now, assume GrpA/GrpB (HMAC-SHA256)
   */

  /* Build message: challenge (16 bytes) || fixed_value (32 bytes) = 48 bytes */
  uint8_t message[48];
  memcpy(message, challenge, 16);
  memcpy(message + 16, DLM_AUTH_FIXED_VALUE, 32);

  /* Compute HMAC-SHA256 response (32 bytes) */
  uint8_t response[32];
  if (compute_hmac_sha256(key, 16, message, 48, response) < 0)
    return -1;

  printf("Computed response: ");
  for (int i = 0; i < 32; i++)
    printf("%02X", response[i]);
  printf("\n");

  /*
   * Send response packet:
   * SOD(0x81) + LNH(0x00) + LNL(0x21) + RES(0x30) + MAC(32) + SUM + ETX
   */
  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), DLM_AUTH_CMD, response, 32, true);
  if (pkt_len < 0)
    return -1;

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  /* Receive final status - may take time for RMA_REQ (flash erase) */
  int timeout = (dest_dlm == DLM_STATE_RMA_REQ) ? 30000 : 5000;
  n = ra_recv(dev, resp, sizeof(resp), timeout);
  if (n < 7) {
    warnx("short response for authentication result");
    return -1;
  }

  if (unpack_with_error(resp, n, resp_data, &data_len, "authentication") < 0)
    return -1;

  printf("DLM authentication successful: %s -> %s\n",
      ra_dlm_state_name(current_dlm),
      ra_dlm_state_name(dest_dlm));
  return 0;
#endif /* HAVE_OPENSSL */
}

/*
 * Find config area in chip layout
 * Returns area index, or -1 if not found
 */
static int
find_config_area(ra_device_t *dev) {
  for (int i = 0; i < MAX_AREAS; i++) {
    if (dev->chip_layout[i].koa == KOA_TYPE_CONFIG)
      return i;
  }
  return -1;
}

/*
 * Config area register offsets (RA4M2 specific, relative to 0x0100A100)
 * See RA4M2 Hardware User's Manual section 6.2
 */
#define CFG_SAS_OFFSET 0x34       /* SAS register (contains FSPR, BTFLG) */
#define CFG_BPS_OFFSET 0xC0       /* Block Protection Setting (18 bytes) */
#define CFG_PBPS_OFFSET 0xE0      /* Permanent Block Protection (18 bytes) */
#define CFG_BPS_SEC_OFFSET 0x140  /* BPS for secure region */
#define CFG_PBPS_SEC_OFFSET 0x160 /* PBPS for secure region */
#define CFG_BPS_SEL_OFFSET 0x1C0  /* Block protection select */
#define CFG_BPS_LEN 18            /* BPS/PBPS register length in bytes */

/* FSPR bit position in SAS register (bit 8) */
#define SAS_FSPR_BIT 0x0100

/*
 * Count protected blocks in BPS/PBPS register
 * A bit value of 0 means the block is protected
 */
static int
count_protected_blocks(const uint8_t *bps, size_t len) {
  int count = 0;
  for (size_t i = 0; i < len; i++) {
    /* Count zero bits (protected blocks) */
    uint8_t byte = bps[i];
    for (int b = 0; b < 8; b++) {
      if ((byte & (1 << b)) == 0)
        count++;
    }
  }
  return count;
}

/*
 * Print block protection status
 */
static void
print_block_protection(const char *label, const uint8_t *bps, size_t len, bool is_permanent) {
  int protected = count_protected_blocks(bps, len);
  int total = (int)(len * 8);

  /* Check if all 0xFF (no protection) */
  bool all_ff = true;
  for (size_t i = 0; i < len; i++) {
    if (bps[i] != 0xFF) {
      all_ff = false;
      break;
    }
  }

  if (all_ff) {
    printf("  %s: none %s\n",
        label,
        is_permanent ? "(no permanent protection)" : "(no blocks protected)");
  } else if (protected == total) {
    printf("  %s: all blocks %s\n", label, is_permanent ? "permanently protected" : "protected");
  } else {
    printf("  %s: %d/%d blocks %s\n",
        label,
        protected,
        total,
        is_permanent ? "permanently protected" : "protected");
    /* List protected block numbers */
    printf("       blocks: ");
    bool first = true;
    for (size_t i = 0; i < len; i++) {
      for (int b = 0; b < 8; b++) {
        if ((bps[i] & (1 << b)) == 0) {
          if (!first)
            printf(", ");
          printf("%zu", i * 8 + b);
          first = false;
        }
      }
    }
    printf("\n");
  }
}

/*
 * Print hex dump with ASCII representation
 */
static void
hexdump(const uint8_t *data, size_t len, uint32_t base_addr) {
  for (size_t i = 0; i < len; i += 16) {
    printf("  %08X: ", base_addr + (uint32_t)i);

    /* Hex bytes */
    for (size_t j = 0; j < 16; j++) {
      if (i + j < len)
        printf("%02X ", data[i + j]);
      else
        printf("   ");
      if (j == 7)
        printf(" ");
    }

    /* ASCII */
    printf(" |");
    for (size_t j = 0; j < 16 && i + j < len; j++) {
      uint8_t c = data[i + j];
      printf("%c", (c >= 0x20 && c <= 0x7E) ? c : '.');
    }
    printf("|\n");
  }
}

int
ra_config_read(ra_device_t *dev) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[CHUNK_SIZE + 6];
  uint8_t chunk[CHUNK_SIZE];
  uint8_t data[8];
  uint8_t ack_data[1] = { STATUS_OK };
  ssize_t pkt_len, n;

  /* Find config area */
  int area = find_config_area(dev);
  if (area < 0) {
    warnx("config area not found in chip layout");
    return -1;
  }

  uint32_t sad = dev->chip_layout[area].sad;
  uint32_t ead = dev->chip_layout[area].ead;
  uint32_t rau = dev->chip_layout[area].rau;

  if (rau == 0) {
    warnx("config area does not support read operations");
    return -1;
  }

  uint32_t size = ead - sad + 1;
  printf("Config Area (0x%08X - 0x%08X, %u bytes):\n\n", sad, ead, size);

  /* Allocate buffer for config data */
  uint8_t *config = malloc(size);
  if (!config) {
    warnx("failed to allocate config buffer");
    return -1;
  }

  /* Set read boundaries */
  dev->sel_area = area;
  uint32_to_be(sad, &data[0]);
  uint32_to_be(ead, &data[4]);

  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), REA_CMD, data, 8, false);
  if (pkt_len < 0) {
    free(config);
    return -1;
  }

  if (ra_send(dev, pkt, pkt_len) < 0) {
    free(config);
    return -1;
  }

  /* Read config area */
  uint32_t nr_packets = (size - 1) / CHUNK_SIZE + 1;
  size_t offset = 0;

  for (uint32_t i = 0; i < nr_packets; i++) {
    n = ra_recv(dev, resp, CHUNK_SIZE + 6, 1000);
    if (n < 7) {
      warnx("short response during config read");
      free(config);
      return -1;
    }

    size_t chunk_len;
    if (unpack_with_error(resp, n, chunk, &chunk_len, "config read") < 0) {
      free(config);
      return -1;
    }

    memcpy(config + offset, chunk, chunk_len);
    offset += chunk_len;

    /* Send ACK except for last packet */
    if (i < nr_packets - 1) {
      pkt_len = ra_pack_pkt(pkt, sizeof(pkt), REA_CMD, ack_data, 1, true);
      if (pkt_len < 0) {
        free(config);
        return -1;
      }
      ra_send(dev, pkt, pkt_len);
    }
  }

  /* Analyze config area */
  int all_ff = 1;
  int all_zero = 1;
  for (size_t i = 0; i < size; i++) {
    if (config[i] != 0xFF)
      all_ff = 0;
    if (config[i] != 0x00)
      all_zero = 0;
  }

  if (all_ff) {
    printf("Status: Factory default (all 0xFF)\n\n");
  } else if (all_zero) {
    printf("Status: All zeros (fully protected/locked)\n\n");
  } else {
    printf("Status: Configured (non-default values present)\n\n");
  }

  /* Parse block protection registers if config area is large enough */
  if (size >= CFG_PBPS_OFFSET + CFG_BPS_LEN) {
    printf("Block Protection:\n");

    /* FSPR (Flash Security Protection) from SAS register */
    if (size > CFG_SAS_OFFSET + 1) {
      uint16_t sas = config[CFG_SAS_OFFSET] | ((uint16_t)config[CFG_SAS_OFFSET + 1] << 8);
      bool fspr_set = (sas & SAS_FSPR_BIT) == 0; /* 0 = protected */
      printf("  FSPR: %s (%s)\n",
          fspr_set ? "0 (locked)" : "1 (unlocked)",
          fspr_set ? "startup area protected" : "startup area changeable");
    }

    /* BPS - Block Protection Setting */
    print_block_protection("BPS", &config[CFG_BPS_OFFSET], CFG_BPS_LEN, false);

    /* PBPS - Permanent Block Protection Setting */
    print_block_protection("PBPS", &config[CFG_PBPS_OFFSET], CFG_BPS_LEN, true);

    /* BPS_SEC and PBPS_SEC if available */
    if (size >= CFG_PBPS_SEC_OFFSET + CFG_BPS_LEN) {
      print_block_protection("BPS_SEC", &config[CFG_BPS_SEC_OFFSET], CFG_BPS_LEN, false);
      print_block_protection("PBPS_SEC", &config[CFG_PBPS_SEC_OFFSET], CFG_BPS_LEN, true);
    }

    printf("\n");
  }

  /* Display hex dump */
  printf("Raw contents:\n");
  hexdump(config, size, sad);

  free(config);
  return 0;
}
