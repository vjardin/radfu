/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Protocol-level tests using mock device
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>

#include "mock/ramock.h"
#include "../src/rapacker.h"

/*
 * Mock response building tests
 */

static void
test_mock_build_sig_response(void **state) {
  (void)state;

  uint8_t buf[256];
  size_t len;

  len = ra_mock_build_sig_response(buf, sizeof(buf),
      1000000,        /* max baud */
      4,              /* num areas */
      0x01,           /* typ: GrpA/GrpB */
      1, 0, 0,        /* BFV 1.0.0 */
      "R7FA4M2AD3CFP");

  assert_true(len > 0);

  /* Verify packet structure */
  assert_int_equal(buf[0], SOD_ACK);
  assert_int_equal(buf[len - 1], ETX);

  /* Unpack and verify */
  uint8_t data[64];
  size_t data_len;
  uint8_t cmd;
  ssize_t ret = ra_unpack_pkt(buf, len, data, &data_len, &cmd);
  assert_true(ret > 0);
  assert_int_equal(cmd, SIG_CMD);
  assert_int_equal(data_len, 41);

  /* Verify RMB (max baud) */
  uint32_t rmb = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
                 ((uint32_t)data[2] << 8) | data[3];
  assert_int_equal(rmb, 1000000);

  /* Verify NOA */
  assert_int_equal(data[4], 4);

  /* Verify TYP */
  assert_int_equal(data[5], 0x01);

  /* Verify BFV */
  assert_int_equal(data[6], 1);
  assert_int_equal(data[7], 0);
  assert_int_equal(data[8], 0);

  /* Verify product name starts correctly */
  assert_true(memcmp(&data[25], "R7FA4M2AD3CFP", 13) == 0);
}

static void
test_mock_build_area_response(void **state) {
  (void)state;

  uint8_t buf[64];
  size_t len;

  len = ra_mock_build_area_response(buf, sizeof(buf),
      0x00,           /* KOA: User/Code area 0 */
      0x00000000,     /* SAD */
      0x0007FFFF,     /* EAD: 512KB */
      0x00002000,     /* EAU: 8KB */
      0x00000080,     /* WAU: 128B */
      0x00000004,     /* RAU: 4B */
      0x00000004);    /* CAU: 4B */

  assert_true(len > 0);

  /* Unpack and verify */
  uint8_t data[32];
  size_t data_len;
  uint8_t cmd;
  ssize_t ret = ra_unpack_pkt(buf, len, data, &data_len, &cmd);
  assert_true(ret > 0);
  assert_int_equal(cmd, ARE_CMD);
  assert_int_equal(data_len, 25);

  /* Verify KOA */
  assert_int_equal(data[0], 0x00);

  /* Verify SAD */
  uint32_t sad = ((uint32_t)data[1] << 24) | ((uint32_t)data[2] << 16) |
                 ((uint32_t)data[3] << 8) | data[4];
  assert_int_equal(sad, 0x00000000);

  /* Verify EAD */
  uint32_t ead = ((uint32_t)data[5] << 24) | ((uint32_t)data[6] << 16) |
                 ((uint32_t)data[7] << 8) | data[8];
  assert_int_equal(ead, 0x0007FFFF);

  /* Verify EAU */
  uint32_t eau = ((uint32_t)data[9] << 24) | ((uint32_t)data[10] << 16) |
                 ((uint32_t)data[11] << 8) | data[12];
  assert_int_equal(eau, 0x00002000);
}

static void
test_mock_build_dlm_response(void **state) {
  (void)state;

  uint8_t buf[32];
  size_t len;

  len = ra_mock_build_dlm_response(buf, sizeof(buf), 0x02); /* SSD state */
  assert_true(len > 0);

  uint8_t data[8];
  size_t data_len;
  uint8_t cmd;
  ssize_t ret = ra_unpack_pkt(buf, len, data, &data_len, &cmd);
  assert_true(ret > 0);
  assert_int_equal(cmd, DLM_CMD);
  assert_int_equal(data_len, 1);
  assert_int_equal(data[0], 0x02);
}

static void
test_mock_build_boundary_response(void **state) {
  (void)state;

  uint8_t buf[32];
  size_t len;

  len = ra_mock_build_boundary_response(buf, sizeof(buf),
      64,   /* CFS1: 64KB */
      128,  /* CFS2: 128KB */
      8,    /* DFS: 8KB */
      16,   /* SRS1: 16KB */
      32);  /* SRS2: 32KB */

  assert_true(len > 0);

  uint8_t data[16];
  size_t data_len;
  uint8_t cmd;
  ssize_t ret = ra_unpack_pkt(buf, len, data, &data_len, &cmd);
  assert_true(ret > 0);
  assert_int_equal(cmd, BND_CMD);
  assert_int_equal(data_len, 10);

  /* Verify CFS1 */
  uint16_t cfs1 = ((uint16_t)data[0] << 8) | data[1];
  assert_int_equal(cfs1, 64);

  /* Verify CFS2 */
  uint16_t cfs2 = ((uint16_t)data[2] << 8) | data[3];
  assert_int_equal(cfs2, 128);

  /* Verify DFS */
  uint16_t dfs = ((uint16_t)data[4] << 8) | data[5];
  assert_int_equal(dfs, 8);
}

