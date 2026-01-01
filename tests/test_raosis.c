/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Unit tests for OSIS (OCD/Serial Programmer ID Setting Register) detection
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>

#include "../src/raosis.h"

/*
 * OSIS mode string tests
 */

static void
test_osis_mode_str(void **state) {
  (void)state;

  /* Test all defined OSIS modes */
  assert_string_equal(ra_osis_mode_str(OSIS_MODE_UNLOCKED),
      "Unlocked (no ID protection)");
  assert_string_equal(ra_osis_mode_str(OSIS_MODE_LOCKED),
      "Locked (ID authentication required)");
  assert_string_equal(ra_osis_mode_str(OSIS_MODE_DISABLED),
      "Disabled (serial programming blocked)");
  assert_string_equal(ra_osis_mode_str(OSIS_MODE_UNKNOWN),
      "Unknown");

  /* Test invalid mode */
  assert_string_equal(ra_osis_mode_str(99), "Invalid");
}

/*
 * OSIS detection tests
 */

static void
test_osis_detect_unlocked(void **state) {
  (void)state;

  ra_device_t dev;
  osis_status_t status;

  memset(&dev, 0, sizeof(dev));
  dev.authenticated = false;  /* No ID auth performed */

  int ret = ra_osis_detect(&dev, &status);
  assert_int_equal(ret, 0);
  assert_int_equal(status.mode, OSIS_MODE_UNLOCKED);
}

static void
test_osis_detect_locked(void **state) {
  (void)state;

  ra_device_t dev;
  osis_status_t status;

  memset(&dev, 0, sizeof(dev));
  dev.authenticated = true;  /* ID auth was performed */

  int ret = ra_osis_detect(&dev, &status);
  assert_int_equal(ret, 0);
  assert_int_equal(status.mode, OSIS_MODE_LOCKED);
}

/*
 * OSIS status structure tests
 */

static void
test_osis_status_init(void **state) {
  (void)state;

  ra_device_t dev;
  osis_status_t status;

  /* Initialize status with garbage */
  memset(&status, 0xFF, sizeof(status));

  memset(&dev, 0, sizeof(dev));
  dev.authenticated = false;

  /* ra_osis_detect should clear the status */
  int ret = ra_osis_detect(&dev, &status);
  assert_int_equal(ret, 0);
  assert_int_equal(status.error_code, 0);
}

/*
 * OSIS mode enum tests
 */

static void
test_osis_mode_enum(void **state) {
  (void)state;

  /* Verify enum values are distinct */
  assert_true(OSIS_MODE_UNLOCKED != OSIS_MODE_LOCKED);
  assert_true(OSIS_MODE_LOCKED != OSIS_MODE_DISABLED);
  assert_true(OSIS_MODE_DISABLED != OSIS_MODE_UNKNOWN);
  assert_true(OSIS_MODE_UNKNOWN != OSIS_MODE_UNLOCKED);
}

int
main(void) {
  const struct CMUnitTest tests[] = {
    /* Mode string tests */
    cmocka_unit_test(test_osis_mode_str),

    /* Detection tests */
    cmocka_unit_test(test_osis_detect_unlocked),
    cmocka_unit_test(test_osis_detect_locked),

    /* Status structure tests */
    cmocka_unit_test(test_osis_status_init),

    /* Enum tests */
    cmocka_unit_test(test_osis_mode_enum),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
