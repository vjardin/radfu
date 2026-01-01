/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * High-level flash operations for Renesas RA bootloader
 */

#define _DEFAULT_SOURCE

#include "radfu.h"
#include "rapacker.h"
#include "progress.h"

/* Make static functions visible for testing */
#ifdef TESTING
#define STATIC
#else
#define STATIC static
#endif

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define CHUNK_SIZE 1024

/*
 * Unpack packet and print MCU error if present
 * Returns: data length on success, -1 on error
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
      /* Error code is in data[0], not in cmd byte */
      uint8_t err_code = (dlen > 0 && data != NULL) ? data[0] : 0;
      warnx("%s: MCU error 0x%02X (%s: %s)",
          context,
          err_code,
          ra_strerror(err_code),
          ra_strdesc(err_code));
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

  if (rau > 0 && start % rau != 0) {
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

  /* Align end address to RAU boundary if needed */
  if (rau > 0 && (end + 1) % rau != 0) {
    uint32_t aligned_end = ((end / rau) + 1) * rau - 1;
    if (aligned_end > ead)
      aligned_end = ead;
    end = aligned_end;
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
  if (sad < 0x00100000)
    return "Code Flash";
  else if (sad >= 0x08000000 && sad < 0x09000000)
    return "Data Flash";
  else if (sad >= 0x01000000 && sad < 0x02000000)
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
  case 0x0:
    return "User/Code";
  case 0x1:
    return "Data";
  case 0x2:
    return "Config";
  default:
    return "Unknown";
  }
}

/*
 * Get device group name from TYP field per spec 6.15.2.2
 */
STATIC const char *
get_device_group(uint8_t typ) {
  switch (typ) {
  case 0x01:
    return "GrpA/GrpB (RA4M2/3, RA6M4/5, RA4E1, RA6E1)";
  case 0x02:
    return "GrpC (RA6T2)";
  case 0x05:
    return "GrpD (RA4E2, RA6E2, RA4T1, RA6T3)";
  default:
    return "Unknown";
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
      if (n > 0) {
        fprintf(stderr, "  resp: ");
        for (ssize_t j = 0; j < n; j++)
          fprintf(stderr, "%02x ", resp[j]);
        fprintf(stderr, "\n");
      }
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
    if (sad < 0x00100000)
      code_flash_size += area_size;
    else if (sad >= 0x08000000 && sad < 0x09000000)
      data_flash_size += area_size;
    else if (sad >= 0x01000000 && sad < 0x02000000)
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
      printf("Area %d [%s] (KOA=0x%02X): 0x%08X - 0x%08X\n",
          i, area_type, koa, sad, ead);
      printf("       Size: %-8s  Erase: %-8s  Write: %-8s  Read: %-8s  CRC: %s\n",
          size_str, erase_str, write_str, read_str, crc_str);
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

    printf("Device Group:       %s\n", get_device_group(typ));
    printf("Boot Firmware:      v%d.%d.%d\n", bfv_major, bfv_minor, bfv_build);
    printf("Max UART Baudrate:  %u bps\n", rmb);
    printf("Number of Areas:    %d\n", noa);

    /* Parse Device ID if available (16 bytes starting at offset 9) */
    if (data_len >= 25) {
      printf("Device ID:          ");
      for (int i = 0; i < 16; i++)
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
      char product[17] = { 0 };
      memcpy(product, &data[25], 16);
      /* Trim trailing spaces */
      for (int i = 15; i >= 0 && product[i] == ' '; i--)
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

#define ID_CODE_LEN 16

int
ra_authenticate(ra_device_t *dev, const uint8_t *id_code) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[16];
  ssize_t pkt_len, n;

  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), IDA_CMD, id_code, ID_CODE_LEN, false);
  if (pkt_len < 0)
    return -1;

  fprintf(stderr, "IDA send %zd bytes:", pkt_len);
  for (ssize_t i = 0; i < pkt_len; i++)
    fprintf(stderr, " %02X", pkt[i]);
  fprintf(stderr, "\n");

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  n = ra_recv(dev, resp, sizeof(resp), 500);
  fprintf(stderr, "IDA recv %zd bytes:", n);
  for (ssize_t i = 0; i < n && i < 16; i++)
    fprintf(stderr, " %02X", resp[i]);
  fprintf(stderr, "\n");

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
  uint8_t data[8];
  ssize_t pkt_len, n;
  uint32_t end;

  if (set_erase_boundaries(dev, start, size == 0 ? 1 : size, &end) < 0)
    return -1;

  printf("Erasing 0x%08x:0x%08x\n", start, end);

  uint32_to_be(start, &data[0]);
  uint32_to_be(end, &data[4]);

  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), ERA_CMD, data, 8, false);
  if (pkt_len < 0)
    return -1;

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  n = ra_recv(dev, resp, 7, 1000); /* Erase takes longer */
  if (n < 7) {
    warnx("short response for erase");
    return -1;
  }

  size_t data_len;
  if (unpack_with_error(resp, n, NULL, &data_len, "erase") < 0)
    return -1;

  printf("Erase complete\n");
  return 0;
}

