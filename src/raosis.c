/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * OSIS (OCD/Serial Programmer ID Setting Register) handling
 */

#include "raosis.h"
#include "rapacker.h"

#include <err.h>
#include <stdio.h>
#include <string.h>

/*
 * OSIS addresses for reading (must match config area layout)
 */
static const uint32_t osis_addrs[4] = {
  OSIS0_ADDR,
  OSIS1_ADDR,
  OSIS2_ADDR,
  OSIS3_ADDR,
};

/*
 * Read a single 4-byte word from flash
 */
static int
read_word(ra_device_t *dev, uint32_t addr, uint32_t *value) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[16];
  uint8_t data[8];
  uint8_t chunk[8];
  ssize_t pkt_len, n;
  size_t data_len;

  /* Read 4 bytes: start=addr, end=addr+3 */
  uint32_to_be(addr, &data[0]);
  uint32_to_be(addr + OSIS_WORD_SIZE - 1, &data[4]);

  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), REA_CMD, data, 8, false);
  if (pkt_len < 0)
    return -1;

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  n = ra_recv(dev, resp, 12, 500);
  if (n < 10) {
    warnx("short response reading OSIS at 0x%08x", addr);
    return -1;
  }

  uint8_t cmd;
  if (ra_unpack_pkt(resp, n, chunk, &data_len, &cmd) < 0) {
    if (cmd & STATUS_ERR) {
      uint8_t err_code = cmd & 0x7F;
      warnx("read OSIS 0x%08x: MCU error 0x%02X (%s)",
          addr, err_code, ra_strerror(err_code));
    }
    return -1;
  }

  if (data_len < OSIS_WORD_SIZE) {
    warnx("incomplete OSIS word at 0x%08x (got %zu bytes)", addr, data_len);
    return -1;
  }

  /* OSIS is stored little-endian in flash */
  *value = ((uint32_t)chunk[3] << 24) | ((uint32_t)chunk[2] << 16) |
           ((uint32_t)chunk[1] << 8) | (uint32_t)chunk[0];

  /* Send ACK to complete read transaction */
  uint8_t ack_data[1] = { 0x00 };
  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), REA_CMD, ack_data, 1, true);
  if (pkt_len > 0)
    ra_send(dev, pkt, pkt_len);

  return 0;
}

int
ra_osis_read(ra_device_t *dev, osis_t *osis) {
  memset(osis, 0, sizeof(*osis));

  /* Check if config area is available (area 3) */
  if (dev->chip_layout[3].ead == 0) {
    warnx("config area not available on this device");
    return -1;
  }

  /* Read each OSIS word */
  for (int i = 0; i < 4; i++) {
    if (read_word(dev, osis_addrs[i], &osis->word[i]) < 0) {
      warnx("failed to read OSIS%d at 0x%08x", i, osis_addrs[i]);
      return -1;
    }
  }

  /* Decode security mode from OSIS3 bits [31:30] (which are ID[127:126]) */
  uint8_t mode_bits = (osis->word[3] >> 30) & 0x03;
  osis->mode = (osis_mode_t)mode_bits;

  return 0;
}

const char *
ra_osis_mode_str(osis_mode_t mode) {
  switch (mode) {
  case OSIS_MODE_DISABLED:
    return "Disabled (serial programming blocked)";
  case OSIS_MODE_LOCKED:
    return "Locked (ALeRASE disabled)";
  case OSIS_MODE_LOCKED_WITH_ERASE:
    return "Locked with All Erase (ALeRASE works)";
  case OSIS_MODE_UNLOCKED:
    return "Unlocked (no protection)";
  default:
    return "Unknown";
  }
}

void
ra_osis_print(const osis_t *osis) {
  printf("OSIS (ID Code Protection):\n");
  printf("  OSIS0 [0x%08x]: 0x%08x\n", OSIS0_ADDR, osis->word[0]);
  printf("  OSIS1 [0x%08x]: 0x%08x\n", OSIS1_ADDR, osis->word[1]);
  printf("  OSIS2 [0x%08x]: 0x%08x\n", OSIS2_ADDR, osis->word[2]);
  printf("  OSIS3 [0x%08x]: 0x%08x\n", OSIS3_ADDR, osis->word[3]);
  printf("  Mode [127:126]=%d%db: %s\n",
      (osis->word[3] >> 31) & 1,
      (osis->word[3] >> 30) & 1,
      ra_osis_mode_str(osis->mode));

  /* Show full 128-bit ID code in hex */
  printf("  ID Code: %08x%08x%08x%08x\n",
      osis->word[3], osis->word[2], osis->word[1], osis->word[0]);
}
