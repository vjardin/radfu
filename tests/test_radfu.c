/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Unit tests for radfu high-level functions
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>

#ifndef TESTING
#define TESTING
#endif
#include "../src/radfu_internal.h"

/*
 * DLM state name tests
 */

static void
test_dlm_state_names(void **state) {
  (void)state;

  /* Test all defined DLM states */
  assert_string_equal(ra_dlm_state_name(0x01), "CM");
  assert_string_equal(ra_dlm_state_name(0x02), "SSD");
  assert_string_equal(ra_dlm_state_name(0x03), "NSECSD");
  assert_string_equal(ra_dlm_state_name(0x04), "DPL");
  assert_string_equal(ra_dlm_state_name(0x05), "LCK_DBG");
  assert_string_equal(ra_dlm_state_name(0x06), "LCK_BOOT");
  assert_string_equal(ra_dlm_state_name(0x07), "RMA_REQ");
  assert_string_equal(ra_dlm_state_name(0x08), "RMA_ACK");

  /* Unknown states */
  assert_string_equal(ra_dlm_state_name(0x00), "UNKNOWN");
  assert_string_equal(ra_dlm_state_name(0x09), "UNKNOWN");
  assert_string_equal(ra_dlm_state_name(0xFF), "UNKNOWN");
}

static void
test_dlm_state_constants(void **state) {
  (void)state;

  /* Verify DLM state constants match spec */
  assert_int_equal(DLM_STATE_CM, 0x01);
  assert_int_equal(DLM_STATE_SSD, 0x02);
  assert_int_equal(DLM_STATE_NSECSD, 0x03);
  assert_int_equal(DLM_STATE_DPL, 0x04);
  assert_int_equal(DLM_STATE_LCK_DBG, 0x05);
  assert_int_equal(DLM_STATE_LCK_BOOT, 0x06);
  assert_int_equal(DLM_STATE_RMA_REQ, 0x07);
  assert_int_equal(DLM_STATE_RMA_ACK, 0x08);
}

/*
 * Format size tests
 */

static void
test_format_size(void **state) {
  (void)state;

  char buf[32];

  /* Bytes */
  format_size(0, buf, sizeof(buf));
  assert_string_equal(buf, "0 bytes");

  format_size(512, buf, sizeof(buf));
  assert_string_equal(buf, "512 bytes");

  format_size(1023, buf, sizeof(buf));
  assert_string_equal(buf, "1023 bytes");

  /* Kilobytes */
  format_size(1024, buf, sizeof(buf));
  assert_string_equal(buf, "1 KB");

  format_size(8192, buf, sizeof(buf));
  assert_string_equal(buf, "8 KB");

  format_size(512 * 1024, buf, sizeof(buf));
  assert_string_equal(buf, "512 KB");

  /* Megabytes */
  format_size(1024 * 1024, buf, sizeof(buf));
  assert_string_equal(buf, "1 MB");

  format_size(2 * 1024 * 1024, buf, sizeof(buf));
  assert_string_equal(buf, "2 MB");
}

/*
 * Area type tests
 */

static void
test_get_area_type(void **state) {
  (void)state;

  /* Code flash range: 0x00000000 - 0x000FFFFF */
  assert_string_equal(get_area_type(0x00000000), "Code Flash");
  assert_string_equal(get_area_type(0x0007FFFF), "Code Flash");
  assert_string_equal(get_area_type(0x000FFFFF), "Code Flash");

  /* Data flash range: 0x08000000 - 0x08FFFFFF */
  assert_string_equal(get_area_type(0x08000000), "Data Flash");
  assert_string_equal(get_area_type(0x0807FFFF), "Data Flash");

  /* Config range: 0x01000000 - 0x01FFFFFF */
  assert_string_equal(get_area_type(0x01000000), "Config");
  assert_string_equal(get_area_type(0x0100FFFF), "Config");

  /* Unknown range */
  assert_string_equal(get_area_type(0x20000000), "Unknown");
  assert_string_equal(get_area_type(0xFFFFFFFF), "Unknown");
}

