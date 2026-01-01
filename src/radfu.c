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
static int
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
static int
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
 * Set boundaries for read operations (no alignment required)
 */
static int
set_read_boundaries(ra_device_t *dev, uint32_t start, uint32_t size, uint32_t *end_out) {
  int area = find_area_for_address(dev, start);
  if (area < 0) {
    warnx("address 0x%x not in any known area", start);
    return -1;
  }
  dev->sel_area = area;

  uint32_t ead = dev->chip_layout[area].ead;
  uint32_t end = start + size - 1;

  if (end <= start && size > 1) {
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
 * Set boundaries for write operations (requires WAU alignment)
 */
static int
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
static int
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
static const char *
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
 * Format size with appropriate unit (KB/MB)
 */
static void
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
      char size_str[32], erase_str[32], write_str[32], crc_str[32];
      format_size(area_size, size_str, sizeof(size_str));
      if (eau > 0)
        format_size(eau, erase_str, sizeof(erase_str));
      else
        snprintf(erase_str, sizeof(erase_str), "n/a");
      format_size(wau, write_str, sizeof(write_str));
      if (cau > 0)
        format_size(cau, crc_str, sizeof(crc_str));
      else
        snprintf(crc_str, sizeof(crc_str), "n/a");
      printf("Area %d [%s]: 0x%08x-0x%08x (%s, erase %s, write %s, crc %s)\n",
          i,
          get_area_type(sad),
          sad,
          ead,
          size_str,
          erase_str,
          write_str,
          crc_str);
    }
    (void)koa; /* KOA field - reserved for future use */
    (void)rau; /* RAU field - read alignment, currently unused */
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
  uint8_t resp[64]; /* RA6 includes product name in response */
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

  /* Get packet length from header */
  uint16_t resp_len = ((uint16_t)resp[1] << 8) | resp[2];
  size_t total_len = 4 + (resp_len - 1) + 2; /* header + data + footer */

  printf("====================\n");

  if (n >= 18 && total_len == 18) {
    /* Short format: Header(4) + SCI(4) + RMB(4) + NOA(1) + TYP(1) + BFV(2) + Footer(2) */
    uint32_t sci = be_to_uint32(&resp[4]);
    uint32_t rmb = be_to_uint32(&resp[8]);
    uint8_t noa = resp[12];
    uint8_t typ = resp[13];
    uint16_t bfv = ((uint16_t)resp[14] << 8) | resp[15];

    if (typ == 0x02)
      printf("Chip: RA MCU + RA2/RA4 Series\n");
    else if (typ == 0x03)
      printf("Chip: RA MCU + RA6 Series\n");
    else
      printf("Unknown MCU type (0x%02x)\n", typ);

    printf("Serial interface speed: %u Hz\n", sci);
    printf("Recommend max UART baud rate: %u bps\n", rmb);
    printf("User area in Code flash [%d|%d]\n", noa & 0x1, (noa & 0x02) >> 1);
    printf("User area in Data flash [%d]\n", (noa & 0x04) >> 2);
    printf("Config area [%d]\n", (noa & 0x08) >> 3);
    printf("Boot firmware: version %d.%d\n", bfv >> 8, bfv & 0xFF);
  } else {
    /* Extended format (RA4M2, etc.): includes product name */
    uint32_t sci = be_to_uint32(&resp[4]);
    printf("Serial interface speed: %u Hz\n", sci);

    /* Extract product name from end of packet (before checksum+ETX) */
    /* Format: ...TR7F + product_name(13) + SUM + ETX */
    if (n >= 20) {
      /* Find "R7F" marker and extract product name */
      char product[16] = { 0 };
      for (ssize_t i = n - 20; i < n - 5; i++) {
        if (resp[i] == 'R' && resp[i + 1] == '7' && resp[i + 2] == 'F') {
          /* Copy product name (up to 13 chars) */
          size_t j = 0;
          for (; j < 13 && i + j < (size_t)(n - 2); j++) {
            if (resp[i + j] == ' ' || resp[i + j] == 0)
              break;
            product[j] = resp[i + j];
          }
          product[j] = '\0';
          break;
        }
      }
      if (product[0]) {
        printf("Product: %s\n", product);
        /* Determine series from product name */
        if (product[3] == 'A' && product[4] == '2')
          printf("Chip: RA MCU + RA2 Series (Cortex-M23)\n");
        else if (product[3] == 'A' && product[4] == '4')
          printf("Chip: RA MCU + RA4 Series (Cortex-M33)\n");
        else if (product[3] == 'A' && product[4] == '6')
          printf("Chip: RA MCU + RA6 Series (Cortex-M33)\n");
        else
          printf("Chip: RA MCU\n");
      }
    }
  }

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

    /* Send ACK */
    pkt_len = ra_pack_pkt(pkt, sizeof(pkt), REA_CMD, ack_data, 1, true);
    if (pkt_len < 0) {
      close(fd);
      return -1;
    }
    ra_send(dev, pkt, pkt_len);

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
  progress_init(&prog, size, "Writing");

  uint32_t total = 0;
  while (total < size) {
    ssize_t bytes_read = read(fd, chunk, CHUNK_SIZE);
    if (bytes_read < 0) {
      warn("read from file failed");
      close(fd);
      return -1;
    }
    if (bytes_read == 0)
      break;

    /* Pad to chunk size if needed */
    if (bytes_read < CHUNK_SIZE)
      memset(chunk + bytes_read, 0, CHUNK_SIZE - bytes_read);

    pkt_len = ra_pack_pkt(pkt, sizeof(pkt), WRI_CMD, chunk, CHUNK_SIZE, true);
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

    total += CHUNK_SIZE;
    progress_update(&prog, total > size ? size : total);
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

static const char *
dlm_state_name(uint8_t code) {
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
  printf("DLM State: 0x%02X (%s: %s)\n", dlm, dlm_state_name(dlm), dlm_state_desc(dlm));

  if (dlm_out != NULL)
    *dlm_out = dlm;

  return 0;
}
