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

#endif /* RADFU_H */