int
ra_read(ra_device_t *dev, const char *file, uint32_t start, uint32_t size) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[CHUNK_SIZE + 6];
  uint8_t chunk[CHUNK_SIZE];
  uint8_t data[8];
  uint8_t ack_data[1] = { 0x00 };
  ssize_t pkt_len, n;
  uint32_t end;
  int fd;

  if (set_read_boundaries(dev, start, size == 0 ? 0x3FFFF - start : size, &end) < 0)
    return -1;

  fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    warn("failed to open %s", file);
    return -1;
  }

  uint32_to_be(start, &data[0]);
  uint32_to_be(end, &data[4]);

  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), REA_CMD, data, 8, false);
  if (pkt_len < 0) {
    close(fd);
    return -1;
  }

  fprintf(stderr, "REA send %zd bytes:", pkt_len);
  for (ssize_t i = 0; i < pkt_len; i++)
    fprintf(stderr, " %02X", pkt[i]);
  fprintf(stderr, "\n");

  if (ra_send(dev, pkt, pkt_len) < 0) {
    close(fd);
    return -1;
  }

  uint32_t nr_packets = (end - start) / CHUNK_SIZE;
  progress_t prog;
  progress_init(&prog, nr_packets + 1, "Reading");

  for (uint32_t i = 0; i <= nr_packets; i++) {
    n = ra_recv(dev, resp, CHUNK_SIZE + 6, 1000);
    fprintf(stderr, "REA recv %zd bytes:", n);
    for (ssize_t j = 0; j < n && j < 20; j++)
      fprintf(stderr, " %02X", resp[j]);
    fprintf(stderr, "\n");
    if (n < 7) {
      warnx("short response during read (%zd bytes)", n);
      close(fd);
      return -1;
    }

    size_t chunk_len;
    if (unpack_with_error(resp, n, chunk, &chunk_len, "read") < 0) {
      close(fd);
      return -1;
    }

    if (write(fd, chunk, chunk_len) != (ssize_t)chunk_len) {
      warn("write to file failed");
      close(fd);
      return -1;
    }

    /* Send ACK for all packets except the last one (per spec 6.20.3) */
    if (i < nr_packets) {
      pkt_len = ra_pack_pkt(pkt, sizeof(pkt), REA_CMD, ack_data, 1, true);
      if (pkt_len < 0) {
        close(fd);
        return -1;
      }
      ra_send(dev, pkt, pkt_len);
    }

    progress_update(&prog, i + 1);
  }

  progress_finish(&prog);
  close(fd);
  return 0;
}

