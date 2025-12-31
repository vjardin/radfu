/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * OSIS (OCD/Serial Programmer ID Setting Register) handling
 */

#ifndef RAOSIS_H
#define RAOSIS_H

#include "raconnect.h"
#include <stdint.h>

/*
 * OSIS register addresses (non-consecutive in option-setting memory)
 */
#define OSIS0_ADDR 0x01010018 /* bits [31:0]   */
#define OSIS1_ADDR 0x01010020 /* bits [63:32]  */
#define OSIS2_ADDR 0x01010028 /* bits [95:64]  */
#define OSIS3_ADDR 0x01010030 /* bits [127:96] - contains control bits */

#define OSIS_WORD_SIZE 4
#define OSIS_TOTAL_BITS 128

/*
 * OSIS security modes based on bits [127:126]
 */
typedef enum {
  OSIS_MODE_DISABLED = 0,          /* [127:126] = 00b - serial programming disabled */
  OSIS_MODE_LOCKED = 1,            /* [127:126] = 01b - locked, no ALeRASE */
  OSIS_MODE_LOCKED_WITH_ERASE = 2, /* [127:126] = 10b - locked, ALeRASE works */
  OSIS_MODE_UNLOCKED = 3,          /* [127:126] = 11b - unlocked (factory default) */
} osis_mode_t;

/*
 * OSIS register structure
 */
typedef struct {
  uint32_t word[4]; /* OSIS0-OSIS3 */
  osis_mode_t mode; /* Decoded security mode */
} osis_t;

/*
 * Read OSIS values from device
 * Returns: 0 on success, -1 on error
 */
int ra_osis_read(ra_device_t *dev, osis_t *osis);

/*
 * Display OSIS information
 */
void ra_osis_print(const osis_t *osis);

/*
 * Get human-readable mode name
 */
const char *ra_osis_mode_str(osis_mode_t mode);

#endif /* RAOSIS_H */
