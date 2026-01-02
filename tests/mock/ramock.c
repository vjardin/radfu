/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Mock device layer for testing protocol handling without hardware
 */

#include "ramock.h"
#include <string.h>
#include <stdio.h>

void
ra_mock_init(ra_mock_t *mock) {
  memset(mock, 0, sizeof(*mock));
}

int
ra_mock_add_response(ra_mock_t *mock, const uint8_t *data, size_t len) {
  if (mock->response_count >= MOCK_MAX_RESPONSES)
    return -1;
  if (len > MOCK_MAX_PKT_SIZE)
    return -1;

  memcpy(mock->responses[mock->response_count].data, data, len);
  mock->responses[mock->response_count].len = len;
  mock->response_count++;
  return 0;
}

int
ra_mock_add_response_pkt(ra_mock_t *mock, uint8_t cmd, const uint8_t *data, size_t len) {
  if (mock->response_count >= MOCK_MAX_RESPONSES)
    return -1;

  uint8_t buf[MOCK_MAX_PKT_SIZE];
  ssize_t pkt_len = ra_pack_pkt(buf, sizeof(buf), cmd, data, len, true);
  if (pkt_len < 0)
    return -1;

  return ra_mock_add_response(mock, buf, (size_t)pkt_len);
}

int
ra_mock_add_error_response(ra_mock_t *mock, uint8_t err_code) {
  /* Error response: cmd has high bit set, data contains error code */
  uint8_t data[] = { err_code };
  return ra_mock_add_response_pkt(mock, STATUS_ERR | 0x00, data, 1);
}

const mock_packet_t *
ra_mock_get_sent(ra_mock_t *mock, size_t index) {
  if (index >= mock->sent_count)
    return NULL;
  return &mock->sent[index];
}

int
ra_mock_verify_sent(ra_mock_t *mock, size_t index, const uint8_t *expected, size_t len) {
  const mock_packet_t *pkt = ra_mock_get_sent(mock, index);
  if (pkt == NULL)
    return -1;
  if (pkt->len != len)
    return -1;
  if (memcmp(pkt->data, expected, len) != 0)
    return -1;
  return 0;
}

int
ra_mock_verify_sent_cmd(ra_mock_t *mock, size_t index, uint8_t expected_cmd) {
  const mock_packet_t *pkt = ra_mock_get_sent(mock, index);
  if (pkt == NULL)
    return -1;
  if (pkt->len < 4)
    return -1;
  /* Command is at offset 3 in packet */
  if (pkt->data[3] != expected_cmd)
    return -1;
  return 0;
}

ssize_t
ra_mock_send(ra_mock_t *mock, const uint8_t *data, size_t len) {
  if (mock->fail_send)
    return -1;

  if (mock->sent_count >= MOCK_MAX_SENT)
    return -1;
  if (len > MOCK_MAX_PKT_SIZE)
    return -1;

  memcpy(mock->sent[mock->sent_count].data, data, len);
  mock->sent[mock->sent_count].len = len;
  mock->sent_count++;

  return (ssize_t)len;
}

ssize_t
ra_mock_recv(ra_mock_t *mock, uint8_t *buf, size_t len, int timeout_ms) {
  (void)timeout_ms;

  if (mock->fail_recv)
    return -1;
  if (mock->timeout_recv)
    return 0;

  if (mock->current_response >= mock->response_count)
    return 0; /* No more responses */

  mock_packet_t *resp = &mock->responses[mock->current_response];
  size_t copy_len = resp->len < len ? resp->len : len;

  memcpy(buf, resp->data, copy_len);
  mock->current_response++;

  return (ssize_t)copy_len;
}

void
ra_mock_setup_device(ra_device_t *dev, ra_mock_t *mock) {
  (void)mock;
  ra_dev_init(dev);
  /* The fd is not used when mocking, but set to -1 to be safe */
  dev->fd = -1;
}

/*
 * Response builders
 */

