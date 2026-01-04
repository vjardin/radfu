/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * RADFU - Renesas RA Device Firmware Update tool
 */

#include "compat.h"
#include "formats.h"
#include "progress.h"
#include "raconnect.h"
#include "radfu.h"
#include "raosis.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
      "  write <file>[:<addr>] ...  Write file(s) to flash memory\n"
      "  verify <file>  Verify flash memory against file\n"
      "  erase          Erase flash sectors\n"
      "  blank-check    Check if flash region is erased (all 0xFF)\n"
      "  crc            Calculate CRC-32 of flash region\n"
      "  dlm            Show Device Lifecycle Management state\n"
      "  dlm-transit <state>  Transition DLM state (ssd/nsecsd/dpl/lck_dbg/lck_boot)\n"
      "  dlm-auth <state> <key>  Authenticated DLM transition (ssd/nsecsd/rma_req)\n"
      "                       Key format: file:<path> or hex:<32_hex_chars>\n"
      "  boundary       Show secure/non-secure boundary settings\n"
      "  boundary-set   Set TrustZone boundaries (--file <rpd> or explicit options)\n"
      "  param          Show device parameter (initialization command)\n"
      "  param-set <enable|disable>  Enable/disable initialization command\n"
      "  init           Initialize device (factory reset to SSD state)\n"
      "  osis           Show OSIS (ID code protection) status\n"
      "  config-read    Read and display config area contents\n"
      "  key-set <type> <file>   Inject wrapped DLM key (secdbg|nonsecdbg|rma)\n"
      "  key-verify <type>       Verify DLM key (secdbg|nonsecdbg|rma)\n"
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
      "  -f, --input-format <fmt>  Input file format (auto/bin/ihex/srec)\n"
      "  -F, --output-format <fmt> Output file format (auto/bin/ihex/srec)\n"
      "      --area <type>    Select memory area (code/data/config or KOA value)\n"
      "      --bank <n>       Select bank for dual bank mode (0 or 1)\n"
      "  -u, --uart           Use plain UART mode (P109/P110 pins)\n"
      "  -q, --quiet          Suppress progress bar output\n"
      "      --cfs1 <KB>      Code flash secure region size without NSC\n"
      "      --cfs2 <KB>      Code flash secure region size (total)\n"
      "      --dfs <KB>       Data flash secure region size\n"
      "      --srs1 <KB>      SRAM secure region size without NSC\n"
      "      --srs2 <KB>      SRAM secure region size (total)\n"
      "      --file <rpd>     Load boundary settings from .rpd file\n"
      "  -h, --help           Show this help message\n"
      "  -V, --version        Show version\n"
      "\n"
      "See 'man radfu' for examples and detailed documentation.\n");
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

/* Maximum number of files for multi-file write */
#define MAX_WRITE_FILES 16

/* Entry for multi-file write: file path and optional address */
typedef struct {
  const char *path;
  uint32_t address;
  bool has_address;
} write_entry_t;

/*
 * Parse file:address format
 * Format: filename or filename:0xADDRESS
 * Returns 0 on success, -1 on error
 */
static int
parse_write_entry(const char *arg, write_entry_t *entry) {
  /* Find last colon that could be an address separator */
  const char *colon = strrchr(arg, ':');

  /* Check if this looks like an address (hex number after colon) */
  if (colon != NULL && colon[1] != '\0') {
    char *endptr;
    unsigned long val = strtoul(colon + 1, &endptr, 16);
    if (*endptr == '\0') {
      /* Valid hex address found */
      size_t path_len = (size_t)(colon - arg);
      char *path = malloc(path_len + 1);
      if (!path)
        return -1;
      memcpy(path, arg, path_len);
      path[path_len] = '\0';
      entry->path = path;
      entry->address = (uint32_t)val;
      entry->has_address = true;
      return 0;
    }
  }

  /* No address, use entire string as path */
  entry->path = arg;
  entry->address = 0;
  entry->has_address = false;
  return 0;
}

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
  CMD_VERIFY,
  CMD_ERASE,
  CMD_BLANK_CHECK,
  CMD_CRC,
  CMD_DLM,
  CMD_DLM_TRANSIT,
  CMD_DLM_AUTH,
  CMD_BOUNDARY,
  CMD_BOUNDARY_SET,
  CMD_PARAM,
  CMD_PARAM_SET,
  CMD_INIT,
  CMD_OSIS,
  CMD_CONFIG_READ,
  CMD_KEY_SET,
  CMD_KEY_VERIFY,
  CMD_UKEY_SET,
  CMD_UKEY_VERIFY,
};