static void
test_get_area_type_koa(void **state) {
  (void)state;

  /* KOA format: 0xTN where T=type, N=area index */

  /* Type 0: User/Code */
  assert_string_equal(get_area_type_koa(KOA_TYPE_CODE << 4 | 0x00), "User/Code");
  assert_string_equal(get_area_type_koa(KOA_TYPE_CODE << 4 | 0x01), "User/Code");
  assert_string_equal(get_area_type_koa(KOA_TYPE_CODE << 4 | 0x0F), "User/Code");

  /* Type 1: Data */
  assert_string_equal(get_area_type_koa(KOA_TYPE_DATA << 4 | 0x00), "Data");
  assert_string_equal(get_area_type_koa(KOA_TYPE_DATA << 4 | 0x01), "Data");
  assert_string_equal(get_area_type_koa(KOA_TYPE_DATA << 4 | 0x0F), "Data");

  /* Type 2: Config */
  assert_string_equal(get_area_type_koa(KOA_TYPE_CONFIG << 4 | 0x00), "Config");
  assert_string_equal(get_area_type_koa(KOA_TYPE_CONFIG << 4 | 0x01), "Config");
  assert_string_equal(get_area_type_koa(KOA_TYPE_CONFIG << 4 | 0x0F), "Config");

  /* Unknown types */
  assert_string_equal(get_area_type_koa(0x30), "Unknown");
  assert_string_equal(get_area_type_koa(0xFF), "Unknown");
}

/*
 * Area lookup tests
 */

static void
test_find_area_for_address(void **state) {
  (void)state;

  ra_device_t dev;
  memset(&dev, 0, sizeof(dev));

  /* Setup typical RA4M2 memory layout */
  dev.chip_layout[0].sad = 0x00000000;
  dev.chip_layout[0].ead = 0x0007FFFF; /* 512KB Code Flash */
  dev.chip_layout[0].eau = 0x2000;     /* 8KB erase unit */
  dev.chip_layout[0].wau = 0x80;       /* 128B write unit */
  dev.chip_layout[0].rau = 0x04;       /* 4B read unit */
  dev.chip_layout[0].cau = 0x04;       /* 4B CRC unit */

  dev.chip_layout[1].sad = 0x08000000;
  dev.chip_layout[1].ead = 0x08001FFF; /* 8KB Data Flash */
  dev.chip_layout[1].eau = 0x40;       /* 64B erase unit */
  dev.chip_layout[1].wau = 0x04;       /* 4B write unit */
  dev.chip_layout[1].rau = 0x04;
  dev.chip_layout[1].cau = 0x04;

  dev.chip_layout[2].sad = 0x01000000;
  dev.chip_layout[2].ead = 0x010001FF; /* Config area */
  dev.chip_layout[2].eau = 0;          /* No erase */
  dev.chip_layout[2].wau = 0x04;
  dev.chip_layout[2].rau = 0x04;
  dev.chip_layout[2].cau = 0x04;

  /* Area 3 not used */
  dev.chip_layout[3].sad = 0;
  dev.chip_layout[3].ead = 0;

  /* Test area lookup */
  assert_int_equal(find_area_for_address(&dev, 0x00000000), 0);
  assert_int_equal(find_area_for_address(&dev, 0x00040000), 0);
  assert_int_equal(find_area_for_address(&dev, 0x0007FFFF), 0);

  assert_int_equal(find_area_for_address(&dev, 0x08000000), 1);
  assert_int_equal(find_area_for_address(&dev, 0x08001000), 1);
  assert_int_equal(find_area_for_address(&dev, 0x08001FFF), 1);

  assert_int_equal(find_area_for_address(&dev, 0x01000000), 2);
  assert_int_equal(find_area_for_address(&dev, 0x010001FF), 2);

  /* Address not in any area */
  assert_int_equal(find_area_for_address(&dev, 0x00080000), -1);
  assert_int_equal(find_area_for_address(&dev, 0x08002000), -1);
  assert_int_equal(find_area_for_address(&dev, 0x20000000), -1);
}

