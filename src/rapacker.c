/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Protocol packet encoding/decoding for Renesas RA bootloader
 */

#include "rapacker.h"
#include <errno.h>
#include <string.h>

/* MCU error codes with descriptive messages */
static const struct {
  uint8_t code;
  const char *name;
  const char *desc;
} error_codes[] = {
  { 0x0C, "ERR_UNSU", "unsupported command"         },
  { 0xC1, "ERR_PCKT", "packet error (length/ETX)"   },
  { 0xC2, "ERR_CHKS", "checksum mismatch"           },
  { 0xC3, "ERR_FLOW", "command flow error"          },
  { 0xD0, "ERR_ADDR", "invalid address"             },
  { 0xD4, "ERR_BAUD", "baud rate margin error"      },
  { 0xDA, "ERR_PROT", "protection error"            },
  { 0xDB, "ERR_ID",   "ID authentication mismatch"  },
  { 0xDC, "ERR_SERI", "serial programming disabled" },
  { 0xE1, "ERR_ERA",  "erase failed"                },
  { 0xE2, "ERR_WRI",  "write failed"                },
  { 0xE7, "ERR_SEQ",  "sequencer error"             },
  { 0,    NULL,       NULL                          }
};

const char *
ra_strerror(uint8_t code) {
  for (size_t i = 0; error_codes[i].name != NULL; i++) {
    if (error_codes[i].code == code)
      return error_codes[i].name;
  }
  return "ERR_UNKNOWN";
}

const char *
ra_strdesc(uint8_t code) {
  for (size_t i = 0; error_codes[i].name != NULL; i++) {
    if (error_codes[i].code == code)
      return error_codes[i].desc;
  }
  return "unknown error";
}

uint8_t
ra_calc_sum(uint8_t cmd, const uint8_t *data, size_t len) {
  uint16_t pkt_len = len + 1;
  uint8_t lnh = (pkt_len >> 8) & 0xFF;
  uint8_t lnl = pkt_len & 0xFF;
  uint32_t sum = lnh + lnl + cmd;

  for (size_t i = 0; i < len; i++)
    sum += data[i];

  /* Two's complement */
  return (~(sum - 1)) & 0xFF;
}

ssize_t
ra_pack_pkt(uint8_t *buf, size_t buflen, uint8_t cmd, const uint8_t *data, size_t len, bool ack) {
  if (len > MAX_DATA_LEN) {
    errno = EINVAL;
    return -1;
  }

  size_t pkt_len = len + 6; /* SOD + LNH + LNL + CMD + data + SUM + ETX */
  if (buflen < pkt_len) {
    errno = ENOBUFS;
    return -1;
  }

  uint16_t data_len = len + 1; /* includes CMD in length */
  uint8_t lnh = (data_len >> 8) & 0xFF;
  uint8_t lnl = data_len & 0xFF;
  uint8_t sum = ra_calc_sum(cmd, data, len);

  buf[0] = ack ? SOD_ACK : SOD_CMD;
  buf[1] = lnh;
  buf[2] = lnl;
  buf[3] = cmd;
  if (len > 0 && data != NULL)
    memcpy(&buf[4], data, len);
  buf[4 + len] = sum;
  buf[5 + len] = ETX;

  return (ssize_t)pkt_len;
}

ssize_t
ra_unpack_pkt(const uint8_t *buf, size_t buflen, uint8_t *data, size_t *data_len, uint8_t *cmd) {
  if (buflen < 6) {
    errno = EINVAL;
    return -1;
  }

  uint8_t sod = buf[0];
  if (sod != SOD_ACK) {
    errno = EPROTO;
    return -1;
  }

  uint8_t lnh = buf[1];
  uint8_t lnl = buf[2];
  uint8_t res = buf[3];
  uint16_t pkt_len = ((uint16_t)lnh << 8) | lnl;

  if (pkt_len < 1) {
    errno = EPROTO;
    return -1;
  }

  size_t dlen = pkt_len - 1;
  size_t total_len = 4 + dlen + 2; /* header + data + footer */

  if (buflen < total_len) {
    errno = EINVAL;
    return -1;
  }

  /* Check for MCU error */
  if (res & STATUS_ERR) {
    if (dlen > 0 && data != NULL && data_len != NULL) {
      *data_len = dlen;
      memcpy(data, &buf[4], dlen);
    }
    errno = EIO;
    return -1;
  }

  uint8_t sum = buf[4 + dlen];
  uint8_t etx = buf[5 + dlen];

  if (etx != ETX) {
    errno = EPROTO;
    return -1;
  }

  /* Verify checksum */
  uint8_t calc = ra_calc_sum(res, &buf[4], dlen);
  if (sum != calc) {
    errno = EBADMSG;
    return -1;
  }

  if (data != NULL && dlen > 0)
    memcpy(data, &buf[4], dlen);
  if (data_len != NULL)
    *data_len = dlen;
  if (cmd != NULL)
    *cmd = res;

  return (ssize_t)dlen;
}