/* DLM key types (KYTY) per R01AN5562 */
#define KYTY_SECDBG 0x01
#define KYTY_NONSECDBG 0x02
#define KYTY_RMA 0x03

/* DLM authentication key length (16 bytes / 128 bits) */
#define DLM_AUTH_KEY_LEN 16

/*
 * Parse DLM authentication key from hex string
 * Returns: 0 on success, -1 on error
 */
static int
parse_hex_key(const char *str, uint8_t *key) {
  size_t len = strlen(str);

  /* Accept with or without 0x prefix */
  if (len >= 2 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
    str += 2;
    len -= 2;
  }

  if (len != DLM_AUTH_KEY_LEN * 2) {
    warnx("authentication key must be %d hex bytes (%d hex characters)",
        DLM_AUTH_KEY_LEN,
        DLM_AUTH_KEY_LEN * 2);
    return -1;
  }

  for (int i = 0; i < DLM_AUTH_KEY_LEN; i++) {
    unsigned int byte;
    if (sscanf(str + i * 2, "%2x", &byte) != 1) {
      warnx("invalid hex character in key at position %d", i * 2);
      return -1;
    }
    key[i] = (uint8_t)byte;
  }

  return 0;
}

/*
 * Load DLM authentication key from file (binary, 16 bytes)
 * Returns: 0 on success, -1 on error
 */
static int
load_key_from_file(const char *filename, uint8_t *key) {
  FILE *f = fopen(filename, "rb");
  if (f == NULL) {
    warn("failed to open key file: %s", filename);
    return -1;
  }

  size_t n = fread(key, 1, DLM_AUTH_KEY_LEN, f);
  fclose(f);

  if (n != DLM_AUTH_KEY_LEN) {
    warnx("key file must be %d bytes (got %zu)", DLM_AUTH_KEY_LEN, n);
    return -1;
  }

  return 0;
}

/*
 * Parse authentication key from string
 * Format: file:<filename> - read 16-byte binary key from file
 *         hex:<value>     - parse 32-char hex string (with/without 0x)
 * Returns: 0 on success, -1 on error
 */
static int
parse_auth_key(const char *str, uint8_t *key) {
  if (strncmp(str, "file:", 5) == 0) {
    return load_key_from_file(str + 5, key);
  } else if (strncmp(str, "hex:", 4) == 0) {
    return parse_hex_key(str + 4, key);
  } else {
    warnx("invalid key format: %s", str);
    warnx("use: file:<filename> for binary key file");
    warnx("     hex:<hex_value> for hex string (32 chars)");
    return -1;
  }
}

/*
 * Parse .rpd (Renesas Partition Data) file for TrustZone boundary settings
 * Format: key=value lines where values are hex (with 0x prefix) in bytes
 * Keys: FLASH_S_SIZE, FLASH_C_SIZE, RAM_S_SIZE, RAM_C_SIZE, DATA_FLASH_S_SIZE
 * Converts bytes to KB for boundary settings
 * Returns: 0 on success, -1 on error
 */