/*
 * Boundary validation tests
 */

static ra_device_t *
setup_test_device(void) {
  static ra_device_t dev;
  memset(&dev, 0, sizeof(dev));

  /* Code Flash: 512KB, 8KB erase, 128B write */
  dev.chip_layout[0].sad = 0x00000000;
  dev.chip_layout[0].ead = 0x0007FFFF;
  dev.chip_layout[0].eau = 0x2000; /* 8KB */
  dev.chip_layout[0].wau = 0x80;   /* 128B */
  dev.chip_layout[0].rau = 0x04;   /* 4B */
  dev.chip_layout[0].cau = 0x04;   /* 4B */

  /* Data Flash: 8KB, 64B erase, 4B write */
  dev.chip_layout[1].sad = 0x08000000;
  dev.chip_layout[1].ead = 0x08001FFF;
  dev.chip_layout[1].eau = 0x40; /* 64B */
  dev.chip_layout[1].wau = 0x04; /* 4B */
  dev.chip_layout[1].rau = 0x04;
  dev.chip_layout[1].cau = 0x04;

  /* Config: no erase */
  dev.chip_layout[2].sad = 0x01000000;
  dev.chip_layout[2].ead = 0x010001FF;
  dev.chip_layout[2].eau = 0; /* No erase */
  dev.chip_layout[2].wau = 0x04;
  dev.chip_layout[2].rau = 0x04;
  dev.chip_layout[2].cau = 0x04;

  return &dev;
}

static void
test_erase_boundaries_aligned(void **state) {
  (void)state;

  ra_device_t *dev = setup_test_device();
  uint32_t end;

  /* Aligned erase should succeed */
  assert_int_equal(set_erase_boundaries(dev, 0x00000000, 0x2000, &end), 0);
  assert_int_equal(end, 0x00001FFF);

  /* Multiple blocks */
  assert_int_equal(set_erase_boundaries(dev, 0x00000000, 0x10000, &end), 0);
  assert_int_equal(end, 0x0000FFFF);
}

static void
test_erase_boundaries_unaligned(void **state) {
  (void)state;

  ra_device_t *dev = setup_test_device();
  uint32_t end;

  /* Unaligned start should fail */
  assert_int_equal(set_erase_boundaries(dev, 0x00000100, 0x2000, &end), -1);
  assert_int_equal(set_erase_boundaries(dev, 0x00001000, 0x2000, &end), -1);
}

static void
test_erase_boundaries_no_erase_area(void **state) {
  (void)state;

  ra_device_t *dev = setup_test_device();
  uint32_t end;

  /* Config area has no erase support */
  assert_int_equal(set_erase_boundaries(dev, 0x01000000, 0x100, &end), -1);
}

static void
test_write_boundaries_aligned(void **state) {
  (void)state;

  ra_device_t *dev = setup_test_device();
  uint32_t end;

  /* Aligned write should succeed */
  assert_int_equal(set_write_boundaries(dev, 0x00000000, 0x80, &end), 0);
  assert_int_equal(end, 0x0000007F);

  /* Multiple blocks */
  assert_int_equal(set_write_boundaries(dev, 0x00000000, 0x100, &end), 0);
  assert_int_equal(end, 0x000000FF);
}

static void
test_write_boundaries_unaligned(void **state) {
  (void)state;

  ra_device_t *dev = setup_test_device();
  uint32_t end;

  /* Unaligned start should fail */
  assert_int_equal(set_write_boundaries(dev, 0x00000001, 0x80, &end), -1);
  assert_int_equal(set_write_boundaries(dev, 0x00000040, 0x80, &end), -1);
}

