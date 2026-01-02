/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * RADFU - Renesas RA Device Firmware Update tool
 */

#include "raconnect.h"
#include "radfu.h"
#include "raosis.h"

#include <err.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef VERSION
#warning "VERSION not defined, using dummy-version"
#define VERSION "dummy-version"
#endif

static void
usage(int status) {
  FILE *out = status == EXIT_SUCCESS ? stdout : stderr;

  fprintf(out,
      "Usage: radfu <command> [options] [file]\n"
      "\n"
      "Commands:\n"
      "  info           Show device and memory information\n"
      "  read <file>    Read flash memory to file\n"
      "  write <file>   Write file to flash memory\n"
      "  erase          Erase flash sectors\n"
      "  crc            Calculate CRC-32 of flash region\n"
      "  dlm            Show Device Lifecycle Management state\n"
      "  dlm-transit <state>  Transition DLM state (ssd/nsecsd/dpl/lck_dbg/lck_boot)\n"
      "  boundary       Show secure/non-secure boundary settings\n"
      "  boundary-set   Set TrustZone boundaries (requires --cfs1/--cfs2/--dfs/--srs1/--srs2)\n"
      "  param          Show device parameter (initialization command)\n"
      "  param-set <enable|disable>  Enable/disable initialization command\n"
      "  init           Initialize device (factory reset to SSD state)\n"
      "  osis           Show OSIS (ID code protection) status\n"
      "  key-set <idx> <file>    Inject wrapped key from file at index\n"
      "  key-verify <idx>        Verify key at index\n"
      "  ukey-set <idx> <file>   Inject user wrapped key from file at index\n"
      "  ukey-verify <idx>       Verify user key at index\n"
      "\n"
      "Options:\n"
      "  -p, --port <dev>     Serial port (auto-detect if omitted)\n"
      "  -a, --address <hex>  Start address (default: 0x0)\n"
      "  -s, --size <hex>     Size in bytes\n"
      "  -b, --baudrate <n>   Set UART baud rate (default: 9600)\n"
      "  -i, --id <hex>       ID code for authentication (32 hex chars)\n"
      "  -e, --erase-all      Erase all areas using ALeRASE magic ID\n"
      "  -v, --verify         Verify after write\n"
      "  -u, --uart           Use plain UART mode (P109/P110 pins)\n"
      "      --cfs1 <KB>      Code flash secure region size without NSC\n"
      "      --cfs2 <KB>      Code flash secure region size (total)\n"
      "      --dfs <KB>       Data flash secure region size\n"
      "      --srs1 <KB>      SRAM secure region size without NSC\n"
      "      --srs2 <KB>      SRAM secure region size (total)\n"
      "  -h, --help           Show this help message\n"
      "  -V, --version        Show version\n"
      "\n"
      "Examples:\n"
      "  radfu info\n"
      "  radfu read -a 0x0 -s 0x10000 firmware.bin\n"
      "  radfu write -b 1000000 -a 0x0 -v firmware.bin\n"
      "  radfu erase -a 0x0 -s 0x10000\n"
      "  radfu crc -a 0x0 -s 0x10000\n"
      "  radfu osis\n"
      "  radfu boundary-set --cfs1 0 --cfs2 0 --dfs 0 --srs1 0 --srs2 0\n"
      "  radfu -u -p /dev/ttyUSB0 info    # UART via USB-UART adapter\n");
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
  CMD_CRC,
  CMD_DLM,
  CMD_DLM_TRANSIT,
  CMD_BOUNDARY,
  CMD_BOUNDARY_SET,
  CMD_PARAM,
  CMD_PARAM_SET,
  CMD_INIT,
  CMD_OSIS,
  CMD_KEY_SET,
  CMD_KEY_VERIFY,
  CMD_UKEY_SET,
  CMD_UKEY_VERIFY,
};

/* Long-only options use values >= 256 */
#define OPT_CFS1 256
#define OPT_CFS2 257
#define OPT_DFS 258
#define OPT_SRS1 259
#define OPT_SRS2 260

