/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Serial port communication for Renesas RA bootloader
 */

#ifndef RACONNECT_H
#define RACONNECT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

#define RENESAS_VID 0x045B
#define RENESAS_PID 0x0261

#define MAX_AREAS 4
#define MAX_TRIES 20
#define TIMEOUT_MS 100

#define MAX_TRANSFER_SIZE (2048 + 6)

typedef struct {
  uint8_t koa;  /* Kind of area (spec 6.16.2.2) */
  uint32_t sad; /* Start address */
  uint32_t ead; /* End address */
  uint32_t eau; /* Erase alignment unit */
  uint32_t wau; /* Write alignment unit */
  uint32_t rau; /* Read alignment unit */
  uint32_t cau; /* CRC alignment unit */
} ra_area_t;

typedef struct {
  int fd;
  uint16_t vendor_id;
  uint16_t product_id;
  int max_tries;
  int timeout_ms;
  ra_area_t chip_layout[MAX_AREAS];
  int sel_area;
  bool authenticated; /* True if ID authentication was performed */
  bool uart_mode;     /* True for plain UART (P109/P110), false for USB */
} ra_device_t;

/*
 * Initialize device structure with defaults
 */
void ra_dev_init(ra_device_t *dev);

/*
 * Open serial port connection
 * Returns: 0 on success, -1 on error
 */
int ra_open(ra_device_t *dev, const char *port);

/*
 * Close device connection
 */
void ra_close(ra_device_t *dev);

/*
 * Send data to device
 * Returns: bytes sent on success, -1 on error
 */
ssize_t ra_send(ra_device_t *dev, const uint8_t *data, size_t len);

/*
 * Receive data from device
 * Returns: bytes received on success, -1 on error
 */
ssize_t ra_recv(ra_device_t *dev, uint8_t *buf, size_t len, int timeout_ms);

/*
 * Set UART baud rate
 * Only affects UART communication, not USB
 * Returns: 0 on success, -1 on error
 */
int ra_set_baudrate(ra_device_t *dev, uint32_t baudrate);

/*
 * Find highest supported baud rate <= max
 * Returns: best supported rate, or 9600 if none higher
 */
uint32_t ra_best_baudrate(uint32_t max);

/*
 * Platform-specific port detection (implemented in port_linux.c / port_macos.c)
 */
int ra_find_port(char *buf, size_t len, char *tty_name, size_t tty_len);
void ra_print_usb_info(const char *tty_name);

#endif /* RACONNECT_H */
