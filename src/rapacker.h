/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Protocol packet encoding/decoding for Renesas RA bootloader
 */

#ifndef RAPACKER_H
#define RAPACKER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

/* Bootloader commands */
#define INQ_CMD 0x00 /* Inquire connection */
#define ERA_CMD 0x12 /* Erase flash */
#define WRI_CMD 0x13 /* Write to flash */
#define REA_CMD 0x15 /* Read from flash */
#define IDA_CMD 0x30 /* ID authentication */
#define BAU_CMD 0x34 /* Baud rate setting */
#define SIG_CMD 0x3A /* Get device signature */
#define ARE_CMD 0x3B /* Get area information */

/* Status codes */
#define STATUS_OK 0x00
#define STATUS_ERR 0x80

/* Protocol constants */
#define SOD_CMD 0x01 /* Start of data (command) */
#define SOD_ACK 0x81 /* Start of data (ack/response) */
#define ETX 0x03     /* End of transmission */

#define MAX_DATA_LEN 1024
#define MAX_PKT_LEN (MAX_DATA_LEN + 6) /* SOD + LNH + LNL + CMD + data + SUM + ETX */

/*
 * Calculate two's complement checksum
 */
uint8_t ra_calc_sum(uint8_t cmd, const uint8_t *data, size_t len);

/*
 * Pack a protocol packet
 *
 * Returns: packet length on success, -1 on error
 */
ssize_t
ra_pack_pkt(uint8_t *buf, size_t buflen, uint8_t cmd, const uint8_t *data, size_t len, bool ack);

/*
 * Unpack a protocol packet
 *
 * Returns: data length on success, -1 on error (sets errno)
 * If err_code is not NULL and MCU returns an error, the error code is stored there
 */
ssize_t
ra_unpack_pkt(const uint8_t *buf, size_t buflen, uint8_t *data, size_t *data_len, uint8_t *cmd);

/*
 * Get error name for MCU error code (e.g., "ERR_ADDR")
 */
const char *ra_strerror(uint8_t code);

/*
 * Get error description for MCU error code (e.g., "invalid address")
 */
const char *ra_strdesc(uint8_t code);

#endif /* RAPACKER_H */