/*
 * Mock send/recv tests
 */

static void
test_mock_send_recv(void **state) {
  (void)state;

  ra_mock_t mock;
  ra_mock_init(&mock);

  /* Add a response */
  uint8_t response[] = { 0x81, 0x00, 0x02, 0x00, 0x00, 0xFE, 0x03 };
  ra_mock_add_response(&mock, response, sizeof(response));

  /* Send a packet */
  uint8_t send_data[] = { 0x01, 0x00, 0x01, 0x3A, 0xC5, 0x03 }; /* SIG command */
  ssize_t ret = ra_mock_send(&mock, send_data, sizeof(send_data));
  assert_int_equal(ret, sizeof(send_data));
  assert_int_equal(mock.sent_count, 1);

  /* Verify sent packet */
  assert_int_equal(ra_mock_verify_sent(&mock, 0, send_data, sizeof(send_data)), 0);
  assert_int_equal(ra_mock_verify_sent_cmd(&mock, 0, 0x3A), 0);

  /* Receive response */
  uint8_t recv_buf[32];
  ret = ra_mock_recv(&mock, recv_buf, sizeof(recv_buf), 100);
  assert_int_equal(ret, sizeof(response));
  assert_memory_equal(recv_buf, response, sizeof(response));
}

static void
test_mock_multiple_responses(void **state) {
  (void)state;

  ra_mock_t mock;
  ra_mock_init(&mock);

  /* Add multiple responses (simulating area info query) */
  uint8_t buf[64];

  ra_mock_build_area_response(buf, sizeof(buf), 0x00, 0x00000000, 0x0007FFFF,
      0x2000, 0x80, 0x04, 0x04);
  ra_mock_add_response(&mock, buf, 31);

  ra_mock_build_area_response(buf, sizeof(buf), 0x10, 0x08000000, 0x08001FFF,
      0x40, 0x04, 0x04, 0x04);
  ra_mock_add_response(&mock, buf, 31);

  assert_int_equal(mock.response_count, 2);

  /* Receive first response */
  uint8_t recv_buf[64];
  ssize_t ret = ra_mock_recv(&mock, recv_buf, sizeof(recv_buf), 100);
  assert_true(ret > 0);
  assert_int_equal(mock.current_response, 1);

  /* Receive second response */
  ret = ra_mock_recv(&mock, recv_buf, sizeof(recv_buf), 100);
  assert_true(ret > 0);
  assert_int_equal(mock.current_response, 2);

  /* No more responses */
  ret = ra_mock_recv(&mock, recv_buf, sizeof(recv_buf), 100);
  assert_int_equal(ret, 0);
}

static void
test_mock_error_simulation(void **state) {
  (void)state;

  ra_mock_t mock;
  ra_mock_init(&mock);

  /* Test send failure */
  mock.fail_send = 1;
  uint8_t data[] = { 0x01, 0x02, 0x03 };
  assert_int_equal(ra_mock_send(&mock, data, sizeof(data)), -1);

  /* Test recv failure */
  mock.fail_send = 0;
  mock.fail_recv = 1;
  uint8_t recv_buf[32];
  assert_int_equal(ra_mock_recv(&mock, recv_buf, sizeof(recv_buf), 100), -1);

  /* Test recv timeout */
  mock.fail_recv = 0;
  mock.timeout_recv = 1;
  assert_int_equal(ra_mock_recv(&mock, recv_buf, sizeof(recv_buf), 100), 0);
}

static void
test_mock_add_error_response(void **state) {
  (void)state;

  ra_mock_t mock;
  ra_mock_init(&mock);

  /* Add an error response */
  ra_mock_add_error_response(&mock, 0xD0); /* ERR_ADDR */

  assert_int_equal(mock.response_count, 1);

  /* Receive and parse the error */
  uint8_t recv_buf[32];
  ssize_t ret = ra_mock_recv(&mock, recv_buf, sizeof(recv_buf), 100);
  assert_true(ret > 0);

  uint8_t data[16];
  size_t data_len;
  uint8_t cmd;
  ret = ra_unpack_pkt(recv_buf, ret, data, &data_len, &cmd);
  assert_int_equal(ret, -1); /* Should fail due to error bit */
  assert_true(cmd & STATUS_ERR);
  assert_int_equal(data[0], 0xD0);
}