size_t
ra_mock_build_sig_response(uint8_t *buf,
    size_t buflen,
    uint32_t max_baud,
    uint8_t num_areas,
    uint8_t typ,
    uint8_t bfv_major,
    uint8_t bfv_minor,
    uint8_t bfv_build,
    const char *product_name) {

  /* Build signature data: RMB(4) + NOA(1) + TYP(1) + BFV(3) + DID(16) + PTN(16) = 41 bytes */
  uint8_t data[41];
  memset(data, 0, sizeof(data));

  /* RMB: Recommended max baud (big-endian) */
  data[0] = (max_baud >> 24) & 0xFF;
  data[1] = (max_baud >> 16) & 0xFF;
  data[2] = (max_baud >> 8) & 0xFF;
  data[3] = max_baud & 0xFF;

  /* NOA: Number of areas */
  data[4] = num_areas;

  /* TYP: Device type */
  data[5] = typ;

  /* BFV: Boot firmware version */
  data[6] = bfv_major;
  data[7] = bfv_minor;
  data[8] = bfv_build;

  /* DID: Device ID (16 bytes) - fill with dummy data */
  data[9] = 'T'; /* Wafer fab */
  data[10] = 'T';
  data[11] = 0x51; /* Year/month: 2015-01 */
  data[12] = 0x01; /* Day */
  data[13] = 0x00; /* CRC16 high */
  data[14] = 0x00; /* CRC16 low */
  data[15] = 'A';  /* Lot number */
  data[16] = 'B';
  data[17] = 'C';
  data[18] = 'D';
  data[19] = 'E';
  data[20] = 'F';
  data[21] = 0x01; /* Wafer number */
  data[22] = 0x10; /* X address */
  data[23] = 0x20; /* Y address */
  data[24] = 0x00; /* Reserved */

  /* PTN: Product type name (16 bytes, space-padded) */
  size_t name_len = product_name ? strlen(product_name) : 0;
  if (name_len > 16)
    name_len = 16;
  if (product_name)
    memcpy(&data[25], product_name, name_len);
  for (size_t i = name_len; i < 16; i++)
    data[25 + i] = ' ';

  /* Build packet */
  return (size_t)ra_pack_pkt(buf, buflen, SIG_CMD, data, sizeof(data), true);
}

size_t
ra_mock_build_area_response(uint8_t *buf,
    size_t buflen,
    uint8_t koa,
    uint32_t sad,
    uint32_t ead,
    uint32_t eau,
    uint32_t wau,
    uint32_t rau,
    uint32_t cau) {

  /* Area data: KOA(1) + SAD(4) + EAD(4) + EAU(4) + WAU(4) + RAU(4) + CAU(4) = 25 bytes */
  uint8_t data[25];

  data[0] = koa;

  /* SAD */
  data[1] = (sad >> 24) & 0xFF;
  data[2] = (sad >> 16) & 0xFF;
  data[3] = (sad >> 8) & 0xFF;
  data[4] = sad & 0xFF;

  /* EAD */
  data[5] = (ead >> 24) & 0xFF;
  data[6] = (ead >> 16) & 0xFF;
  data[7] = (ead >> 8) & 0xFF;
  data[8] = ead & 0xFF;

  /* EAU */
  data[9] = (eau >> 24) & 0xFF;
  data[10] = (eau >> 16) & 0xFF;
  data[11] = (eau >> 8) & 0xFF;
  data[12] = eau & 0xFF;

  /* WAU */
  data[13] = (wau >> 24) & 0xFF;
  data[14] = (wau >> 16) & 0xFF;
  data[15] = (wau >> 8) & 0xFF;
  data[16] = wau & 0xFF;

  /* RAU */
  data[17] = (rau >> 24) & 0xFF;
  data[18] = (rau >> 16) & 0xFF;
  data[19] = (rau >> 8) & 0xFF;
  data[20] = rau & 0xFF;

  /* CAU */
  data[21] = (cau >> 24) & 0xFF;
  data[22] = (cau >> 16) & 0xFF;
  data[23] = (cau >> 8) & 0xFF;
  data[24] = cau & 0xFF;

  /* Build packet */
  return (size_t)ra_pack_pkt(buf, buflen, ARE_CMD, data, sizeof(data), true);
}

size_t
ra_mock_build_dlm_response(uint8_t *buf, size_t buflen, uint8_t dlm_state) {
  uint8_t data[] = { dlm_state };
  return (size_t)ra_pack_pkt(buf, buflen, DLM_CMD, data, sizeof(data), true);
}

size_t
ra_mock_build_ok_response(uint8_t *buf, size_t buflen, uint8_t cmd) {
  uint8_t data[] = { 0x00 };
  return (size_t)ra_pack_pkt(buf, buflen, cmd, data, sizeof(data), true);
}

size_t
ra_mock_build_boundary_response(uint8_t *buf,
    size_t buflen,
    uint16_t cfs1,
    uint16_t cfs2,
    uint16_t dfs,
    uint16_t srs1,
    uint16_t srs2) {

  /* Boundary data: CFS1(2) + CFS2(2) + DFS(2) + SRS1(2) + SRS2(2) = 10 bytes */
  uint8_t data[10];

  data[0] = (cfs1 >> 8) & 0xFF;
  data[1] = cfs1 & 0xFF;
  data[2] = (cfs2 >> 8) & 0xFF;
  data[3] = cfs2 & 0xFF;
  data[4] = (dfs >> 8) & 0xFF;
  data[5] = dfs & 0xFF;
  data[6] = (srs1 >> 8) & 0xFF;
  data[7] = srs1 & 0xFF;
  data[8] = (srs2 >> 8) & 0xFF;
  data[9] = srs2 & 0xFF;

  return (size_t)ra_pack_pkt(buf, buflen, BND_CMD, data, sizeof(data), true);
}
