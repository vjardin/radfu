/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Internal functions exposed for testing
 * Only include this header in test files!
 */

#ifndef RADFU_INTERNAL_H
#define RADFU_INTERNAL_H

#include "radfu.h"

#ifdef TESTING

/*
 * Get area type name based on address range
 */
const char *get_area_type(uint32_t sad);

/*
 * Get area type from KOA (Kind of Area) field per spec 6.16.2.2
 */
const char *get_area_type_koa(uint8_t koa);

/*
 * Get device group name from TYP field per spec 6.15.2.2
 */
const char *get_device_group(uint8_t typ);

/*
 * Format size with appropriate unit (KB/MB)
 */
void format_size(uint32_t bytes, char *buf, size_t buflen);

/*
 * Find area index containing the given address
 * Returns: area index (0-3) on success, -1 if not found
 */
int find_area_for_address(ra_device_t *dev, uint32_t addr);

/*
 * Set boundaries for erase operations (requires EAU alignment)
 */
int set_erase_boundaries(ra_device_t *dev, uint32_t start, uint32_t size, uint32_t *end_out);

/*
 * Set boundaries for read operations (requires RAU alignment)
 */
int set_read_boundaries(ra_device_t *dev, uint32_t start, uint32_t size, uint32_t *end_out);

/*
 * Set boundaries for write operations (requires WAU alignment)
 */
int set_write_boundaries(ra_device_t *dev, uint32_t start, uint32_t size, uint32_t *end_out);

/*
 * Set boundaries for CRC operations (requires CAU alignment)
 */
int set_crc_boundaries(ra_device_t *dev, uint32_t start, uint32_t size, uint32_t *end_out);

#endif /* TESTING */

#endif /* RADFU_INTERNAL_H */
