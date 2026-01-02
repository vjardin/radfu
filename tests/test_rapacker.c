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
#include <string.h>

#include "../src/rapacker.h"

/*
 * Checksum calculation tests
 */

static void
test_calc_sum(void **state) {
  (void)state;

  uint8_t data1[] = { 0x00 };
  assert_int_equal(ra_calc_sum(ERA_CMD, data1, 1), 0xEC);
  assert_int_equal(ra_calc_sum(BAU_CMD, data1, 1), 0xCA);
  assert_int_equal(ra_calc_sum(INQ_CMD, data1, 1), 0xFE);
}

static void
test_calc_sum_empty(void **state) {
  (void)state;

  /* Empty data - checksum based only on cmd and length
   * For empty data: pkt_len=1, sum = 0 + 1 + cmd, then two's complement
   * SIG (0x3A): sum = 0x3B, ~(0x3B-1) & 0xFF = 0xC5
   * DLM (0x2C): sum = 0x2D, ~(0x2D-1) & 0xFF = 0xD3
   */
  assert_int_equal(ra_calc_sum(SIG_CMD, NULL, 0), 0xC5);
  assert_int_equal(ra_calc_sum(DLM_CMD, NULL, 0), 0xD3);
}

static void
test_calc_sum_multidata(void **state) {
  (void)state;

  /* Multiple data bytes */
  uint8_t data[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xFF, 0xFF };
  /* Erase command with address range */
  uint8_t sum = ra_calc_sum(ERA_CMD, data, 8);
  /* Verify checksum is non-zero and valid */
  assert_true(sum != 0);
}

/*
 * Endian conversion tests
 */

static void
test_endian_uint32(void **state) {
  (void)state;

  uint8_t buf[4];

  /* Test known values */
  uint32_to_be(0x12345678, buf);
  assert_int_equal(buf[0], 0x12);
  assert_int_equal(buf[1], 0x34);
  assert_int_equal(buf[2], 0x56);
  assert_int_equal(buf[3], 0x78);

  /* Round-trip */
  assert_int_equal(be_to_uint32(buf), 0x12345678);

  /* Zero */
  uint32_to_be(0x00000000, buf);
  assert_int_equal(be_to_uint32(buf), 0x00000000);

  /* Max value */
  uint32_to_be(0xFFFFFFFF, buf);
  assert_int_equal(be_to_uint32(buf), 0xFFFFFFFF);

  /* Typical addresses */
  uint32_to_be(0x00000000, buf); /* Code flash start */
  assert_int_equal(be_to_uint32(buf), 0x00000000);

  uint32_to_be(0x0807FFFF, buf); /* Data flash end */
  assert_int_equal(be_to_uint32(buf), 0x0807FFFF);
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
  pkt_len = ra_pack_pkt(buf, sizeof(buf), WRI_CMD, data1, 3, true);
  assert_true(pkt_len > 0);

  ret = ra_unpack_pkt(buf, pkt_len, data_out, &data_len, &cmd);
  assert_int_equal(ret, 3);
  assert_int_equal(data_len, 3);
  assert_int_equal(data_out[0], 0x00);
  assert_int_equal(data_out[1], 0x01);
  assert_int_equal(data_out[2], 0x02);

  /* Test with single byte */
  uint8_t data2[] = { 0x00 };
  pkt_len = ra_pack_pkt(buf, sizeof(buf), BAU_CMD, data2, 1, true);
  assert_true(pkt_len > 0);
  ret = ra_unpack_pkt(buf, pkt_len, data_out, &data_len, &cmd);
  assert_int_equal(ret, 1);
  assert_int_equal(data_out[0], 0x00);

  pkt_len = ra_pack_pkt(buf, sizeof(buf), INQ_CMD, data2, 1, true);
  assert_true(pkt_len > 0);
  ret = ra_unpack_pkt(buf, pkt_len, data_out, &data_len, &cmd);
  assert_int_equal(ret, 1);

  pkt_len = ra_pack_pkt(buf, sizeof(buf), ERA_CMD, data2, 1, true);
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

  /* Error packet: SOD_ACK LNH LNL (WRI|0x80) ERR_FLOW SUM ETX */
  uint8_t pkt[] = { SOD_ACK, 0x00, 0x02, WRI_CMD | STATUS_ERR, ERR_FLOW, 0x38, ETX };
  ret = ra_unpack_pkt(pkt, sizeof(pkt), data, &data_len, &cmd);
  assert_int_equal(ret, -1);
  assert_int_equal(errno, EIO);
  assert_int_equal(data[0], ERR_FLOW);
}

