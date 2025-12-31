/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * OSIS (OCD/Serial Programmer ID Setting Register) detection
 *
 * The OSIS register controls ID code protection but cannot be read
 * directly via the bootloader. This module detects the protection
 * status by probing authentication behavior.
 */

#ifndef RAOSIS_H
#define RAOSIS_H

#include "raconnect.h"

/*
 * Detected OSIS protection status
 */
typedef enum {
  OSIS_MODE_UNLOCKED, /* No protection (factory default, all 0xFF) */
  OSIS_MODE_LOCKED,   /* ID authentication required */
  OSIS_MODE_DISABLED, /* Serial programming disabled */
  OSIS_MODE_UNKNOWN,  /* Could not determine status */
} osis_mode_t;

/*
 * OSIS detection result
 */
typedef struct {
  osis_mode_t mode;
  uint8_t error_code; /* MCU error code if detection involved errors */
} osis_status_t;

/*
 * Detect OSIS protection status by probing authentication
 * Returns: 0 on success, -1 on error
 */
int ra_osis_detect(ra_device_t *dev, osis_status_t *status);

/*
 * Display OSIS protection status
 */
void ra_osis_print(const osis_status_t *status);

/*
 * Get human-readable mode name
 */
const char *ra_osis_mode_str(osis_mode_t mode);

#endif /* RAOSIS_H */
