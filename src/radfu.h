/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * High-level flash operations for Renesas RA bootloader
 */

#ifndef RADFU_H
#define RADFU_H

#include "formats.h"
#include "raconnect.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * Query and store memory area information
 * Returns: 0 on success, -1 on error
 */
int ra_get_area_info(ra_device_t *dev, bool print);

/*
 * Query and display device signature
 * Returns: 0 on success, -1 on error
 */
int ra_get_dev_info(ra_device_t *dev);

/*
 * Query recommended maximum UART baudrate (RMB) from device signature
 * rmb_out: pointer to store the RMB value in bps
 * Returns: 0 on success, -1 on error
 */
int ra_get_rmb(ra_device_t *dev, uint32_t *rmb_out);

/*
 * Query device series max baud rate based on product name
 * Returns: max baud rate for the MCU series, or 115200 if unknown
 */
uint32_t ra_get_device_max_baudrate(ra_device_t *dev);

/*
 * Perform ID authentication with device
 * id_code: 16-byte ID code (hex string parsed to bytes)
 * Returns: 0 on success, -1 on error
 */
int ra_authenticate(ra_device_t *dev, const uint8_t *id_code);

/*
 * Erase flash sectors
 * Returns: 0 on success, -1 on error
 */
int ra_erase(ra_device_t *dev, uint32_t start, uint32_t size);

/*
 * Read flash memory to file
 * format: output file format (FORMAT_AUTO to detect from extension)
 * Returns: 0 on success, -1 on error
 */
int ra_read(
    ra_device_t *dev, const char *file, uint32_t start, uint32_t size, output_format_t format);

/*
 * Verify flash memory against file
 * Compares flash contents with file, reports first mismatch
 * format: input file format (FORMAT_AUTO to detect from extension)
 * If file contains address info (Intel HEX, S-record) and start==0,
 * the embedded address is used.
 * Returns: 0 on success (match), -1 on error or mismatch
 */
int ra_verify(
    ra_device_t *dev, const char *file, uint32_t start, uint32_t size, input_format_t format);

/*
 * Check if flash memory region is blank (all 0xFF)
 * Reports first non-blank byte if found
 *
 * NOTE: This only works reliably for code flash. Data flash returns
 * undefined values after erase (not 0xFF) per Renesas RA hardware spec.
 * The bootloader protocol does not expose the hardware blank-check command.
 *
 * Returns: 0 on success (blank), -1 on error or non-blank
 */
int ra_blank_check(ra_device_t *dev, uint32_t start, uint32_t size);

/*
 * Write file to flash memory
 * format: input file format (FORMAT_AUTO to detect from extension)
 * If file contains address info (Intel HEX, S-record) and start==0,
 * the embedded address is used.
 * Returns: 0 on success, -1 on error
 */
int ra_write(ra_device_t *dev,
    const char *file,
    uint32_t start,
    uint32_t size,
    bool verify,
    input_format_t format);

/*
 * Calculate CRC of flash memory region
 * Uses CRC-32-IEEE-802.3 (polynomial 0x04C11DB7)
 * crc_out: pointer to store the CRC result (may be NULL)
 * Returns: 0 on success, -1 on error
 */
int ra_crc(ra_device_t *dev, uint32_t start, uint32_t size, uint32_t *crc_out);

/*
 * Query Device Lifecycle Management (DLM) state
 * Supported on GrpA (RA4M2/3, RA6M4/5), GrpB (RA4E1, RA6E1), GrpC (RA6T2)
 * Not supported on GrpD (RA4E2, RA6E2, RA4T1, RA6T3)
 * dlm_out: pointer to store the DLM state code (may be NULL)
 * Returns: 0 on success, -1 on error
 */
int ra_get_dlm(ra_device_t *dev, uint8_t *dlm_out);

/*
 * Get DLM state name string
 */
const char *ra_dlm_state_name(uint8_t code);

