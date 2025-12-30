/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * RADFU - Renesas RA Device Firmware Update tool
 */

#include "raconnect.h"
#include "radfu.h"

#include <err.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VERSION "0.0.1"

static void
usage(int status) {
  FILE *out = status == EXIT_SUCCESS ? stdout : stderr;

  fprintf(out,
      "Usage: radfu <command> [options] [file]\n"
      "\n"
      "Commands:\n"
      "  info                 Show device and memory information\n"
      "  read  <file>         Read flash memory to file\n"
      "  write <file>         Write file to flash memory\n"
      "  erase                Erase flash sectors\n"
      "\n"
      "Options:\n"
      "  -p, --port <dev>     Serial port (auto-detect if omitted)\n"
      "  -a, --address <hex>  Start address (default: 0x0)\n"
      "  -s, --size <hex>     Size in bytes\n"
      "  -b, --baudrate <n>   Set UART baud rate (default: 9600)\n"
      "  -i, --id <hex>       ID code for authentication (32 hex chars)\n"
      "  -e, --erase-all      Erase all areas using ALeRASE magic ID\n"
      "  -v, --verify         Verify after write\n"
      "  -h, --help           Show this help message\n"
      "  -V, --version        Show version\n"
      "\n"
      "Examples:\n"
      "  radfu info\n"
      "  radfu read -a 0x0 -s 0x10000 firmware.bin\n"
      "  radfu write -b 1000000 -a 0x0 -v firmware.bin\n"
      "  radfu erase -a 0x0 -s 0x10000\n");
  exit(status);
}

static void
version(void) {
  printf("radfu version %s\n", VERSION);
  printf("Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025\n");
  printf("License: AGPL-3.0-or-later\n");
  exit(EXIT_SUCCESS);
}

static uint32_t
parse_hex(const char *str) {
  char *endptr;
  unsigned long val;

  val = strtoul(str, &endptr, 16);
  if (*endptr != '\0')
    errx(EXIT_FAILURE, "invalid hex value: %s", str);

  return (uint32_t)val;
}

#define ID_CODE_LEN 16

/* Magic ID code for total area erasure: "ALeRASE" + 0xFF padding */
static const uint8_t ALERASE_ID[ID_CODE_LEN] = {
  'A', 'L', 'e', 'R', 'A', 'S', 'E', 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

static int
parse_id_code(const char *str, uint8_t *id_code) {
  size_t len = strlen(str);

  /* Accept with or without 0x prefix */
  if (len >= 2 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
    str += 2;
    len -= 2;
  }

  if (len != ID_CODE_LEN * 2) {
    warnx("ID code must be %d hex bytes (%d hex characters)", ID_CODE_LEN, ID_CODE_LEN * 2);
    return -1;
  }

  for (int i = 0; i < ID_CODE_LEN; i++) {
    unsigned int byte;
    if (sscanf(str + i * 2, "%2x", &byte) != 1) {
      warnx("invalid hex character in ID code at position %d", i * 2);
      return -1;
    }
    id_code[i] = (uint8_t)byte;
  }

  return 0;
}

enum command {
  CMD_NONE,
  CMD_INFO,
  CMD_READ,
  CMD_WRITE,
  CMD_ERASE,
};

static const struct option longopts[] = {
  { "port",      required_argument, NULL, 'p' },
  { "address",   required_argument, NULL, 'a' },
  { "size",      required_argument, NULL, 's' },
  { "baudrate",  required_argument, NULL, 'b' },
  { "id",        required_argument, NULL, 'i' },
  { "erase-all", no_argument,       NULL, 'e' },
  { "verify",    no_argument,       NULL, 'v' },
  { "help",      no_argument,       NULL, 'h' },
  { "version",   no_argument,       NULL, 'V' },
  { NULL,        0,                 NULL, 0   }
};

int
main(int argc, char *argv[]) {
  const char *port = NULL;
  const char *file = NULL;
  const char *id_str = NULL;
  uint8_t id_code[ID_CODE_LEN];
  uint32_t address = 0;
  uint32_t size = 0;
  uint32_t baudrate = 0;
  bool verify = false;
  bool use_auth = false;
  bool erase_all = false;
  enum command cmd = CMD_NONE;
  int opt;

  while ((opt = getopt_long(argc, argv, "p:a:s:b:i:evhV", longopts, NULL)) != -1) {
    switch (opt) {
    case 'p':
      port = optarg;
      break;
    case 'a':
      address = parse_hex(optarg);
      break;
    case 's':
      size = parse_hex(optarg);
      break;
    case 'b':
      baudrate = (uint32_t)strtoul(optarg, NULL, 10);
      break;
    case 'i':
      id_str = optarg;
      break;
    case 'e':
      erase_all = true;
      break;
    case 'v':
      verify = true;
      break;
    case 'h':
      usage(EXIT_SUCCESS);
      break;
    case 'V':
      version();
      break;
    default:
      usage(EXIT_FAILURE);
    }
  }

  /* Handle ID code: either from --id or --erase-all */
  if (erase_all) {
    if (id_str != NULL) {
      warnx("--erase-all and --id are mutually exclusive");
      usage(EXIT_FAILURE);
    }
    memcpy(id_code, ALERASE_ID, ID_CODE_LEN);
    use_auth = true;
  } else if (id_str != NULL) {
    if (parse_id_code(id_str, id_code) < 0)
      errx(EXIT_FAILURE, "invalid ID code format");
    use_auth = true;
  }

  if (optind >= argc)
    usage(EXIT_FAILURE);

  const char *command = argv[optind++];

  if (strcmp(command, "info") == 0) {
    cmd = CMD_INFO;
  } else if (strcmp(command, "read") == 0) {
    cmd = CMD_READ;
    if (optind >= argc)
      errx(EXIT_FAILURE, "read command requires a file argument");
    file = argv[optind];
  } else if (strcmp(command, "write") == 0) {
    cmd = CMD_WRITE;
    if (optind >= argc)
      errx(EXIT_FAILURE, "write command requires a file argument");
    file = argv[optind];
  } else if (strcmp(command, "erase") == 0) {
    cmd = CMD_ERASE;
  } else {
    errx(EXIT_FAILURE, "unknown command: %s", command);
  }

  ra_device_t dev;
  ra_dev_init(&dev);

  if (ra_open(&dev, port) < 0)
    errx(EXIT_FAILURE, "failed to connect to device");

  if (ra_get_area_info(&dev, false) < 0) {
    ra_close(&dev);
    errx(EXIT_FAILURE, "failed to get area info");
  }

  /* Set baud rate if requested */
  if (baudrate > 0 && baudrate != 9600) {
    if (ra_set_baudrate(&dev, baudrate) < 0) {
      ra_close(&dev);
      errx(EXIT_FAILURE, "failed to set baud rate");
    }
  }

  /* Perform ID authentication if requested */
  if (use_auth) {
    if (ra_authenticate(&dev, id_code) < 0) {
      ra_close(&dev);
      errx(EXIT_FAILURE, "ID authentication failed");
    }
  }

  int ret = 0;
  switch (cmd) {
  case CMD_INFO:
    ret = ra_get_dev_info(&dev);
    if (ret == 0)
      ra_get_area_info(&dev, true);
    break;
  case CMD_READ:
    ret = ra_read(&dev, file, address, size);
    break;
  case CMD_WRITE:
    ret = ra_write(&dev, file, address, size, verify);
    break;
  case CMD_ERASE:
    ret = ra_erase(&dev, address, size);
    break;
  default:
    break;
  }

  ra_close(&dev);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
