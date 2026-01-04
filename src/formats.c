/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Input file format parsers for Intel HEX and Motorola S-record
 *
 * Design goal: small and simple implementation covering common use cases.
 * TODO: Consider using an upstream library (e.g., arkku/ihex, arkku/srec)
 * for full format compliance and edge case handling if needed.
 */

#include "compat.h"
#include "formats.h"
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_LINE_LEN 1024

static int
hex_nibble(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return -1;
}

static int
hex_byte(const char *s) {
  int hi = hex_nibble(s[0]);
  int lo = hex_nibble(s[1]);
  if (hi < 0 || lo < 0)
    return -1;
  return (hi << 4) | lo;
}

static int
ensure_capacity(uint8_t **buf, size_t *capacity, size_t needed) {
  if (needed <= *capacity)
    return 0;

  size_t new_cap = *capacity ? *capacity * 2 : 65536;
  while (new_cap < needed)
    new_cap *= 2;

  uint8_t *new_buf = realloc(*buf, new_cap);
  if (!new_buf) {
    warn("realloc failed");
    return -1;
  }

  memset(new_buf + *capacity, 0xFF, new_cap - *capacity);
  *buf = new_buf;
  *capacity = new_cap;
  return 0;
}

input_format_t
format_detect(const char *filename) {
  const char *ext = strrchr(filename, '.');
  if (!ext)
    return FORMAT_BIN;

  ext++;
  if (strcasecmp(ext, "hex") == 0 || strcasecmp(ext, "ihex") == 0)
    return FORMAT_IHEX;
  if (strcasecmp(ext, "srec") == 0 || strcasecmp(ext, "s19") == 0 || strcasecmp(ext, "s28") == 0 ||
      strcasecmp(ext, "s37") == 0 || strcasecmp(ext, "mot") == 0)
    return FORMAT_SREC;

  return FORMAT_BIN;
}

const char *
format_name(input_format_t fmt) {
  switch (fmt) {
  case FORMAT_AUTO:
    return "auto";
  case FORMAT_BIN:
    return "binary";
  case FORMAT_IHEX:
    return "Intel HEX";
  case FORMAT_SREC:
    return "Motorola S-record";
  default:
    return "unknown";
  }
}

int
bin_parse(const char *filename, parsed_file_t *out) {
  int fd;
  struct stat st;

  fd = open(filename, O_RDONLY);
  if (fd < 0) {
    warn("failed to open %s", filename);
    return -1;
  }

  if (fstat(fd, &st) < 0) {
    warn("failed to stat %s", filename);
    close(fd);
    return -1;
  }

  out->size = (size_t)st.st_size;
  out->base_addr = 0;
  out->has_addr = 0;
  out->data = malloc(out->size);
  if (!out->data) {
    warn("malloc failed");
    close(fd);
    return -1;
  }

  ssize_t n = read(fd, out->data, out->size);
  close(fd);

  if (n < 0 || (size_t)n != out->size) {
    warn("read failed");
    free(out->data);
    out->data = NULL;
    return -1;
  }

  return 0;
}

