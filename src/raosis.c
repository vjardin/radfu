/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * OSIS (OCD/Serial Programmer ID Setting Register) detection
 *
 * Detection strategy:
 * The OSIS register cannot be read directly via the bootloader (security).
 * We detect the protection status based on whether area info query succeeded,
 * which happens before this command is called.
 *
 * If we reach this point, device communication is working, meaning either:
 * - Device is unlocked (no protection)
 * - User provided correct ID via -i option
 */

#include "raosis.h"
#include "rapacker.h"

#include <stdio.h>
#include <string.h>

int
ra_osis_detect(ra_device_t *dev, osis_status_t *status) {
  memset(status, 0, sizeof(*status));

  /*
   * If we got here, ra_get_area_info() already succeeded.
   * This means the device is either:
   * 1. Unlocked (factory default, no protection)
   * 2. Locked but user authenticated with -i option
   *
   * We distinguish these cases by checking if authentication was performed.
   */
  if (dev->authenticated) {
    status->mode = OSIS_MODE_LOCKED;
  } else {
    status->mode = OSIS_MODE_UNLOCKED;
  }

  return 0;
}

const char *
ra_osis_mode_str(osis_mode_t mode) {
  switch (mode) {
  case OSIS_MODE_UNLOCKED:
    return "Unlocked (no ID protection)";
  case OSIS_MODE_LOCKED:
    return "Locked (ID authentication required)";
  case OSIS_MODE_DISABLED:
    return "Disabled (serial programming blocked)";
  case OSIS_MODE_UNKNOWN:
    return "Unknown";
  default:
    return "Invalid";
  }
}

void
ra_osis_print(const osis_status_t *status) {
  printf("OSIS Protection Status:\n");
  printf("  Mode: %s\n", ra_osis_mode_str(status->mode));

  switch (status->mode) {
  case OSIS_MODE_UNLOCKED:
    printf("  Device is accessible without ID authentication.\n");
    printf("  This typically means factory default settings (all 0xFF).\n");
    break;
  case OSIS_MODE_LOCKED:
    printf("  Custom ID code has been programmed.\n");
    printf("  Use -i/--id option to authenticate.\n");
    printf("  Use -e/--erase-all if ALeRASE is enabled.\n");
    break;
  case OSIS_MODE_DISABLED:
    printf("  OSIS[127:126] = 00b\n");
    printf("  Serial programming permanently disabled.\n");
    printf("  Device cannot be programmed via bootloader.\n");
    break;
  case OSIS_MODE_UNKNOWN:
    if (status->error_code)
      printf("  Error: MCU returned 0x%02X (%s)\n",
          status->error_code,
          ra_strerror(status->error_code));
    break;
  }
}