/* DLM state codes */
#define DLM_STATE_CM 0x01       /* Chip Manufacturing */
#define DLM_STATE_SSD 0x02      /* Secure Software Development */
#define DLM_STATE_NSECSD 0x03   /* Non-Secure Software Development */
#define DLM_STATE_DPL 0x04      /* Deployed */
#define DLM_STATE_LCK_DBG 0x05  /* Locked Debug */
#define DLM_STATE_LCK_BOOT 0x06 /* Locked Boot Interface */
#define DLM_STATE_RMA_REQ 0x07  /* RMA Request */
#define DLM_STATE_RMA_ACK 0x08  /* RMA Acknowledged */

/* Device type (TYP) codes per spec 6.15.2.2 */
#define TYP_GRP_AB 0x01 /* GrpA/GrpB: RA4M2/3, RA6M4/5, RA4E1, RA6E1 */
#define TYP_GRP_C 0x02  /* GrpC: RA6T2 */
#define TYP_GRP_D 0x05  /* GrpD: RA4E2, RA6E2, RA4T1, RA6T3 */

/* Kind of Area (KOA) type codes per spec 6.16.2.2 */
#define KOA_TYPE_CODE 0x00   /* User/Code flash area (bank 0) */
#define KOA_TYPE_CODE1 0x01  /* User/Code flash area (bank 1, dual bank mode) */
#define KOA_TYPE_DATA 0x10   /* Data flash area */
#define KOA_TYPE_CONFIG 0x20 /* Config area */

/* Memory address range boundaries for area type detection */
#define ADDR_CODE_FLASH_END 0x00100000   /* Code flash: 0x00000000 - 0x000FFFFF */
#define ADDR_DATA_FLASH_START 0x08000000 /* Data flash start */
#define ADDR_DATA_FLASH_END 0x09000000   /* Data flash end (exclusive) */
#define ADDR_CONFIG_START 0x01000000     /* Config area start */
#define ADDR_CONFIG_END 0x02000000       /* Config area end (exclusive) */

/*
 * Find area by Kind of Area (KOA) type
 * Combines all areas matching KOA into a single SAD/EAD range
 * koa: KOA type (0x00=code, 0x10=data, 0x20=config)
 * sad_out: pointer to store start address (may be NULL)
 * ead_out: pointer to store end address (may be NULL)
 * Returns: 0 on success, -1 if no matching area found
 */
int ra_find_area_by_koa(ra_device_t *dev, uint8_t koa, uint32_t *sad_out, uint32_t *ead_out);

/*
 * Transition DLM state without authentication
 * Supported on GrpA (RA4M2/3, RA6M4/5), GrpB (RA4E1, RA6E1), GrpC (RA6T2)
 * Not supported on GrpD (RA4E2, RA6E2, RA4T1, RA6T3)
 *
 * Allowed transitions (without authentication):
 *   CM (0x01) -> SSD (0x02)
 *   SSD (0x02) -> NSECSD (0x03), DPL (0x04)
 *   NSECSD (0x03) -> DPL (0x04)
 *   DPL (0x04) -> LCK_DBG (0x05), LCK_BOOT (0x06)
 *   LCK_DBG (0x05) -> LCK_BOOT (0x06)
 *
 * WARNING: LCK_BOOT transition causes bootloader to hang (no more commands)
 * dest_dlm: destination DLM state code
 * Returns: 0 on success, -1 on error
 */
int ra_dlm_transit(ra_device_t *dev, uint8_t dest_dlm);

/*
 * Transition DLM state with authentication (regression)
 * Supported on GrpA (RA4M2/3, RA6M4/5), GrpB (RA4E1, RA6E1), GrpC (RA6T2)
 * Not supported on GrpD (RA4E2, RA6E2, RA4T1, RA6T3)
 *
 * Allowed authenticated transitions (regression without erase):
 *   NSECSD (0x03) -> SSD (0x02) using SECDBG_KEY
 *   DPL (0x04) -> NSECSD (0x03) using NONSECDBG_KEY
 *   SSD (0x02) -> RMA_REQ (0x07) using RMA_KEY (erases flash!)
 *   DPL (0x04) -> RMA_REQ (0x07) using RMA_KEY (erases flash!)
 *
 * Authentication uses challenge-response:
 *   GrpA/GrpB: HMAC-SHA256(key, challenge || fixed_value)
 *   GrpC: AES-128-CMAC(key, challenge)
 *
 * dest_dlm: destination DLM state code
 * key: 16-byte plaintext authentication key
 * Returns: 0 on success, -1 on error
 */