static void
test_strerror(void **state) {
  (void)state;

  assert_string_equal(ra_strerror(ERR_FLOW), "ERR_FLOW");
  assert_string_equal(ra_strerror(ERR_ERA), "ERR_ERA");
  assert_string_equal(ra_strerror(0xFF), "ERR_UNKNOWN");
}

/*
 * Complete error code coverage
 */

static void
test_all_error_codes(void **state) {
  (void)state;

  /* Test all defined error codes */
  assert_string_equal(ra_strerror(ERR_UNSU), "ERR_UNSU");
  assert_string_equal(ra_strerror(ERR_PCKT), "ERR_PCKT");
  assert_string_equal(ra_strerror(ERR_CHKS), "ERR_CHKS");
  assert_string_equal(ra_strerror(ERR_FLOW), "ERR_FLOW");
  assert_string_equal(ra_strerror(ERR_ADDR), "ERR_ADDR");
  assert_string_equal(ra_strerror(ERR_BAUD), "ERR_BAUD");
  assert_string_equal(ra_strerror(ERR_CMD), "ERR_CMD");
  assert_string_equal(ra_strerror(ERR_PROT), "ERR_PROT");
  assert_string_equal(ra_strerror(ERR_ID), "ERR_ID");
  assert_string_equal(ra_strerror(ERR_SERI), "ERR_SERI");
  assert_string_equal(ra_strerror(ERR_ERA), "ERR_ERA");
  assert_string_equal(ra_strerror(ERR_WRI), "ERR_WRI");
  assert_string_equal(ra_strerror(ERR_SEQ), "ERR_SEQ");

  /* Unknown codes return ERR_UNKNOWN */
  assert_string_equal(ra_strerror(0x00), "ERR_UNKNOWN");
  assert_string_equal(ra_strerror(0xFF), "ERR_UNKNOWN");
  assert_string_equal(ra_strerror(0x99), "ERR_UNKNOWN");
}

static void
test_strdesc(void **state) {
  (void)state;

  /* Test error descriptions */
  assert_string_equal(ra_strdesc(ERR_UNSU), "unsupported command");
  assert_string_equal(ra_strdesc(ERR_PCKT), "packet error (length/ETX)");
  assert_string_equal(ra_strdesc(ERR_CHKS), "checksum mismatch");
  assert_string_equal(ra_strdesc(ERR_FLOW), "command flow error");
  assert_string_equal(ra_strdesc(ERR_ADDR), "invalid address");
  assert_string_equal(ra_strdesc(ERR_BAUD), "baud rate margin error");
  assert_string_equal(ra_strdesc(ERR_CMD), "command not accepted (wrong state)");
  assert_string_equal(ra_strdesc(ERR_PROT), "protection error");
  assert_string_equal(ra_strdesc(ERR_ID), "ID authentication mismatch");
  assert_string_equal(ra_strdesc(ERR_SERI), "serial programming disabled");
  assert_string_equal(ra_strdesc(ERR_ERA), "erase failed");
  assert_string_equal(ra_strdesc(ERR_WRI), "write failed");
  assert_string_equal(ra_strdesc(ERR_SEQ), "sequencer error");

  /* Unknown codes */
  assert_string_equal(ra_strdesc(0xFF), "unknown error");
}

/*
 * Pack edge cases
 */

static void
test_pack_empty_data(void **state) {
  (void)state;

  uint8_t buf[MAX_PKT_LEN];
  ssize_t pkt_len;

  /* Pack with no data (like SIG command) */
  pkt_len = ra_pack_pkt(buf, sizeof(buf), SIG_CMD, NULL, 0, false);
  assert_true(pkt_len > 0);
  assert_int_equal(pkt_len, 6); /* SOD + LNH + LNL + CMD + SUM + ETX */
  assert_int_equal(buf[0], SOD_CMD);
  assert_int_equal(buf[3], SIG_CMD);
  assert_int_equal(buf[5], ETX);
}