int
ihex_parse(const char *filename, parsed_file_t *out) {
  FILE *fp;
  char line[MAX_LINE_LEN];
  uint8_t *buf = NULL;
  size_t capacity = 0;
  uint32_t ext_addr = 0;
  uint32_t min_addr = UINT32_MAX;
  uint32_t max_addr = 0;
  int line_num = 0;
  int eof_seen = 0;

  fp = fopen(filename, "r");
  if (!fp) {
    warn("failed to open %s", filename);
    return -1;
  }

  while (fgets(line, sizeof(line), fp)) {
    line_num++;
    char *p = line;

    while (isspace(*p))
      p++;
    if (*p == '\0' || *p == '\n')
      continue;

    if (*p != ':') {
      warnx("%s:%d: expected ':' at start of line", filename, line_num);
      goto fail;
    }
    p++;

    size_t len = strlen(p);
    while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r'))
      len--;

    if (len < 10) {
      warnx("%s:%d: line too short", filename, line_num);
      goto fail;
    }

    int byte_count = hex_byte(p);
    int addr_hi = hex_byte(p + 2);
    int addr_lo = hex_byte(p + 4);
    int rec_type = hex_byte(p + 6);

    if (byte_count < 0 || addr_hi < 0 || addr_lo < 0 || rec_type < 0) {
      warnx("%s:%d: invalid hex digits", filename, line_num);
      goto fail;
    }

    uint16_t addr = (addr_hi << 8) | addr_lo;
    size_t expected_len = 8 + byte_count * 2 + 2;
    if (len < expected_len) {
      warnx("%s:%d: line too short for byte count", filename, line_num);
      goto fail;
    }

    uint8_t checksum = byte_count + addr_hi + addr_lo + rec_type;
    uint8_t data[256];
    for (int i = 0; i < byte_count; i++) {
      int b = hex_byte(p + 8 + i * 2);
      if (b < 0) {
        warnx("%s:%d: invalid hex digits in data", filename, line_num);
        goto fail;
      }
      data[i] = b;
      checksum += b;
    }

    int file_checksum = hex_byte(p + 8 + byte_count * 2);
    if (file_checksum < 0) {
      warnx("%s:%d: invalid checksum hex", filename, line_num);
      goto fail;
    }
    checksum += file_checksum;
    if (checksum != 0) {
      warnx("%s:%d: checksum mismatch", filename, line_num);
      goto fail;
    }

    switch (rec_type) {
    case 0x00: /* Data record */
    {
      uint32_t full_addr = ext_addr + addr;
      if (full_addr < min_addr)
        min_addr = full_addr;
      if (full_addr + byte_count > max_addr)
        max_addr = full_addr + byte_count;

      size_t offset = full_addr - (min_addr == UINT32_MAX ? full_addr : min_addr);
      if (min_addr == UINT32_MAX) {
        min_addr = full_addr;
        offset = 0;
      }
      offset = full_addr - min_addr;

      if (ensure_capacity(&buf, &capacity, offset + byte_count) < 0)
        goto fail;

      memcpy(buf + offset, data, byte_count);
      break;
    }
    case 0x01: /* End of file */
      eof_seen = 1;
      break;
    case 0x02: /* Extended segment address */
      if (byte_count != 2) {
        warnx("%s:%d: invalid extended segment address record", filename, line_num);
        goto fail;
      }
      ext_addr = ((uint32_t)data[0] << 8 | data[1]) << 4;
      break;
    case 0x04: /* Extended linear address */
      if (byte_count != 2) {
        warnx("%s:%d: invalid extended linear address record", filename, line_num);
        goto fail;
      }
      ext_addr = ((uint32_t)data[0] << 8 | data[1]) << 16;
      break;
    case 0x03: /* Start segment address - ignored */
    case 0x05: /* Start linear address - ignored */
      break;
    default:
      warnx("%s:%d: unknown record type 0x%02X", filename, line_num, rec_type);
      goto fail;
    }
  }

  fclose(fp);

  if (!eof_seen) {
    warnx("%s: no EOF record found", filename);
    free(buf);
    return -1;
  }

  if (min_addr == UINT32_MAX) {
    warnx("%s: no data records found", filename);
    free(buf);
    return -1;
  }

  out->data = buf;
  out->size = max_addr - min_addr;
  out->base_addr = min_addr;
  out->has_addr = 1;

  return 0;

fail:
  fclose(fp);
  free(buf);
  return -1;
}