int ra_dlm_auth(ra_device_t *dev, uint8_t dest_dlm, const uint8_t *key);

/*
 * Secure/Non-secure boundary settings
 * All sizes are in KB
 */
typedef struct {
  uint16_t cfs1; /* Code flash secure region size without NSC */
  uint16_t cfs2; /* Code flash secure region size */
  uint16_t dfs;  /* Data flash secure region size */
  uint16_t srs1; /* SRAM secure region size without NSC */
  uint16_t srs2; /* SRAM secure region size */
} ra_boundary_t;

/*
 * Query secure/non-secure boundary settings
 * Supported on GrpA (RA4M2/3, RA6M4/5), GrpB (RA4E1, RA6E1), GrpC (RA6T2)
 * Not supported on GrpD (RA4E2, RA6E2, RA4T1, RA6T3)
 * bnd_out: pointer to store the boundary settings (may be NULL)
 * Returns: 0 on success, -1 on error
 */
int ra_get_boundary(ra_device_t *dev, ra_boundary_t *bnd_out);

/*
 * Set secure/non-secure boundary settings
 * Supported on GrpA (RA4M2/3, RA6M4/5), GrpB (RA4E1, RA6E1), GrpC (RA6T2)
 * Not supported on GrpD (RA4E2, RA6E2, RA4T1, RA6T3)
 *
 * All sizes are in KB:
 *   cfs1: Code flash secure region without NSC
 *   cfs2: Code flash secure region total (32KB alignment)
 *   dfs:  Data flash secure region
 *   srs1: SRAM secure region without NSC
 *   srs2: SRAM secure region total (8KB alignment)
 *
 * Constraints: cfs1 <= cfs2, srs1 <= srs2
 * Settings become effective after device reset.
 * Returns: 0 on success, -1 on error
 */
int ra_set_boundary(ra_device_t *dev, const ra_boundary_t *bnd);

/* Parameter IDs for ra_get_param() */
#define PARAM_ID_INIT 0x01 /* Initialization enable/disable */

/* Parameter values for PARAM_ID_INIT */
#define PARAM_INIT_DISABLED 0x00
#define PARAM_INIT_ENABLED 0x07

/*
 * Query device parameter
 * Supported on GrpA (RA4M2/3, RA6M4/5), GrpB (RA4E1, RA6E1), GrpC (RA6T2)
 * Not supported on GrpD (RA4E2, RA6E2, RA4T1, RA6T3)
 * param_id: parameter ID (e.g., PARAM_ID_INIT)
 * value_out: pointer to store the parameter value (may be NULL)
 * Returns: 0 on success, -1 on error
 */
int ra_get_param(ra_device_t *dev, uint8_t param_id, uint8_t *value_out);

/*
 * Set device parameter
 * Supported on GrpA (RA4M2/3, RA6M4/5), GrpB (RA4E1, RA6E1), GrpC (RA6T2)
 * Not supported on GrpD (RA4E2, RA6E2, RA4T1, RA6T3)
 * param_id: parameter ID (e.g., PARAM_ID_INIT)
 * value: parameter value to set
 *
 * For PARAM_ID_INIT:
 *   PARAM_INIT_DISABLED (0x00): Disable initialization command
 *   PARAM_INIT_ENABLED (0x07): Enable initialization command
 *
 * WARNING: Disabling initialization prevents factory reset capability.
 * Returns: 0 on success, -1 on error
 */
int ra_set_param(ra_device_t *dev, uint8_t param_id, uint8_t value);