static void
test_read_boundaries_aligned(void **state) {
  (void)state;

  ra_device_t *dev = setup_test_device();
  uint32_t end;

  /* Aligned read should succeed */
  assert_int_equal(set_read_boundaries(dev, 0x00000000, 0x100, &end), 0);
  assert_int_equal(end, 0x000000FF);
}

static void
test_read_boundaries_unaligned(void **state) {
  (void)state;

  ra_device_t *dev = setup_test_device();
  uint32_t end;

  /* Unaligned start should fail (RAU = 4) */
  assert_int_equal(set_read_boundaries(dev, 0x00000001, 0x100, &end), -1);
  assert_int_equal(set_read_boundaries(dev, 0x00000002, 0x100, &end), -1);
}

static void
test_crc_boundaries_aligned(void **state) {
  (void)state;

  ra_device_t *dev = setup_test_device();
  uint32_t end;

  /* Aligned CRC should succeed */
  assert_int_equal(set_crc_boundaries(dev, 0x00000000, 0x100, &end), 0);
  assert_int_equal(end, 0x000000FF);
}

static void
test_crc_boundaries_unaligned(void **state) {
  (void)state;

  ra_device_t *dev = setup_test_device();
  uint32_t end;

  /* Unaligned start should fail (CAU = 4) */
  assert_int_equal(set_crc_boundaries(dev, 0x00000001, 0x100, &end), -1);
}

static void
test_boundaries_exceed_area(void **state) {
  (void)state;

  ra_device_t *dev = setup_test_device();
  uint32_t end;

  /* Size exceeds area should fail */
  assert_int_equal(set_erase_boundaries(dev, 0x00000000, 0x100000, &end), -1);
  assert_int_equal(set_write_boundaries(dev, 0x00000000, 0x100000, &end), -1);
}

static void
test_boundaries_unknown_address(void **state) {
  (void)state;

  ra_device_t *dev = setup_test_device();
  uint32_t end;

  /* Address not in any known area */
  assert_int_equal(set_erase_boundaries(dev, 0x20000000, 0x1000, &end), -1);
  assert_int_equal(set_write_boundaries(dev, 0x20000000, 0x1000, &end), -1);
  assert_int_equal(set_read_boundaries(dev, 0x20000000, 0x1000, &end), -1);
  assert_int_equal(set_crc_boundaries(dev, 0x20000000, 0x1000, &end), -1);
}

/*
 * Parameter constants tests
 */

static void
test_param_constants(void **state) {
  (void)state;

  assert_int_equal(PARAM_ID_INIT, 0x01);
  assert_int_equal(PARAM_INIT_DISABLED, 0x00);
  assert_int_equal(PARAM_INIT_ENABLED, 0x07);
}

int
main(void) {
  const struct CMUnitTest tests[] = {
    /* DLM state tests */
    cmocka_unit_test(test_dlm_state_names),
    cmocka_unit_test(test_dlm_state_constants),

    /* Format size tests */
    cmocka_unit_test(test_format_size),

    /* Area type tests */
    cmocka_unit_test(test_get_area_type),
    cmocka_unit_test(test_get_area_type_koa),

    /* Area lookup tests */
    cmocka_unit_test(test_find_area_for_address),

    /* Boundary validation tests */
    cmocka_unit_test(test_erase_boundaries_aligned),
    cmocka_unit_test(test_erase_boundaries_unaligned),
    cmocka_unit_test(test_erase_boundaries_no_erase_area),
    cmocka_unit_test(test_write_boundaries_aligned),
    cmocka_unit_test(test_write_boundaries_unaligned),
    cmocka_unit_test(test_read_boundaries_aligned),
    cmocka_unit_test(test_read_boundaries_unaligned),
    cmocka_unit_test(test_crc_boundaries_aligned),
    cmocka_unit_test(test_crc_boundaries_unaligned),
    cmocka_unit_test(test_boundaries_exceed_area),
    cmocka_unit_test(test_boundaries_unknown_address),

    /* Parameter constants */
    cmocka_unit_test(test_param_constants),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