int
srec_parse(const char *filename, parsed_file_t *out) {
  FILE *fp;
  char line[MAX_LINE_LEN];
  uint8_t *buf = NULL;
  size_t capacity = 0;
  uint32_t min_addr = UINT32_MAX;
  uint32_t max_addr = 0;
  int line_num = 0;
  int eof_seen = 0;

  fp = fopen(filename, "r");
  if (!fp) {
    warn("failed to open %s", filename);
    return -1;
  }

  while (fgets(line, sizeof(line), fp)) {
    line_num++;
    char *p = line;

    while (isspace(*p))
      p++;
    if (*p == '\0' || *p == '\n')
      continue;

    if (*p != 'S' && *p != 's') {
      warnx("%s:%d: expected 'S' at start of line", filename, line_num);
      goto fail;
    }
    p++;

    if (!isdigit(*p)) {
      warnx("%s:%d: expected digit after 'S'", filename, line_num);
      goto fail;
    }
    int rec_type = *p - '0';
    p++;

    size_t len = strlen(p);
    while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r'))
      len--;

    if (len < 4) {
      warnx("%s:%d: line too short", filename, line_num);
      goto fail;
    }

    int byte_count = hex_byte(p);
    if (byte_count < 0) {
      warnx("%s:%d: invalid byte count", filename, line_num);
      goto fail;
    }

    size_t expected_len = 2 + byte_count * 2;
    if (len < expected_len) {
      warnx("%s:%d: line too short for byte count", filename, line_num);
      goto fail;
    }

    int addr_bytes;
    switch (rec_type) {
    case 0:
    case 1:
    case 5:
    case 9:
      addr_bytes = 2;
      break;
    case 2:
    case 8:
      addr_bytes = 3;
      break;
    case 3:
    case 7:
      addr_bytes = 4;
      break;
    default:
      warnx("%s:%d: unknown record type S%d", filename, line_num, rec_type);
      goto fail;
    }

    if (byte_count < addr_bytes + 1) {
      warnx("%s:%d: byte count too small", filename, line_num);
      goto fail;
    }

    uint8_t checksum = byte_count;
    uint32_t addr = 0;
    for (int i = 0; i < addr_bytes; i++) {
      int b = hex_byte(p + 2 + i * 2);
      if (b < 0) {
        warnx("%s:%d: invalid address hex", filename, line_num);
        goto fail;
      }
      addr = (addr << 8) | b;
      checksum += b;
    }

    int data_bytes = byte_count - addr_bytes - 1;
    uint8_t data[256];
    for (int i = 0; i < data_bytes; i++) {
      int b = hex_byte(p + 2 + addr_bytes * 2 + i * 2);
      if (b < 0) {
        warnx("%s:%d: invalid data hex", filename, line_num);
        goto fail;
      }
      data[i] = b;
      checksum += b;
    }

    int file_checksum = hex_byte(p + 2 + addr_bytes * 2 + data_bytes * 2);
    if (file_checksum < 0) {
      warnx("%s:%d: invalid checksum hex", filename, line_num);
      goto fail;
    }
    checksum += file_checksum;
    if (checksum != 0xFF) {
      warnx("%s:%d: checksum mismatch", filename, line_num);
      goto fail;
    }

    switch (rec_type) {
    case 1:
    case 2:
    case 3: /* Data records */
    {
      if (addr < min_addr)
        min_addr = addr;
      if (addr + data_bytes > max_addr)
        max_addr = addr + data_bytes;

      size_t offset;
      if (min_addr == UINT32_MAX) {
        min_addr = addr;
        offset = 0;
      } else {
        offset = addr - min_addr;
      }

      if (ensure_capacity(&buf, &capacity, offset + data_bytes) < 0)
        goto fail;

      memcpy(buf + offset, data, data_bytes);
      break;
    }
    case 7:
    case 8:
    case 9: /* End records */
      eof_seen = 1;
      break;
    case 0: /* Header - ignored */
    case 5: /* Record count - ignored */
      break;
    }
  }

  fclose(fp);

  if (!eof_seen) {
    warnx("%s: no end record found", filename);
    free(buf);
    return -1;
  }

  if (min_addr == UINT32_MAX) {
    warnx("%s: no data records found", filename);
    free(buf);
    return -1;
  }

  out->data = buf;
  out->size = max_addr - min_addr;
  out->base_addr = min_addr;
  out->has_addr = 1;

  return 0;

fail:
  fclose(fp);
  free(buf);
  return -1;
}

int
format_parse(const char *filename, input_format_t format, parsed_file_t *out) {
  if (format == FORMAT_AUTO)
    format = format_detect(filename);

  memset(out, 0, sizeof(*out));

  switch (format) {
  case FORMAT_BIN:
    return bin_parse(filename, out);
  case FORMAT_IHEX:
    return ihex_parse(filename, out);
  case FORMAT_SREC:
    return srec_parse(filename, out);
  default:
    warnx("unknown format");
    return -1;
  }
}

/*
 * Intel HEX encoder
 */

#define IHEX_BYTES_PER_LINE 16

