/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Unit tests for rapacker protocol handling
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <errno.h>

#include "../src/rapacker.h"

static void
test_calc_sum(void **state) {
  (void)state;

  uint8_t data1[] = { 0x00 };
  assert_int_equal(ra_calc_sum(0x12, data1, 1), 0xEC);
  assert_int_equal(ra_calc_sum(0x34, data1, 1), 0xCA);
  assert_int_equal(ra_calc_sum(0x00, data1, 1), 0xFE);
}

static void
test_unpack(void **state) {
  (void)state;

  uint8_t data[MAX_DATA_LEN];
  size_t data_len;
  uint8_t cmd;
  ssize_t ret;

  /* Test packet: 0x81 0x00 0x02 0x00 0x00 0xFE 0x03 */
  uint8_t pkt1[] = { 0x81, 0x00, 0x02, 0x00, 0x00, 0xFE, 0x03 };
  ret = ra_unpack_pkt(pkt1, sizeof(pkt1), data, &data_len, &cmd);
  assert_int_equal(ret, 1);
  assert_int_equal(data_len, 1);
  assert_int_equal(data[0], 0x00);

  /* Test packet: 0x81 0x00 0x02 0x12 0x00 0xEC 0x03 */
  uint8_t pkt2[] = { 0x81, 0x00, 0x02, 0x12, 0x00, 0xEC, 0x03 };
  ret = ra_unpack_pkt(pkt2, sizeof(pkt2), data, &data_len, &cmd);
  assert_int_equal(ret, 1);
  assert_int_equal(data[0], 0x00);

  /* Test packet: 0x81 0x00 0x02 0x13 0x00 0xEB 0x03 */
  uint8_t pkt3[] = { 0x81, 0x00, 0x02, 0x13, 0x00, 0xEB, 0x03 };
  ret = ra_unpack_pkt(pkt3, sizeof(pkt3), data, &data_len, &cmd);
  assert_int_equal(ret, 1);
  assert_int_equal(data[0], 0x00);
}

static void
test_read_unpack(void **state) {
  (void)state;

  uint8_t data[MAX_DATA_LEN];
  size_t data_len;
  uint8_t cmd;
  ssize_t ret;

  /* Test packet with multiple data bytes */
  uint8_t pkt[] = { 0x81, 0x00, 0x04, 0x15, 0xAA, 0xBB, 0xCC, 0xB6, 0x03 };
  ret = ra_unpack_pkt(pkt, sizeof(pkt), data, &data_len, &cmd);
  assert_int_equal(ret, 3);
  assert_int_equal(data_len, 3);
  assert_int_equal(data[0], 0xAA);
  assert_int_equal(data[1], 0xBB);
  assert_int_equal(data[2], 0xCC);
}

static void
test_pack_unpack(void **state) {
  (void)state;

  uint8_t buf[MAX_PKT_LEN];
  uint8_t data_out[MAX_DATA_LEN];
  size_t data_len;
  uint8_t cmd;
  ssize_t pkt_len, ret;

  /* Test round-trip: pack then unpack */
  uint8_t data1[] = { 0x00, 0x01, 0x02 };
  pkt_len = ra_pack_pkt(buf, sizeof(buf), 0x13, data1, 3, true);
  assert_true(pkt_len > 0);

  ret = ra_unpack_pkt(buf, pkt_len, data_out, &data_len, &cmd);
  assert_int_equal(ret, 3);
  assert_int_equal(data_len, 3);
  assert_int_equal(data_out[0], 0x00);
  assert_int_equal(data_out[1], 0x01);
  assert_int_equal(data_out[2], 0x02);

  /* Test with single byte */
  uint8_t data2[] = { 0x00 };
  pkt_len = ra_pack_pkt(buf, sizeof(buf), 0x34, data2, 1, true);
  assert_true(pkt_len > 0);
  ret = ra_unpack_pkt(buf, pkt_len, data_out, &data_len, &cmd);
  assert_int_equal(ret, 1);
  assert_int_equal(data_out[0], 0x00);

  pkt_len = ra_pack_pkt(buf, sizeof(buf), 0x00, data2, 1, true);
  assert_true(pkt_len > 0);
  ret = ra_unpack_pkt(buf, pkt_len, data_out, &data_len, &cmd);
  assert_int_equal(ret, 1);

  pkt_len = ra_pack_pkt(buf, sizeof(buf), 0x12, data2, 1, true);
  assert_true(pkt_len > 0);
  ret = ra_unpack_pkt(buf, pkt_len, data_out, &data_len, &cmd);
  assert_int_equal(ret, 1);
}

static void
test_err_unpack(void **state) {
  (void)state;

  uint8_t data[MAX_DATA_LEN];
  size_t data_len;
  uint8_t cmd;
  ssize_t ret;

  /* Error packet: 0x81 0x00 0x02 0x93 0xC3 0x38 0x03 */
  uint8_t pkt[] = { 0x81, 0x00, 0x02, 0x93, 0xC3, 0x38, 0x03 };
  ret = ra_unpack_pkt(pkt, sizeof(pkt), data, &data_len, &cmd);
  assert_int_equal(ret, -1);
  assert_int_equal(errno, EIO);
  assert_int_equal(data[0], 0xC3);
}

static void
test_strerror(void **state) {
  (void)state;

  assert_string_equal(ra_strerror(0xC3), "ERR_FLOW");
  assert_string_equal(ra_strerror(0xE1), "ERR_ERA");
  assert_string_equal(ra_strerror(0xFF), "ERR_UNKNOWN");
}

int
main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_calc_sum),    cmocka_unit_test(test_unpack),
    cmocka_unit_test(test_read_unpack), cmocka_unit_test(test_pack_unpack),
    cmocka_unit_test(test_err_unpack),  cmocka_unit_test(test_strerror),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