static void
test_pack_buffer_too_small(void **state) {
  (void)state;

  uint8_t buf[5]; /* Too small for any packet */
  uint8_t data[] = { 0x00 };
  ssize_t pkt_len;

  pkt_len = ra_pack_pkt(buf, sizeof(buf), ERA_CMD, data, 1, false);
  assert_int_equal(pkt_len, -1);
  assert_int_equal(errno, ENOBUFS);
}

static void
test_pack_data_too_long(void **state) {
  (void)state;

  uint8_t buf[MAX_PKT_LEN];
  uint8_t data[MAX_DATA_LEN + 1];
  ssize_t pkt_len;

  memset(data, 0xAA, sizeof(data));
  pkt_len = ra_pack_pkt(buf, sizeof(buf), WRI_CMD, data, sizeof(data), false);
  assert_int_equal(pkt_len, -1);
  assert_int_equal(errno, EINVAL);
}

static void
test_pack_max_data(void **state) {
  (void)state;

  uint8_t buf[MAX_PKT_LEN];
  uint8_t data[MAX_DATA_LEN];
  ssize_t pkt_len;

  memset(data, 0x55, sizeof(data));
  pkt_len = ra_pack_pkt(buf, sizeof(buf), WRI_CMD, data, MAX_DATA_LEN, false);
  assert_true(pkt_len > 0);
  assert_int_equal(pkt_len, MAX_DATA_LEN + 6);
}

static void
test_pack_ack_vs_cmd(void **state) {
  (void)state;

  uint8_t buf_cmd[MAX_PKT_LEN];
  uint8_t buf_ack[MAX_PKT_LEN];
  uint8_t data[] = { 0x00 };

  ra_pack_pkt(buf_cmd, sizeof(buf_cmd), REA_CMD, data, 1, false);
  ra_pack_pkt(buf_ack, sizeof(buf_ack), REA_CMD, data, 1, true);

  assert_int_equal(buf_cmd[0], SOD_CMD);
  assert_int_equal(buf_ack[0], SOD_ACK);
}

/*
 * Unpack edge cases
 */

static void
test_unpack_too_short(void **state) {
  (void)state;

  uint8_t data[MAX_DATA_LEN];
  size_t data_len;
  uint8_t cmd;
  ssize_t ret;

  /* Buffer too short */
  uint8_t pkt[] = { 0x81, 0x00, 0x01 };
  ret = ra_unpack_pkt(pkt, sizeof(pkt), data, &data_len, &cmd);
  assert_int_equal(ret, -1);
  assert_int_equal(errno, EINVAL);
}

static void
test_unpack_bad_sod(void **state) {
  (void)state;

  uint8_t data[MAX_DATA_LEN];
  size_t data_len;
  uint8_t cmd;
  ssize_t ret;

  /* Wrong SOD byte */
  uint8_t pkt[] = { 0x01, 0x00, 0x02, 0x00, 0x00, 0xFE, 0x03 };
  ret = ra_unpack_pkt(pkt, sizeof(pkt), data, &data_len, &cmd);
  assert_int_equal(ret, -1);
  assert_int_equal(errno, EPROTO);
}

static void
test_unpack_bad_etx(void **state) {
  (void)state;

  uint8_t data[MAX_DATA_LEN];
  size_t data_len;
  uint8_t cmd;
  ssize_t ret;

  /* Wrong ETX byte */
  uint8_t pkt[] = { 0x81, 0x00, 0x02, 0x00, 0x00, 0xFE, 0xFF };
  ret = ra_unpack_pkt(pkt, sizeof(pkt), data, &data_len, &cmd);
  assert_int_equal(ret, -1);
  assert_int_equal(errno, EPROTO);
}

static void
test_unpack_bad_checksum(void **state) {
  (void)state;

  uint8_t data[MAX_DATA_LEN];
  size_t data_len;
  uint8_t cmd;
  ssize_t ret;

  /* Bad checksum */
  uint8_t pkt[] = { 0x81, 0x00, 0x02, 0x00, 0x00, 0xFF, 0x03 };
  ret = ra_unpack_pkt(pkt, sizeof(pkt), data, &data_len, &cmd);
  assert_int_equal(ret, -1);
  assert_int_equal(errno, EBADMSG);
}