static int
parse_rpd_file(const char *filename, ra_boundary_t *bnd) {
  FILE *f = fopen(filename, "r");
  if (!f) {
    warn("failed to open boundary file: %s", filename);
    return -1;
  }

  char line[256];
  bool found_cfs1 = false, found_cfs2 = false, found_dfs = false;
  bool found_srs1 = false, found_srs2 = false;

  while (fgets(line, sizeof(line), f)) {
    char *eq = strchr(line, '=');
    if (!eq)
      continue;

    *eq = '\0';
    const char *key = line;
    const char *val = eq + 1;

    /* Skip 0x prefix if present */
    if (val[0] == '0' && (val[1] == 'x' || val[1] == 'X'))
      val += 2;

    char *endptr;
    unsigned long bytes = strtoul(val, &endptr, 16);

    /* Convert bytes to KB */
    uint16_t kb = (uint16_t)(bytes / 1024);

    if (strcmp(key, "FLASH_S_SIZE") == 0) {
      bnd->cfs1 = kb;
      found_cfs1 = true;
    } else if (strcmp(key, "FLASH_C_SIZE") == 0) {
      bnd->cfs2 = kb;
      found_cfs2 = true;
    } else if (strcmp(key, "RAM_S_SIZE") == 0) {
      bnd->srs1 = kb;
      found_srs1 = true;
    } else if (strcmp(key, "RAM_C_SIZE") == 0) {
      bnd->srs2 = kb;
      found_srs2 = true;
    } else if (strcmp(key, "DATA_FLASH_S_SIZE") == 0) {
      bnd->dfs = kb;
      found_dfs = true;
    }
  }

  fclose(f);

  if (!found_cfs1 || !found_cfs2 || !found_dfs || !found_srs1 || !found_srs2) {
    warnx("incomplete .rpd file: missing required fields");
    if (!found_cfs1)
      warnx("  missing FLASH_S_SIZE (CFS1)");
    if (!found_cfs2)
      warnx("  missing FLASH_C_SIZE (CFS2)");
    if (!found_dfs)
      warnx("  missing DATA_FLASH_S_SIZE (DFS)");
    if (!found_srs1)
      warnx("  missing RAM_S_SIZE (SRS1)");
    if (!found_srs2)
      warnx("  missing RAM_C_SIZE (SRS2)");
    return -1;
  }

  return 0;
}

/*
 * Parse key type from string: accepts numeric (1,2,3) or keywords
 * Keywords: secdbg, nonsecdbg, rma (case-insensitive)
 * Returns key type on success, 0 on error
 */
static uint8_t
parse_key_type(const char *str) {
  if (strcasecmp(str, "secdbg") == 0)
    return KYTY_SECDBG;
  if (strcasecmp(str, "nonsecdbg") == 0)
    return KYTY_NONSECDBG;
  if (strcasecmp(str, "rma") == 0)
    return KYTY_RMA;

  /* Try numeric */
  char *endptr;
  unsigned long val = strtoul(str, &endptr, 0);
  if (*endptr != '\0' || val < 1 || val > 3) {
    warnx("invalid key type: %s (use secdbg, nonsecdbg, rma, or 1-3)", str);
    return 0;
  }
  return (uint8_t)val;
}

/* Long-only options use values >= 256 */
#define OPT_CFS1 256
#define OPT_CFS2 257
#define OPT_DFS 258
#define OPT_SRS1 259
#define OPT_SRS2 260
#define OPT_AREA 261
#define OPT_BOUNDARY_FILE 262
#define OPT_BANK 263