/*
 * Initialize device (factory reset)
 * Clears User area, Data area, Config area, boundary settings, and key index.
 * DLM state transitions to SSD after completion.
 * Supported on GrpA (RA4M2/3, RA6M4/5), GrpB (RA4E1, RA6E1), GrpC (RA6T2)
 * Not supported on GrpD (RA4E2, RA6E2, RA4T1, RA6T3)
 * Requires: initialization command enabled (PARAM_ID_INIT == 0x07)
 * Can only be executed from SSD, NSECSD, or DPL states (not CM)
 * Returns: 0 on success, -1 on error
 */
int ra_initialize(ra_device_t *dev);

/*
 * Key setting - inject wrapped DLM key for authenticated state transitions
 * Supported on GrpA (RA4M2/3, RA6M4/5), GrpB (RA4E1, RA6E1), GrpC (RA6T2)
 * Not supported on GrpD (RA4E2, RA6E2, RA4T1, RA6T3)
 *
 * key_type: Key type (KYTY field per R01AN5562)
 * wrapped_key: Wrapped key data (W-UFPK + IV + encrypted key + MAC)
 * key_len: Length of wrapped key data (80 bytes for DLM keys)
 *
 * Key types (KYTY):
 *   0x01: SECDBG_KEY     - Secure debug auth (DLM-SSD regression)
 *   0x02: NONSECDBG_KEY  - Non-secure debug auth (DLM-NSECSD regression)
 *   0x03: RMA_KEY        - Return Material Authorization
 *
 * Keys must be wrapped using Renesas SKMT or security/rawrapkey.sh script.
 * See security/SECURITY.md for the wrapping process.
 * Returns: 0 on success, -1 on error
 */
int ra_key_set(ra_device_t *dev, uint8_t key_type, const uint8_t *wrapped_key, size_t key_len);

/*
 * Key verify - verify injected key at key type
 * Checks if a valid key exists for the specified key type.
 * key_type: Key type (KYTY: 0x01=SECDBG, 0x02=NONSECDBG, 0x03=RMA)
 * valid_out: pointer to store result (1=valid, 0=invalid/empty), may be NULL
 * Returns: 0 on success, -1 on error
 */
int ra_key_verify(ra_device_t *dev, uint8_t key_type, int *valid_out);

/*
 * User key setting - inject user-defined wrapped key
 * Similar to ra_key_set but for user-defined key slots.
 * key_index: User key slot index
 * wrapped_key: Wrapped key data
 * key_len: Length of wrapped key data
 * Returns: 0 on success, -1 on error
 */
int ra_ukey_set(ra_device_t *dev, uint8_t key_index, const uint8_t *wrapped_key, size_t key_len);

/*
 * User key verify - verify user key at index
 * valid_out: pointer to store result (1=valid, 0=invalid/empty), may be NULL
 * Returns: 0 on success, -1 on error
 */
int ra_ukey_verify(ra_device_t *dev, uint8_t key_index, int *valid_out);

/*
 * Read and display config area contents
 * Reads the config area (KOA=0x20) and displays known fields.
 * Returns: 0 on success, -1 on error
 */
int ra_config_read(ra_device_t *dev);

/*
 * Send raw command for protocol analysis/exploration
 * Sends a command with optional data and displays detailed TX/RX analysis.
 * Useful for exploring undocumented commands or debugging protocol issues.
 *
 * cmd: command byte (0x00-0xFF)
 * data: optional data bytes to send (may be NULL if data_len is 0)
 * data_len: number of data bytes
 *
 * Output includes:
 * - Raw hexdump of TX and RX packets
 * - Field-by-field analysis (SOH/SOD, LNH/LNL, CMD/RES, DATA, SUM, ETX)
 * - Known field interpretation (addresses, DLM states, error codes, etc.)
 *
 * Returns: 0 on success, -1 on error
 */
int ra_raw_cmd(ra_device_t *dev, uint8_t cmd, const uint8_t *data, size_t data_len);

#endif /* RADFU_H */