static void
test_unpack_zero_pkt_len(void **state) {
  (void)state;

  uint8_t data[MAX_DATA_LEN];
  size_t data_len;
  uint8_t cmd;
  ssize_t ret;

  /* Zero packet length in header */
  uint8_t pkt[] = { 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03 };
  ret = ra_unpack_pkt(pkt, sizeof(pkt), data, &data_len, &cmd);
  assert_int_equal(ret, -1);
  assert_int_equal(errno, EPROTO);
}

static void
test_unpack_length_mismatch(void **state) {
  (void)state;

  uint8_t data[MAX_DATA_LEN];
  size_t data_len;
  uint8_t cmd;
  ssize_t ret;

  /* Length field says more data than buffer has */
  uint8_t pkt[] = { 0x81, 0x00, 0x10, 0x00, 0x00, 0xFE, 0x03 };
  ret = ra_unpack_pkt(pkt, sizeof(pkt), data, &data_len, &cmd);
  assert_int_equal(ret, -1);
  assert_int_equal(errno, EINVAL);
}

/*
 * Command constant tests
 */

static void
test_command_constants(void **state) {
  (void)state;

  /* Verify command constants match spec */
  assert_int_equal(INQ_CMD, 0x00);
  assert_int_equal(ERA_CMD, 0x12);
  assert_int_equal(WRI_CMD, 0x13);
  assert_int_equal(REA_CMD, 0x15);
  assert_int_equal(CRC_CMD, 0x18);
  assert_int_equal(KEY_CMD, 0x28);
  assert_int_equal(KEY_VFY_CMD, 0x29);
  assert_int_equal(UKEY_CMD, 0x2A);
  assert_int_equal(UKEY_VFY_CMD, 0x2B);
  assert_int_equal(DLM_CMD, 0x2C);
  assert_int_equal(IDA_CMD, 0x30);
  assert_int_equal(BAU_CMD, 0x34);
  assert_int_equal(SIG_CMD, 0x3A);
  assert_int_equal(ARE_CMD, 0x3B);
  assert_int_equal(BND_SET_CMD, 0x4E);
  assert_int_equal(BND_CMD, 0x4F);
  assert_int_equal(INI_CMD, 0x50);
  assert_int_equal(PRM_SET_CMD, 0x51);
  assert_int_equal(PRM_CMD, 0x52);
  assert_int_equal(DLM_TRANSIT_CMD, 0x71);
}

static void
test_protocol_constants(void **state) {
  (void)state;

  assert_int_equal(SOD_CMD, 0x01);
  assert_int_equal(SOD_ACK, 0x81);
  assert_int_equal(ETX, 0x03);
  assert_int_equal(STATUS_OK, 0x00);
  assert_int_equal(STATUS_ERR, 0x80);
}

int
main(void) {
  const struct CMUnitTest tests[] = {
    /* Checksum tests */
    cmocka_unit_test(test_calc_sum),
    cmocka_unit_test(test_calc_sum_empty),
    cmocka_unit_test(test_calc_sum_multidata),

    /* Endian conversion tests */
    cmocka_unit_test(test_endian_uint32),

    /* Unpack tests */
    cmocka_unit_test(test_unpack),
    cmocka_unit_test(test_read_unpack),
    cmocka_unit_test(test_pack_unpack),
    cmocka_unit_test(test_err_unpack),

    /* Error code tests */
    cmocka_unit_test(test_strerror),
    cmocka_unit_test(test_all_error_codes),
    cmocka_unit_test(test_strdesc),

    /* Pack edge cases */
    cmocka_unit_test(test_pack_empty_data),
    cmocka_unit_test(test_pack_buffer_too_small),
    cmocka_unit_test(test_pack_data_too_long),
    cmocka_unit_test(test_pack_max_data),
    cmocka_unit_test(test_pack_ack_vs_cmd),

    /* Unpack edge cases */
    cmocka_unit_test(test_unpack_too_short),
    cmocka_unit_test(test_unpack_bad_sod),
    cmocka_unit_test(test_unpack_bad_etx),
    cmocka_unit_test(test_unpack_bad_checksum),
    cmocka_unit_test(test_unpack_zero_pkt_len),
    cmocka_unit_test(test_unpack_length_mismatch),

    /* Constant verification */
    cmocka_unit_test(test_command_constants),
    cmocka_unit_test(test_protocol_constants),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