/*
 * Protocol packet round-trip tests
 */

static void
test_protocol_sig_roundtrip(void **state) {
  (void)state;

  /* Build a SIG command packet */
  uint8_t cmd_buf[32];
  ssize_t cmd_len = ra_pack_pkt(cmd_buf, sizeof(cmd_buf), SIG_CMD, NULL, 0, false);
  assert_true(cmd_len > 0);

  /* Build a SIG response */
  uint8_t resp_buf[256];
  size_t resp_len = ra_mock_build_sig_response(resp_buf, sizeof(resp_buf),
      1000000, 4, 0x01, 1, 0, 0, "R7FA4M2AD3CFP");
  assert_true(resp_len > 0);

  /* Setup mock */
  ra_mock_t mock;
  ra_mock_init(&mock);
  ra_mock_add_response(&mock, resp_buf, resp_len);

  /* Simulate send */
  ra_mock_send(&mock, cmd_buf, cmd_len);
  assert_int_equal(ra_mock_verify_sent_cmd(&mock, 0, SIG_CMD), 0);

  /* Simulate receive */
  uint8_t recv_buf[256];
  ssize_t ret = ra_mock_recv(&mock, recv_buf, sizeof(recv_buf), 100);
  assert_true(ret > 0);

  /* Parse response */
  uint8_t data[64];
  size_t data_len;
  uint8_t cmd;
  ret = ra_unpack_pkt(recv_buf, ret, data, &data_len, &cmd);
  assert_true(ret > 0);
  assert_int_equal(cmd, SIG_CMD);
}

static void
test_protocol_era_roundtrip(void **state) {
  (void)state;

  /* Build an ERA command packet with address range */
  uint8_t era_data[8];
  uint32_to_be(0x00000000, &era_data[0]); /* Start */
  uint32_to_be(0x00001FFF, &era_data[4]); /* End (8KB) */

  uint8_t cmd_buf[32];
  ssize_t cmd_len = ra_pack_pkt(cmd_buf, sizeof(cmd_buf), ERA_CMD, era_data, 8, false);
  assert_true(cmd_len > 0);

  /* Build an OK response */
  uint8_t resp_buf[32];
  size_t resp_len = ra_mock_build_ok_response(resp_buf, sizeof(resp_buf), ERA_CMD);
  assert_true(resp_len > 0);

  /* Setup mock */
  ra_mock_t mock;
  ra_mock_init(&mock);
  ra_mock_add_response(&mock, resp_buf, resp_len);

  /* Simulate send */
  ra_mock_send(&mock, cmd_buf, cmd_len);
  assert_int_equal(ra_mock_verify_sent_cmd(&mock, 0, ERA_CMD), 0);

  /* Verify the address range in sent packet */
  const mock_packet_t *sent = ra_mock_get_sent(&mock, 0);
  assert_non_null(sent);
  /* Data starts at offset 4 in packet */
  uint32_t sent_start = be_to_uint32(&sent->data[4]);
  uint32_t sent_end = be_to_uint32(&sent->data[8]);
  assert_int_equal(sent_start, 0x00000000);
  assert_int_equal(sent_end, 0x00001FFF);
}

static void
test_protocol_dlm_roundtrip(void **state) {
  (void)state;

  /* Build a DLM command packet */
  uint8_t cmd_buf[32];
  ssize_t cmd_len = ra_pack_pkt(cmd_buf, sizeof(cmd_buf), DLM_CMD, NULL, 0, false);
  assert_true(cmd_len > 0);

  /* Build a DLM response */
  uint8_t resp_buf[32];
  size_t resp_len = ra_mock_build_dlm_response(resp_buf, sizeof(resp_buf), 0x04); /* DPL */
  assert_true(resp_len > 0);

  /* Setup mock */
  ra_mock_t mock;
  ra_mock_init(&mock);
  ra_mock_add_response(&mock, resp_buf, resp_len);

  /* Simulate send */
  ra_mock_send(&mock, cmd_buf, cmd_len);

  /* Simulate receive */
  uint8_t recv_buf[32];
  ssize_t ret = ra_mock_recv(&mock, recv_buf, sizeof(recv_buf), 100);
  assert_true(ret > 0);

  /* Parse response */
  uint8_t data[8];
  size_t data_len;
  uint8_t cmd;
  ret = ra_unpack_pkt(recv_buf, ret, data, &data_len, &cmd);
  assert_true(ret > 0);
  assert_int_equal(cmd, DLM_CMD);
  assert_int_equal(data_len, 1);
  assert_int_equal(data[0], 0x04); /* DPL state */
}