int
ihex_write(const char *filename, const uint8_t *data, size_t size, uint32_t addr) {
  FILE *fp = fopen(filename, "w");
  if (!fp) {
    warn("failed to open %s", filename);
    return -1;
  }

  uint32_t current_ext_addr = 0;
  size_t offset = 0;

  while (offset < size) {
    uint32_t line_addr = addr + (uint32_t)offset;

    /* Emit extended linear address record if needed (type 04) */
    uint32_t ext_addr = line_addr >> 16;
    if (ext_addr != current_ext_addr) {
      uint8_t sum = 0x02 + 0x00 + 0x00 + 0x04 + (ext_addr >> 8) + (ext_addr & 0xFF);
      fprintf(fp, ":02000004%04X%02X\n", ext_addr, (uint8_t)(~sum + 1));
      current_ext_addr = ext_addr;
    }

    /* Data record (type 00) */
    size_t remaining = size - offset;
    size_t line_len = remaining < IHEX_BYTES_PER_LINE ? remaining : IHEX_BYTES_PER_LINE;
    uint16_t rec_addr = line_addr & 0xFFFF;

    uint8_t sum = (uint8_t)line_len + (rec_addr >> 8) + (rec_addr & 0xFF) + 0x00;
    fprintf(fp, ":%02X%04X00", (unsigned)line_len, rec_addr);

    for (size_t i = 0; i < line_len; i++) {
      fprintf(fp, "%02X", data[offset + i]);
      sum += data[offset + i];
    }

    fprintf(fp, "%02X\n", (uint8_t)(~sum + 1));
    offset += line_len;
  }

  /* EOF record (type 01) */
  fprintf(fp, ":00000001FF\n");

  fclose(fp);
  return 0;
}

/*
 * Motorola S-record encoder
 */

#define SREC_BYTES_PER_LINE 16

int
srec_write(const char *filename, const uint8_t *data, size_t size, uint32_t addr) {
  FILE *fp = fopen(filename, "w");
  if (!fp) {
    warn("failed to open %s", filename);
    return -1;
  }

  /* S0 header record */
  const char *hdr = "HDR";
  size_t hdr_len = strlen(hdr);
  uint8_t sum = (uint8_t)(hdr_len + 3); /* byte count includes address (2) + data + checksum */
  fprintf(fp, "S0%02X0000", (unsigned)(hdr_len + 3));
  for (size_t i = 0; i < hdr_len; i++) {
    fprintf(fp, "%02X", (uint8_t)hdr[i]);
    sum += (uint8_t)hdr[i];
  }
  fprintf(fp, "%02X\n", (uint8_t)(~sum));

  /* S3 data records (32-bit address) */
  size_t offset = 0;
  while (offset < size) {
    uint32_t line_addr = addr + (uint32_t)offset;
    size_t remaining = size - offset;
    size_t line_len = remaining < SREC_BYTES_PER_LINE ? remaining : SREC_BYTES_PER_LINE;

    /* Byte count = address (4) + data + checksum (1) */
    uint8_t byte_count = (uint8_t)(4 + line_len + 1);
    sum = byte_count;
    sum += (line_addr >> 24) & 0xFF;
    sum += (line_addr >> 16) & 0xFF;
    sum += (line_addr >> 8) & 0xFF;
    sum += line_addr & 0xFF;

    fprintf(fp, "S3%02X%08X", byte_count, line_addr);

    for (size_t i = 0; i < line_len; i++) {
      fprintf(fp, "%02X", data[offset + i]);
      sum += data[offset + i];
    }

    fprintf(fp, "%02X\n", (uint8_t)(~sum));
    offset += line_len;
  }

  /* S7 end record (32-bit start address) */
  sum = 0x05 + ((addr >> 24) & 0xFF) + ((addr >> 16) & 0xFF) + ((addr >> 8) & 0xFF) + (addr & 0xFF);
  fprintf(fp, "S705%08X%02X\n", addr, (uint8_t)(~sum));

  fclose(fp);
  return 0;
}

int
format_write(
    const char *filename, output_format_t format, const uint8_t *data, size_t size, uint32_t addr) {
  if (format == FORMAT_AUTO)
    format = format_detect(filename);

  switch (format) {
  case FORMAT_BIN: {
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
      warn("failed to open %s", filename);
      return -1;
    }
    if (fwrite(data, 1, size, fp) != size) {
      warn("failed to write %s", filename);
      fclose(fp);
      return -1;
    }
    fclose(fp);
    return 0;
  }
  case FORMAT_IHEX:
    return ihex_write(filename, data, size, addr);
  case FORMAT_SREC:
    return srec_write(filename, data, size, addr);
  default:
    warnx("unknown output format");
    return -1;
  }
}
