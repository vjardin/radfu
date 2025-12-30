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
  ssize_t ret = ra_unpack_pkt(buf, buflen, data, data_len, &cmd);
  if (ret < 0 && (cmd & STATUS_ERR)) {
    uint8_t err_code = cmd & 0x7F;
    warnx("%s: MCU error 0x%02X (%s: %s)",
        context,
        err_code,
        ra_strerror(err_code),
        ra_strdesc(err_code));
  }
  return ret;
}

static void
uint32_to_be(uint32_t val, uint8_t *buf) {
  buf[0] = (val >> 24) & 0xFF;
  buf[1] = (val >> 16) & 0xFF;
  buf[2] = (val >> 8) & 0xFF;
  buf[3] = val & 0xFF;
}

static uint32_t
be_to_uint32(const uint8_t *buf) {
  return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8) |
         (uint32_t)buf[3];
}

static int
set_size_boundaries(ra_device_t *dev, uint32_t start, uint32_t size, uint32_t *end_out) {
  uint32_t align = dev->chip_layout[dev->sel_area].align;
  uint32_t ead = dev->chip_layout[dev->sel_area].ead;

  if (align == 0) {
    warnx("invalid alignment (area not configured?)");
    return -1;
  }

  if (start % align != 0) {
    warnx("start address 0x%x not aligned on sector size 0x%x", start, align);
    return -1;
  }

  if (size < align)
    warnx("warning: size less than sector size, padding with zeros");

  uint32_t blocks = (size + align - 1) / align;
  uint32_t end = blocks * align + start - 1;

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

int
ra_get_area_info(ra_device_t *dev, bool print) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[32];
  uint8_t data[32];
  size_t data_len;
  ssize_t pkt_len, n;

  for (int i = 0; i < MAX_AREAS; i++) {
    uint8_t area = (uint8_t)i;
    pkt_len = ra_pack_pkt(pkt, sizeof(pkt), ARE_CMD, &area, 1, false);
    if (pkt_len < 0)
      return -1;

    if (ra_send(dev, pkt, pkt_len) < 0)
      return -1;

    n = ra_recv(dev, resp, 23, 500);
    if (n < 23) {
      warnx("short response for area %d", i);
      return -1;
    }

    if (unpack_with_error(resp, n, data, &data_len, "area info") < 0)
      return -1;

    /* Parse: KOA(1) + SAD(4) + EAD(4) + EAU(4) + WAU(4) = 17 bytes */
    if (data_len < 17) {
      warnx("invalid area info length");
      return -1;
    }

    uint8_t koa = data[0];
    uint32_t sad = be_to_uint32(&data[1]);
    uint32_t ead = be_to_uint32(&data[5]);
    uint32_t eau = be_to_uint32(&data[9]);
    uint32_t wau = be_to_uint32(&data[13]);

    dev->chip_layout[i].sad = sad;
    dev->chip_layout[i].ead = ead;
    dev->chip_layout[i].align = eau;

    if (print) {
      printf("Area %d: 0x%08x:0x%08x (erase 0x%x - write 0x%x)\n", koa, sad, ead, eau, wau);
    }
  }

  return 0;
}

int
ra_get_dev_info(ra_device_t *dev) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[32];
  ssize_t pkt_len, n;

  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), SIG_CMD, NULL, 0, false);
  if (pkt_len < 0)
    return -1;

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  n = ra_recv(dev, resp, 18, 500);
  if (n < 18) {
    warnx("short response for device info");
    return -1;
  }

  /* Parse signature response */
  /* Header(4) + SCI(4) + RMB(4) + NOA(1) + TYP(1) + BFV(2) + Footer(2) */
  uint32_t sci = be_to_uint32(&resp[4]);
  uint32_t rmb = be_to_uint32(&resp[8]);
  uint8_t noa = resp[12];
  uint8_t typ = resp[13];
  uint16_t bfv = ((uint16_t)resp[14] << 8) | resp[15];

  printf("====================\n");
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

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  n = ra_recv(dev, resp, 7, 500);
  if (n < 7) {
    warnx("short response for ID authentication");
    return -1;
  }

  size_t data_len;
  if (unpack_with_error(resp, n, NULL, &data_len, "ID authentication") < 0)
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

  if (size == 0)
    size = dev->chip_layout[dev->sel_area].ead - start;

  if (set_size_boundaries(dev, start, size, &end) < 0)
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

  if (size == 0)
    size = 0x3FFFF - start;

  if (set_size_boundaries(dev, start, size, &end) < 0)
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

  if (ra_send(dev, pkt, pkt_len) < 0) {
    close(fd);
    return -1;
  }

  uint32_t nr_packets = (end - start) / CHUNK_SIZE;
  progress_t prog;
  progress_init(&prog, nr_packets + 1, "Reading");

  for (uint32_t i = 0; i <= nr_packets; i++) {
    n = ra_recv(dev, resp, CHUNK_SIZE + 6, 1000);
    if (n < 7) {
      warnx("short response during read");
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

  if (set_size_boundaries(dev, start, size, &end) < 0) {
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