static void
test_protocol_ida_roundtrip(void **state) {
  (void)state;

  /* Build an IDA command packet with 16-byte ID code */
  uint8_t id_code[16] = {
    0x45, 0x66, 0x73, 0x89, 0x9A, 0xAB, 0xBC, 0xCD,
    0xDE, 0xEF, 0xF0, 0x01, 0x12, 0x23, 0x34, 0x45
  };

  uint8_t cmd_buf[64];
  ssize_t cmd_len = ra_pack_pkt(cmd_buf, sizeof(cmd_buf), IDA_CMD, id_code, 16, false);
  assert_true(cmd_len > 0);

  /* Build an OK response */
  uint8_t resp_buf[32];
  size_t resp_len = ra_mock_build_ok_response(resp_buf, sizeof(resp_buf), IDA_CMD);
  assert_true(resp_len > 0);

  /* Setup mock */
  ra_mock_t mock;
  ra_mock_init(&mock);
  ra_mock_add_response(&mock, resp_buf, resp_len);

  /* Simulate send */
  ra_mock_send(&mock, cmd_buf, cmd_len);
  assert_int_equal(ra_mock_verify_sent_cmd(&mock, 0, IDA_CMD), 0);

  /* Verify ID code in sent packet */
  const mock_packet_t *sent = ra_mock_get_sent(&mock, 0);
  assert_non_null(sent);
  assert_memory_equal(&sent->data[4], id_code, 16);
}

/*
 * Error handling tests
 */

static void
test_protocol_error_addr(void **state) {
  (void)state;

  ra_mock_t mock;
  ra_mock_init(&mock);

  /* Add an ERR_ADDR error response */
  ra_mock_add_error_response(&mock, 0xD0);

  /* Receive and verify error */
  uint8_t recv_buf[32];
  ssize_t ret = ra_mock_recv(&mock, recv_buf, sizeof(recv_buf), 100);
  assert_true(ret > 0);

  uint8_t data[16];
  size_t data_len;
  uint8_t cmd;
  ret = ra_unpack_pkt(recv_buf, ret, data, &data_len, &cmd);
  assert_int_equal(ret, -1);
  assert_int_equal(data[0], 0xD0);
  assert_string_equal(ra_strerror(data[0]), "ERR_ADDR");
}

static void
test_protocol_error_id(void **state) {
  (void)state;

  ra_mock_t mock;
  ra_mock_init(&mock);

  /* Add an ERR_ID error response (wrong ID code) */
  ra_mock_add_error_response(&mock, 0xDB);

  uint8_t recv_buf[32];
  ssize_t ret = ra_mock_recv(&mock, recv_buf, sizeof(recv_buf), 100);
  assert_true(ret > 0);

  uint8_t data[16];
  size_t data_len;
  uint8_t cmd;
  ret = ra_unpack_pkt(recv_buf, ret, data, &data_len, &cmd);
  assert_int_equal(ret, -1);
  assert_int_equal(data[0], 0xDB);
  assert_string_equal(ra_strerror(data[0]), "ERR_ID");
}

static void
test_protocol_error_prot(void **state) {
  (void)state;

  ra_mock_t mock;
  ra_mock_init(&mock);

  /* Add an ERR_PROT error response (protection error) */
  ra_mock_add_error_response(&mock, 0xDA);

  uint8_t recv_buf[32];
  ssize_t ret = ra_mock_recv(&mock, recv_buf, sizeof(recv_buf), 100);
  assert_true(ret > 0);

  uint8_t data[16];
  size_t data_len;
  uint8_t cmd;
  ret = ra_unpack_pkt(recv_buf, ret, data, &data_len, &cmd);
  assert_int_equal(ret, -1);
  assert_int_equal(data[0], 0xDA);
  assert_string_equal(ra_strerror(data[0]), "ERR_PROT");
}

int
main(void) {
  const struct CMUnitTest tests[] = {
    /* Mock response building */
    cmocka_unit_test(test_mock_build_sig_response),
    cmocka_unit_test(test_mock_build_area_response),
    cmocka_unit_test(test_mock_build_dlm_response),
    cmocka_unit_test(test_mock_build_boundary_response),

    /* Mock send/recv */
    cmocka_unit_test(test_mock_send_recv),
    cmocka_unit_test(test_mock_multiple_responses),
    cmocka_unit_test(test_mock_error_simulation),
    cmocka_unit_test(test_mock_add_error_response),

    /* Protocol round-trips */
    cmocka_unit_test(test_protocol_sig_roundtrip),
    cmocka_unit_test(test_protocol_era_roundtrip),
    cmocka_unit_test(test_protocol_dlm_roundtrip),
    cmocka_unit_test(test_protocol_ida_roundtrip),

    /* Error handling */
    cmocka_unit_test(test_protocol_error_addr),
    cmocka_unit_test(test_protocol_error_id),
    cmocka_unit_test(test_protocol_error_prot),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
