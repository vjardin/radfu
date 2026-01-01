/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Mock device layer for testing protocol handling without hardware
 */

#ifndef RAMOCK_H
#define RAMOCK_H

#include "../../src/raconnect.h"
#include "../../src/rapacker.h"
#include <stddef.h>
#include <stdint.h>

#define MOCK_MAX_RESPONSES 32
#define MOCK_MAX_SENT 32
#define MOCK_MAX_PKT_SIZE 2048

/*
 * Pre-programmed response for mock device
 */
typedef struct {
  uint8_t data[MOCK_MAX_PKT_SIZE];
  size_t len;
} mock_packet_t;

/*
 * Mock device state
 */
typedef struct {
  /* Pre-programmed responses */
  mock_packet_t responses[MOCK_MAX_RESPONSES];
  size_t response_count;
  size_t current_response;

  /* Captured sent packets */
  mock_packet_t sent[MOCK_MAX_SENT];
  size_t sent_count;

  /* Error simulation */
  int fail_send;      /* If set, ra_mock_send returns -1 */
  int fail_recv;      /* If set, ra_mock_recv returns -1 */
  int timeout_recv;   /* If set, ra_mock_recv returns 0 (timeout) */
} ra_mock_t;

/*
 * Initialize mock device
 */
void ra_mock_init(ra_mock_t *mock);

/*
 * Add a pre-programmed response packet (raw bytes)
 * Returns: 0 on success, -1 if full
 */
int ra_mock_add_response(ra_mock_t *mock, const uint8_t *data, size_t len);

/*
 * Add a pre-programmed response packet (builds packet from cmd + data)
 * Returns: 0 on success, -1 if full
 */
int ra_mock_add_response_pkt(ra_mock_t *mock, uint8_t cmd, const uint8_t *data, size_t len);

/*
 * Add an error response packet
 * Returns: 0 on success, -1 if full
 */
int ra_mock_add_error_response(ra_mock_t *mock, uint8_t err_code);

/*
 * Get a captured sent packet
 * Returns: pointer to packet data, NULL if index out of range
 */
const mock_packet_t *ra_mock_get_sent(ra_mock_t *mock, size_t index);

/*
 * Verify a sent packet matches expected
 * Returns: 0 if match, -1 if mismatch
 */
int ra_mock_verify_sent(ra_mock_t *mock, size_t index, const uint8_t *expected, size_t len);

/*
 * Verify command byte in sent packet
 * Returns: 0 if match, -1 if mismatch
 */
int ra_mock_verify_sent_cmd(ra_mock_t *mock, size_t index, uint8_t expected_cmd);

/*
 * Mock implementations of ra_send/ra_recv
 * These are called instead of the real implementations when testing
 */
ssize_t ra_mock_send(ra_mock_t *mock, const uint8_t *data, size_t len);
ssize_t ra_mock_recv(ra_mock_t *mock, uint8_t *buf, size_t len, int timeout_ms);

/*
 * Setup a device with mock functions
 * After calling this, you can use the mock directly
 */
void ra_mock_setup_device(ra_device_t *dev, ra_mock_t *mock);

/*
 * Pre-built response generators for common protocol responses
 */

/*
 * Build a signature (SIG) response for RA4M2
 * Populates provided buffer with valid signature packet
 * Returns: packet length
 */
size_t ra_mock_build_sig_response(uint8_t *buf, size_t buflen,
    uint32_t max_baud, uint8_t num_areas, uint8_t typ,
    uint8_t bfv_major, uint8_t bfv_minor, uint8_t bfv_build,
    const char *product_name);

/*
 * Build an area info (ARE) response
 * Returns: packet length
 */
size_t ra_mock_build_area_response(uint8_t *buf, size_t buflen,
    uint8_t koa, uint32_t sad, uint32_t ead,
    uint32_t eau, uint32_t wau, uint32_t rau, uint32_t cau);

/*
 * Build a DLM state response
 * Returns: packet length
 */
size_t ra_mock_build_dlm_response(uint8_t *buf, size_t buflen, uint8_t dlm_state);

/*
 * Build a simple OK response (no data)
 * Returns: packet length
 */
size_t ra_mock_build_ok_response(uint8_t *buf, size_t buflen, uint8_t cmd);

/*
 * Build a boundary response
 * Returns: packet length
 */
size_t ra_mock_build_boundary_response(uint8_t *buf, size_t buflen,
    uint16_t cfs1, uint16_t cfs2, uint16_t dfs, uint16_t srs1, uint16_t srs2);

#endif /* RAMOCK_H */