static const struct option longopts[] = {
  { "port",      required_argument, NULL, 'p'      },
  { "address",   required_argument, NULL, 'a'      },
  { "size",      required_argument, NULL, 's'      },
  { "baudrate",  required_argument, NULL, 'b'      },
  { "id",        required_argument, NULL, 'i'      },
  { "erase-all", no_argument,       NULL, 'e'      },
  { "verify",    no_argument,       NULL, 'v'      },
  { "uart",      no_argument,       NULL, 'u'      },
  { "cfs1",      required_argument, NULL, OPT_CFS1 },
  { "cfs2",      required_argument, NULL, OPT_CFS2 },
  { "dfs",       required_argument, NULL, OPT_DFS  },
  { "srs1",      required_argument, NULL, OPT_SRS1 },
  { "srs2",      required_argument, NULL, OPT_SRS2 },
  { "help",      no_argument,       NULL, 'h'      },
  { "version",   no_argument,       NULL, 'V'      },
  { NULL,        0,                 NULL, 0        }
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
  bool uart_mode = false;
  uint8_t dest_dlm = 0;
  uint8_t param_value = 0;
  uint8_t key_index = 0;
  const char *key_file = NULL;
  ra_boundary_t bnd = { 0 };
  bool bnd_cfs1_set = false, bnd_cfs2_set = false, bnd_dfs_set = false;
  bool bnd_srs1_set = false, bnd_srs2_set = false;
  enum command cmd = CMD_NONE;
  int opt;

  while ((opt = getopt_long(argc, argv, "p:a:s:b:i:evuhV", longopts, NULL)) != -1) {
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
    case 'u':
      uart_mode = true;
      break;
    case OPT_CFS1:
      bnd.cfs1 = (uint16_t)strtoul(optarg, NULL, 10);
      bnd_cfs1_set = true;
      break;
    case OPT_CFS2:
      bnd.cfs2 = (uint16_t)strtoul(optarg, NULL, 10);
      bnd_cfs2_set = true;
      break;
    case OPT_DFS:
      bnd.dfs = (uint16_t)strtoul(optarg, NULL, 10);
      bnd_dfs_set = true;
      break;
    case OPT_SRS1:
      bnd.srs1 = (uint16_t)strtoul(optarg, NULL, 10);
      bnd_srs1_set = true;
      break;
    case OPT_SRS2:
      bnd.srs2 = (uint16_t)strtoul(optarg, NULL, 10);
      bnd_srs2_set = true;
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
    warnx("note: ALeRASE requires OSIS[127:126]=10b (Locked with All Erase support)");
    warnx("      will fail if device has OSIS[127:126]=01b (Locked mode)");
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
  } else if (strcmp(command, "crc") == 0) {
    cmd = CMD_CRC;
  } else if (strcmp(command, "dlm") == 0) {
    cmd = CMD_DLM;
  } else if (strcmp(command, "dlm-transit") == 0) {
    cmd = CMD_DLM_TRANSIT;
    if (optind >= argc)
      errx(EXIT_FAILURE, "dlm-transit requires a state argument (ssd/nsecsd/dpl/lck_dbg/lck_boot)");
    const char *state = argv[optind];
    if (strcasecmp(state, "ssd") == 0)
      dest_dlm = DLM_STATE_SSD;
    else if (strcasecmp(state, "nsecsd") == 0)
      dest_dlm = DLM_STATE_NSECSD;
    else if (strcasecmp(state, "dpl") == 0)
      dest_dlm = DLM_STATE_DPL;
    else if (strcasecmp(state, "lck_dbg") == 0)
      dest_dlm = DLM_STATE_LCK_DBG;
    else if (strcasecmp(state, "lck_boot") == 0)
      dest_dlm = DLM_STATE_LCK_BOOT;
    else
      errx(EXIT_FAILURE, "unknown DLM state: %s (use ssd/nsecsd/dpl/lck_dbg/lck_boot)", state);
  } else if (strcmp(command, "boundary") == 0) {
    cmd = CMD_BOUNDARY;
  } else if (strcmp(command, "boundary-set") == 0) {
    cmd = CMD_BOUNDARY_SET;
    if (!bnd_cfs1_set || !bnd_cfs2_set || !bnd_dfs_set || !bnd_srs1_set || !bnd_srs2_set)
      errx(EXIT_FAILURE, "boundary-set requires all options: --cfs1 --cfs2 --dfs --srs1 --srs2");
  } else if (strcmp(command, "param") == 0) {
    cmd = CMD_PARAM;
  } else if (strcmp(command, "param-set") == 0) {
    cmd = CMD_PARAM_SET;
    if (optind >= argc)
      errx(EXIT_FAILURE, "param-set requires an argument: enable or disable");
    const char *val = argv[optind];
    if (strcasecmp(val, "enable") == 0)
      param_value = PARAM_INIT_ENABLED;
    else if (strcasecmp(val, "disable") == 0)
      param_value = PARAM_INIT_DISABLED;
    else
      errx(EXIT_FAILURE, "invalid param-set value: %s (use enable or disable)", val);
  } else if (strcmp(command, "init") == 0) {
    cmd = CMD_INIT;
  } else if (strcmp(command, "osis") == 0) {
    cmd = CMD_OSIS;
  } else if (strcmp(command, "key-set") == 0) {
    cmd = CMD_KEY_SET;
    if (optind + 1 >= argc)
      errx(EXIT_FAILURE, "key-set requires index and file arguments");
    key_index = (uint8_t)strtoul(argv[optind], NULL, 10);
    key_file = argv[optind + 1];
  } else if (strcmp(command, "key-verify") == 0) {
    cmd = CMD_KEY_VERIFY;
    if (optind >= argc)
      errx(EXIT_FAILURE, "key-verify requires index argument");
    key_index = (uint8_t)strtoul(argv[optind], NULL, 10);
  } else if (strcmp(command, "ukey-set") == 0) {
    cmd = CMD_UKEY_SET;
    if (optind + 1 >= argc)
      errx(EXIT_FAILURE, "ukey-set requires index and file arguments");
    key_index = (uint8_t)strtoul(argv[optind], NULL, 10);
    key_file = argv[optind + 1];
  } else if (strcmp(command, "ukey-verify") == 0) {
    cmd = CMD_UKEY_VERIFY;
    if (optind >= argc)
      errx(EXIT_FAILURE, "ukey-verify requires index argument");
    key_index = (uint8_t)strtoul(argv[optind], NULL, 10);
  } else {
    errx(EXIT_FAILURE, "unknown command: %s", command);
  }

  ra_device_t dev;
  ra_dev_init(&dev);
  dev.uart_mode = uart_mode;

  if (ra_open(&dev, port) < 0)
    errx(EXIT_FAILURE, "failed to connect to device");

  if (ra_get_area_info(&dev, false) < 0) {
    ra_close(&dev);
    errx(EXIT_FAILURE, "failed to get area info");
  }

  /* Note: IDA with all-0xFF fails with 0xC1 on unlocked devices
   * Per Renesas docs, unlocked devices skip Authentication phase */

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
    if (ret == 0) {
      /* Show DLM state */
      uint8_t dlm_state;
      if (ra_get_dlm(&dev, &dlm_state) == 0) {
        printf("DLM State:          %s (0x%02X)\n", ra_dlm_state_name(dlm_state), dlm_state);
      }
      printf("\n");
      ra_get_area_info(&dev, true);
    }
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
  case CMD_CRC:
    ret = ra_crc(&dev, address, size, NULL);
    break;
  case CMD_DLM:
    ret = ra_get_dlm(&dev, NULL);
    break;
  case CMD_DLM_TRANSIT:
    ret = ra_dlm_transit(&dev, dest_dlm);
    break;
  case CMD_BOUNDARY:
    ret = ra_get_boundary(&dev, NULL);
    break;
  case CMD_BOUNDARY_SET:
    ret = ra_set_boundary(&dev, &bnd);
    break;
  case CMD_PARAM:
    ret = ra_get_param(&dev, PARAM_ID_INIT, NULL);
    break;
  case CMD_PARAM_SET:
    ret = ra_set_param(&dev, PARAM_ID_INIT, param_value);
    break;
  case CMD_INIT:
    ret = ra_initialize(&dev);
    break;
  case CMD_OSIS: {
    osis_status_t status;
    ret = ra_osis_detect(&dev, &status);
    if (ret == 0)
      ra_osis_print(&status);
  } break;
  case CMD_KEY_SET: {
    FILE *fp = fopen(key_file, "rb");
    if (fp == NULL) {
      warn("cannot open key file: %s", key_file);
      ret = -1;
      break;
    }
    uint8_t key_data[64];
    size_t key_len = fread(key_data, 1, sizeof(key_data), fp);
    fclose(fp);
    if (key_len == 0) {
      warnx("empty key file: %s", key_file);
      ret = -1;
      break;
    }
    ret = ra_key_set(&dev, key_index, key_data, key_len);
  } break;
  case CMD_KEY_VERIFY:
    ret = ra_key_verify(&dev, key_index, NULL);
    break;
  case CMD_UKEY_SET: {
    FILE *fp = fopen(key_file, "rb");
    if (fp == NULL) {
      warn("cannot open key file: %s", key_file);
      ret = -1;
      break;
    }
    uint8_t key_data[64];
    size_t key_len = fread(key_data, 1, sizeof(key_data), fp);
    fclose(fp);
    if (key_len == 0) {
      warnx("empty key file: %s", key_file);
      ret = -1;
      break;
    }
    ret = ra_ukey_set(&dev, key_index, key_data, key_len);
  } break;
  case CMD_UKEY_VERIFY:
    ret = ra_ukey_verify(&dev, key_index, NULL);
    break;
  default:
    break;
  }

  ra_close(&dev);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