static const struct option longopts[] = {
  { "port",          required_argument, NULL, 'p'               },
  { "address",       required_argument, NULL, 'a'               },
  { "size",          required_argument, NULL, 's'               },
  { "baudrate",      required_argument, NULL, 'b'               },
  { "id",            required_argument, NULL, 'i'               },
  { "erase-all",     no_argument,       NULL, 'e'               },
  { "verify",        no_argument,       NULL, 'v'               },
  { "input-format",  required_argument, NULL, 'f'               },
  { "output-format", required_argument, NULL, 'F'               },
  { "uart",          no_argument,       NULL, 'u'               },
  { "quiet",         no_argument,       NULL, 'q'               },
  { "cfs1",          required_argument, NULL, OPT_CFS1          },
  { "cfs2",          required_argument, NULL, OPT_CFS2          },
  { "dfs",           required_argument, NULL, OPT_DFS           },
  { "srs1",          required_argument, NULL, OPT_SRS1          },
  { "srs2",          required_argument, NULL, OPT_SRS2          },
  { "area",          required_argument, NULL, OPT_AREA          },
  { "bank",          required_argument, NULL, OPT_BANK          },
  { "file",          required_argument, NULL, OPT_BOUNDARY_FILE },
  { "help",          no_argument,       NULL, 'h'               },
  { "version",       no_argument,       NULL, 'V'               },
  { NULL,            0,                 NULL, 0                 }
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
  input_format_t input_format = FORMAT_AUTO;
  output_format_t output_format = FORMAT_AUTO;
  uint8_t dest_dlm = 0;
  uint8_t param_value = 0;
  uint8_t key_index = 0;
  const char *key_file = NULL;
  uint8_t auth_key[DLM_AUTH_KEY_LEN];
  ra_boundary_t bnd = { 0 };
  bool bnd_cfs1_set = false, bnd_cfs2_set = false, bnd_dfs_set = false;
  bool bnd_srs1_set = false, bnd_srs2_set = false;
  const char *boundary_file = NULL;
  int8_t area_koa = -1; /* -1 = not set, 0/1/2 = code/data/config */
  int8_t bank = -1;     /* -1 = not set, 0/1 = bank selection for dual bank mode */
  bool addr_explicit = false, size_explicit = false;
  write_entry_t write_entries[MAX_WRITE_FILES];
  int write_count = 0;
  enum command cmd = CMD_NONE;
  int opt;

  while ((opt = getopt_long(argc, argv, "p:a:s:b:i:evf:F:uqhV", longopts, NULL)) != -1) {
    switch (opt) {
    case 'p':
      port = optarg;
      break;
    case 'a':
      address = parse_hex(optarg);
      addr_explicit = true;
      break;
    case 's':
      size = parse_hex(optarg);
      size_explicit = true;
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
    case 'f':
      if (strcasecmp(optarg, "auto") == 0)
        input_format = FORMAT_AUTO;
      else if (strcasecmp(optarg, "bin") == 0 || strcasecmp(optarg, "binary") == 0)
        input_format = FORMAT_BIN;
      else if (strcasecmp(optarg, "ihex") == 0 || strcasecmp(optarg, "hex") == 0)
        input_format = FORMAT_IHEX;
      else if (strcasecmp(optarg, "srec") == 0 || strcasecmp(optarg, "s19") == 0)
        input_format = FORMAT_SREC;
      else
        errx(EXIT_FAILURE, "unknown input format: %s (use auto/bin/ihex/srec)", optarg);
      break;
    case 'F':
      if (strcasecmp(optarg, "auto") == 0)
        output_format = FORMAT_AUTO;
      else if (strcasecmp(optarg, "bin") == 0 || strcasecmp(optarg, "binary") == 0)
        output_format = FORMAT_BIN;
      else if (strcasecmp(optarg, "ihex") == 0 || strcasecmp(optarg, "hex") == 0)
        output_format = FORMAT_IHEX;
      else if (strcasecmp(optarg, "srec") == 0 || strcasecmp(optarg, "s19") == 0)
        output_format = FORMAT_SREC;
      else
        errx(EXIT_FAILURE, "unknown output format: %s (use auto/bin/ihex/srec)", optarg);
      break;
    case 'u':
      uart_mode = true;
      break;
    case 'q':
      progress_global_quiet = 1;
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
    case OPT_AREA:
      if (strcasecmp(optarg, "code") == 0)
        area_koa = KOA_TYPE_CODE;
      else if (strcasecmp(optarg, "data") == 0)
        area_koa = KOA_TYPE_DATA;
      else if (strcasecmp(optarg, "config") == 0)
        area_koa = KOA_TYPE_CONFIG;
      else {
        char *endptr;
        unsigned long val = strtoul(optarg, &endptr, 0);
        if (*endptr != '\0' || val > 0x20)
          errx(EXIT_FAILURE, "invalid area: %s (use code/data/config or KOA value)", optarg);
        area_koa = (int8_t)val;
      }
      break;
    case OPT_BANK: {
      char *endptr;
      long val = strtol(optarg, &endptr, 10);
      if (*endptr != '\0' || val < 0 || val > 1)
        errx(EXIT_FAILURE, "invalid bank: %s (use 0 or 1)", optarg);
      bank = (int8_t)val;
      break;
    }
    case OPT_BOUNDARY_FILE:
      boundary_file = optarg;
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
      errx(EXIT_FAILURE, "write command requires at least one file argument");
    /* Parse all remaining arguments as file:address pairs */
    while (optind < argc && write_count < MAX_WRITE_FILES) {
      if (parse_write_entry(argv[optind], &write_entries[write_count]) < 0)
        errx(EXIT_FAILURE, "failed to parse file argument: %s", argv[optind]);
      write_count++;
      optind++;
    }
    if (write_count == 0)
      errx(EXIT_FAILURE, "write command requires at least one file argument");
    /* For single file, also set file/address for backward compatibility */
    if (write_count == 1) {
      file = write_entries[0].path;
      if (write_entries[0].has_address && address == 0)
        address = write_entries[0].address;
    }
  } else if (strcmp(command, "verify") == 0) {
    cmd = CMD_VERIFY;
    if (optind >= argc)
      errx(EXIT_FAILURE, "verify command requires a file argument");
    file = argv[optind];
  } else if (strcmp(command, "erase") == 0) {
    cmd = CMD_ERASE;
  } else if (strcmp(command, "blank-check") == 0) {
    cmd = CMD_BLANK_CHECK;
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
  } else if (strcmp(command, "dlm-auth") == 0) {
    cmd = CMD_DLM_AUTH;
    if (optind + 1 >= argc)
      errx(EXIT_FAILURE, "dlm-auth requires <state> and <key> arguments");
    const char *state = argv[optind];
    if (strcasecmp(state, "ssd") == 0)
      dest_dlm = DLM_STATE_SSD;
    else if (strcasecmp(state, "nsecsd") == 0)
      dest_dlm = DLM_STATE_NSECSD;
    else if (strcasecmp(state, "rma_req") == 0)
      dest_dlm = DLM_STATE_RMA_REQ;
    else
      errx(EXIT_FAILURE, "dlm-auth: invalid target state: %s (use ssd/nsecsd/rma_req)", state);
    if (parse_auth_key(argv[optind + 1], auth_key) < 0)
      errx(EXIT_FAILURE, "dlm-auth: invalid key format");
  } else if (strcmp(command, "boundary") == 0) {
    cmd = CMD_BOUNDARY;
  } else if (strcmp(command, "boundary-set") == 0) {
    cmd = CMD_BOUNDARY_SET;
    if (boundary_file) {
      /* Parse .rpd file */
      if (parse_rpd_file(boundary_file, &bnd) < 0)
        exit(EXIT_FAILURE);
      printf("Loaded boundary settings from %s:\n"
             "  CFS1: %u KB, CFS2: %u KB, DFS: %u KB\n"
             "  SRS1: %u KB, SRS2: %u KB\n",
          boundary_file,
          bnd.cfs1,
          bnd.cfs2,
          bnd.dfs,
          bnd.srs1,
          bnd.srs2);
    } else if (!bnd_cfs1_set || !bnd_cfs2_set || !bnd_dfs_set || !bnd_srs1_set || !bnd_srs2_set) {
      errx(EXIT_FAILURE,
          "boundary-set requires --file <rpd> or all options: --cfs1 --cfs2 --dfs --srs1 --srs2");
    }
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
  } else if (strcmp(command, "config-read") == 0) {
    cmd = CMD_CONFIG_READ;
  } else if (strcmp(command, "key-set") == 0) {
    cmd = CMD_KEY_SET;
    if (optind + 1 >= argc)
      errx(EXIT_FAILURE, "key-set requires type and file arguments");
    key_index = parse_key_type(argv[optind]);
    if (key_index == 0)
      errx(EXIT_FAILURE, "key-set: invalid key type");
    key_file = argv[optind + 1];
  } else if (strcmp(command, "key-verify") == 0) {
    cmd = CMD_KEY_VERIFY;
    if (optind >= argc)
      errx(EXIT_FAILURE, "key-verify requires type argument");
    key_index = parse_key_type(argv[optind]);
    if (key_index == 0)
      errx(EXIT_FAILURE, "key-verify: invalid key type");
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

  /* Handle --bank option for dual bank mode */
  if (bank >= 0) {
    /* Bank 0 = KOA 0x00 (User area 0), Bank 1 = KOA 0x01 (User area 1) */
    if (dev.noa <= 4) {
      ra_close(&dev);
      errx(EXIT_FAILURE, "device is not in dual bank mode (NOA=%d)", dev.noa);
    }
    if (area_koa >= 0 && area_koa != KOA_TYPE_CODE && area_koa != 0x01)
      warnx("--bank overrides --area for user area selection");
    area_koa = bank; /* 0 = KOA_TYPE_CODE (0x00), 1 = 0x01 */
  }

  /* Resolve --area to address/size if specified */
  if (area_koa >= 0) {
    uint32_t area_sad, area_ead;
    if (ra_find_area_by_koa(&dev, (uint8_t)area_koa, &area_sad, &area_ead) < 0) {
      ra_close(&dev);
      errx(EXIT_FAILURE, "area not found");
    }
    /* Config area CRC requires exact boundaries per spec 6.21 */
    if (cmd == CMD_CRC && area_koa == KOA_TYPE_CONFIG) {
      if (addr_explicit || size_explicit)
        warnx("config area CRC requires exact boundaries, -a/-s ignored");
      address = area_sad;
      size = area_ead - area_sad + 1;
    } else {
      /* --area sets defaults, -a/-s can still override */
      if (address == 0)
        address = area_sad;
      if (size == 0)
        size = area_ead - area_sad + 1;
    }
  }

  /* Note: IDA with all-0xFF fails with 0xC1 on unlocked devices
   * Per Renesas docs, unlocked devices skip Authentication phase */

  /* Set baud rate: auto-detect max in UART mode, or use explicit value */
  if (baudrate > 0 && baudrate != 9600) {
    if (ra_set_baudrate(&dev, baudrate) < 0) {
      ra_close(&dev);
      errx(EXIT_FAILURE, "failed to set baud rate");
    }
  } else if (uart_mode && baudrate == 0) {
    /* Auto-switch to max baud rate in UART mode */
    /* Get device limit based on MCU series */
    uint32_t device_max = ra_get_device_max_baudrate(&dev);

    /* Get adapter limit based on USB VID/PID */
    const char *tty = port;
    if (tty != NULL) {
      const char *slash = strrchr(tty, '/');
      if (slash != NULL)
        tty = slash + 1;
    }
    uint32_t adapter_max = ra_get_adapter_max_baudrate(tty);

    /* Use minimum of device max, adapter max, and termios support */
    uint32_t target = device_max < adapter_max ? device_max : adapter_max;
    uint32_t best = ra_best_baudrate(target);

    if (best > 9600) {
      if (ra_set_baudrate(&dev, best) == 0) {
        /* Verify communication works at new rate */
        uint32_t rmb_check;
        if (ra_get_rmb(&dev, &rmb_check) < 0) {
          ra_close(&dev);
          errx(EXIT_FAILURE,
              "communication failed at %u bps, reset board and use -b 115200 or lower",
              best);
        }
      } else {
        warnx("baud rate %u bps failed, falling back", best);
        if (ra_set_baudrate(&dev, 115200) < 0) {
          warnx("continuing at 9600 bps");
        }
      }
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
    ret = ra_read(&dev, file, address, size, output_format);
    break;
  case CMD_WRITE:
    if (write_count == 1) {
      /* Single file mode - use file/address variables (may include --area) */
      ret = ra_write(&dev, file, address, size, verify, input_format);
    } else {
      /* Multi-file mode - write each file sequentially */
      for (int i = 0; i < write_count; i++) {
        uint32_t addr = write_entries[i].has_address ? write_entries[i].address : 0;
        printf("Writing %s to 0x%08X...\n", write_entries[i].path, addr);
        ret = ra_write(&dev, write_entries[i].path, addr, 0, verify, input_format);
        if (ret < 0) {
          warnx("failed to write %s", write_entries[i].path);
          break;
        }
      }
      if (ret == 0)
        printf("All %d files programmed successfully.\n", write_count);
    }
    break;
  case CMD_VERIFY:
    ret = ra_verify(&dev, file, address, size, input_format);
    break;
  case CMD_ERASE:
    ret = ra_erase(&dev, address, size);
    break;
  case CMD_BLANK_CHECK:
    ret = ra_blank_check(&dev, address, size);
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
  case CMD_DLM_AUTH:
    ret = ra_dlm_auth(&dev, dest_dlm, auth_key);
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
  case CMD_CONFIG_READ:
    ret = ra_config_read(&dev);
    break;
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
