/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * High-level flash operations for Renesas RA bootloader
 */

#ifndef RADFU_H
#define RADFU_H

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
 * Returns: 0 on success, -1 on error
 */
int ra_read(ra_device_t *dev, const char *file, uint32_t start, uint32_t size);

/*
 * Write file to flash memory
 * Returns: 0 on success, -1 on error
 */
int ra_write(ra_device_t *dev, const char *file, uint32_t start, uint32_t size, bool verify);

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

/* DLM state codes */
#define DLM_STATE_CM 0x01       /* Chip Manufacturing */
#define DLM_STATE_SSD 0x02      /* Secure Software Development */
#define DLM_STATE_NSECSD 0x03   /* Non-Secure Software Development */
#define DLM_STATE_DPL 0x04      /* Deployed */
#define DLM_STATE_LCK_DBG 0x05  /* Locked Debug */
#define DLM_STATE_LCK_BOOT 0x06 /* Locked Boot Interface */
#define DLM_STATE_RMA_REQ 0x07  /* RMA Request */
#define DLM_STATE_RMA_ACK 0x08  /* RMA Acknowledged */

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

#endif /* RADFU_H */