int
ra_write(ra_device_t *dev, const char *file, uint32_t start, uint32_t size, bool verify) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[16];
  uint8_t data[8];
  uint8_t chunk[CHUNK_SIZE];
  ssize_t pkt_len, n;
  uint32_t end;
  int fd;
  struct stat st;

  fd = open(file, O_RDONLY);
  if (fd < 0) {
    warn("failed to open %s", file);
    return -1;
  }

  if (fstat(fd, &st) < 0) {
    warn("failed to stat %s", file);
    close(fd);
    return -1;
  }

  uint32_t file_size = (uint32_t)st.st_size;
  if (size == 0)
    size = file_size;

  if (size > file_size) {
    warnx("write size > file size");
    close(fd);
    return -1;
  }

  if (set_write_boundaries(dev, start, size, &end) < 0) {
    close(fd);
    return -1;
  }

  /* Calculate actual write size (WAU-aligned range) */
  uint32_t write_size = end - start + 1;

  /* Send write command */
  uint32_to_be(start, &data[0]);
  uint32_to_be(end, &data[4]);

  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), WRI_CMD, data, 8, false);
  if (pkt_len < 0) {
    close(fd);
    return -1;
  }

  if (ra_send(dev, pkt, pkt_len) < 0) {
    close(fd);
    return -1;
  }

  n = ra_recv(dev, resp, 7, 500);
  if (n < 7) {
    warnx("short response for write init");
    close(fd);
    return -1;
  }

  size_t data_len;
  if (unpack_with_error(resp, n, NULL, &data_len, "write init") < 0) {
    close(fd);
    return -1;
  }

  progress_t prog;
  progress_init(&prog, write_size, "Writing");

  uint32_t total = 0;
  while (total < write_size) {
    /* Calculate chunk size: min(CHUNK_SIZE, remaining) per spec 6.19 */
    uint32_t remaining = write_size - total;
    uint32_t chunk_size = remaining < CHUNK_SIZE ? remaining : CHUNK_SIZE;

    ssize_t bytes_read = read(fd, chunk, chunk_size);
    if (bytes_read < 0) {
      warn("read from file failed");
      close(fd);
      return -1;
    }

    /* Pad with zeros if file is smaller than write range */
    if (bytes_read < (ssize_t)chunk_size)
      memset(chunk + bytes_read, 0, chunk_size - bytes_read);

    pkt_len = ra_pack_pkt(pkt, sizeof(pkt), WRI_CMD, chunk, chunk_size, true);
    if (pkt_len < 0) {
      close(fd);
      return -1;
    }

    if (ra_send(dev, pkt, pkt_len) < 0) {
      close(fd);
      return -1;
    }

    n = ra_recv(dev, resp, 7, 500);
    if (n < 7) {
      warnx("short response during write");
      close(fd);
      return -1;
    }

    if (unpack_with_error(resp, n, NULL, &data_len, "write") < 0) {
      close(fd);
      return -1;
    }

    total += chunk_size;
    progress_update(&prog, total);
  }

  progress_finish(&prog);
  close(fd);

  if (verify) {
    const char *tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL)
      tmpdir = "/tmp";
    char tmpfile[PATH_MAX];
    snprintf(tmpfile, sizeof(tmpfile), "%s/radfu_verify_XXXXXX", tmpdir);
    int tmpfd = mkstemp(tmpfile);
    if (tmpfd < 0) {
      warn("failed to create temp file for verify");
      return -1;
    }
    close(tmpfd);

    if (ra_read(dev, tmpfile, start, size) < 0) {
      unlink(tmpfile);
      return -1;
    }

    /* Compare files */
    fd = open(file, O_RDONLY);
    tmpfd = open(tmpfile, O_RDONLY);
    if (fd < 0 || tmpfd < 0) {
      if (fd >= 0)
        close(fd);
      if (tmpfd >= 0)
        close(tmpfd);
      unlink(tmpfile);
      return -1;
    }

    uint8_t buf1[CHUNK_SIZE], buf2[CHUNK_SIZE];
    bool match = true;
    size_t compared = 0;

    while (compared < file_size) {
      ssize_t n1 = read(fd, buf1, CHUNK_SIZE);
      ssize_t n2 = read(tmpfd, buf2, CHUNK_SIZE);

      if (n1 < 0 || n2 < 0) {
        match = false;
        break;
      }
      if (n1 == 0)
        break;

      size_t cmp_len = (size_t)n1;
      if (memcmp(buf1, buf2, cmp_len) != 0) {
        match = false;
        break;
      }
      compared += cmp_len;
    }

    close(fd);
    close(tmpfd);
    unlink(tmpfile);

    if (match)
      printf("Verify complete\n");
    else
      printf("Verify failed\n");
  }

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
  { 0x01, "CM",       "Chip Manufacturing"                         },
  { 0x02, "SSD",      "Secure Software Development"                },
  { 0x03, "NSECSD",   "Non-Secure Software Development"            },
  { 0x04, "DPL",      "Deployed"                                   },
  { 0x05, "LCK_DBG",  "Locked Debug"                               },
  { 0x06, "LCK_BOOT", "Locked Boot Interface"                      },
  { 0x07, "RMA_REQ",  "Return Material Authorization Request"      },
  { 0x08, "RMA_ACK",  "Return Material Authorization Acknowledged" },
  { 0,    NULL,       NULL                                         },
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

  printf("Current DLM state: 0x%02X (%s)\n", current_dlm, ra_dlm_state_name(current_dlm));
  printf("Target DLM state:  0x%02X (%s)\n", dest_dlm, ra_dlm_state_name(dest_dlm));

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

  printf("DLM transit complete: %s -> %s\n", ra_dlm_state_name(current_dlm), ra_dlm_state_name(dest_dlm));
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

/* DLM state codes for initialize command */
#define DLM_CM 0x01
#define DLM_SSD 0x02
#define DLM_NSECSD 0x03
#define DLM_DPL 0x04

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
  if (current_dlm == DLM_CM) {
    warnx("cannot initialize from CM state (0x01)");
    warnx("initialize command requires SSD, NSECSD, or DPL state");
    return -1;
  }

  if (current_dlm != DLM_SSD && current_dlm != DLM_NSECSD && current_dlm != DLM_DPL) {
    warnx("cannot initialize from DLM state 0x%02X", current_dlm);
    warnx("initialize command requires SSD (0x02), NSECSD (0x03), or DPL (0x04) state");
    return -1;
  }

  printf("Current DLM state: 0x%02X (%s)\n", current_dlm, ra_dlm_state_name(current_dlm));
  printf("Initializing device (factory reset to SSD state)...\n");
  printf("WARNING: This will erase all flash areas and reset boundaries!\n");

  /* Send initialize command: SDLM = current state, DDLM = SSD */
  data[0] = current_dlm; /* SDLM: source DLM state */
  data[1] = DLM_SSD;     /* DDLM: destination DLM state (always SSD) */

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

  printf("Initialize complete - device reset to SSD state\n");
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

  /* Response contains KVST (key verification status) */
  int valid = (data_len >= 1 && resp_data[0] == 0x00) ? 1 : 0;

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

  /* Response contains KVST (key verification status) */
  int valid = (data_len >= 1 && resp_data[0] == 0x00) ? 1 : 0;

  if (valid)
    printf("User key at index %u: VALID\n", key_index);
  else
    printf("User key at index %u: INVALID or EMPTY\n", key_index);

  if (valid_out != NULL)
    *valid_out = valid;

  return 0;
}
