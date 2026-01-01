/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Serial port communication for Renesas RA bootloader
 */

#define _DEFAULT_SOURCE

#include "raconnect.h"
#include "rapacker.h"

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define GENERIC_CODE 0x55
#define BOOT_CODE_M4 0xC3  /* Cortex-M4/M23 (RA2/RA4 series) */
#define BOOT_CODE_M33 0xC6 /* Cortex-M33 (RA4M2/RA6 series) */

/* Forward declarations for static functions */
static int ra_inquire(ra_device_t *dev);
static int ra_confirm(ra_device_t *dev);

void
ra_dev_init(ra_device_t *dev) {
  memset(dev, 0, sizeof(*dev));
  dev->fd = -1;
  dev->vendor_id = RENESAS_VID;
  dev->product_id = RENESAS_PID;
  dev->max_tries = MAX_TRIES;
  dev->timeout_ms = TIMEOUT_MS;
  dev->sel_area = 0;
}

/* Forward declarations */
static int ra_sync(ra_device_t *dev);
static int ra_inquire(ra_device_t *dev);
static int ra_confirm(ra_device_t *dev);

static int
set_serial_attrs(int fd, speed_t speed) {
  struct termios tty;

  if (tcgetattr(fd, &tty) < 0)
    return -1;

  cfsetospeed(&tty, speed);
  cfsetispeed(&tty, speed);

  tty.c_cflag &= ~CSIZE;
  tty.c_cflag |= CS8;
  tty.c_cflag &= ~PARENB;
  tty.c_cflag &= ~CSTOPB;
  tty.c_cflag &= ~CRTSCTS;
  tty.c_cflag |= CREAD | CLOCAL;

  tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ECHONL | ISIG);
  tty.c_iflag &= ~(IXON | IXOFF | IXANY);
  tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
  tty.c_oflag &= ~(OPOST | ONLCR);

  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 1; /* 100ms timeout */

  if (tcsetattr(fd, TCSANOW, &tty) < 0)
    return -1;

  return 0;
}

int
ra_open(ra_device_t *dev, const char *port) {
  char portbuf[256];
  char tty_name[64];
  bool auto_detect = false;

  if (port == NULL) {
    if (ra_find_port(portbuf, sizeof(portbuf), tty_name, sizeof(tty_name)) < 0) {
      warnx("no Renesas device found");
      return -1;
    }
    port = portbuf;
    auto_detect = true;
  } else {
    /* Extract tty name from user-provided port */
    const char *slash = strrchr(port, '/');
    if (slash != NULL)
      strncpy(tty_name, slash + 1, sizeof(tty_name) - 1);
    else
      strncpy(tty_name, port, sizeof(tty_name) - 1);
    tty_name[sizeof(tty_name) - 1] = '\0';
  }

  /* Print USB device information */
  ra_print_usb_info(tty_name);
  if (auto_detect)
    fprintf(stderr, "Auto-detected Renesas device\n");

  dev->fd = open(port, O_RDWR | O_NOCTTY | O_SYNC);
  if (dev->fd < 0) {
    warn("failed to open %s", port);
    return -1;
  }

  if (set_serial_attrs(dev->fd, B9600) < 0) {
    warn("failed to set serial attributes");
    close(dev->fd);
    dev->fd = -1;
    return -1;
  }

  /* Flush any stale data in buffers */
  tcflush(dev->fd, TCIOFLUSH);

  /* Check if bootloader is already in command mode (from previous connection) */
  int already_connected = ra_inquire(dev);
  if (already_connected < 0) {
    close(dev->fd);
    dev->fd = -1;
    return -1;
  }

  if (already_connected) {
    fprintf(stderr, "Bootloader already in command mode\n");
  } else {
    /* Establish connection:
     * 1. Sync with 0x00 bytes until device responds with 0x00
     * 2. Confirm with 0x55, expect boot code (0xC3 or 0xC6)
     */
    if (ra_sync(dev) < 0) {
      close(dev->fd);
      dev->fd = -1;
      return -1;
    }
    if (ra_confirm(dev) < 0) {
      close(dev->fd);
      dev->fd = -1;
      return -1;
    }
  }

  return 0;
}

void
ra_close(ra_device_t *dev) {
  if (dev->fd >= 0) {
    close(dev->fd);
    dev->fd = -1;
  }
}

ssize_t
ra_send(ra_device_t *dev, const uint8_t *data, size_t len) {
  if (dev->fd < 0) {
    errno = EBADF;
    return -1;
  }

  ssize_t n = write(dev->fd, data, len);
  if (n < 0)
    warn("write failed");

  return n;
}

ssize_t
ra_recv(ra_device_t *dev, uint8_t *buf, size_t len, int timeout_ms) {
  if (dev->fd < 0) {
    errno = EBADF;
    return -1;
  }

  struct pollfd pfd = {
    .fd = dev->fd,
    .events = POLLIN,
  };

  size_t total = 0;
  while (total < len) {
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret < 0) {
      warn("poll failed");
      return -1;
    }
    if (ret == 0) {
      /* Timeout - return what we have */
      break;
    }

    ssize_t n = read(dev->fd, buf + total, len - total);
    if (n < 0) {
      warn("read failed");
      return -1;
    }
    if (n == 0)
      break;

    total += n;
  }

  return (ssize_t)total;
}

static int
ra_sync(ra_device_t *dev) {
  uint8_t sync = 0x00;
  uint8_t resp;

  /* Send 0x00 bytes until device responds with 0x00 */
  for (int i = 0; i < dev->max_tries; i++) {
    if (write(dev->fd, &sync, 1) != 1)
      continue;

    ssize_t n = ra_recv(dev, &resp, 1, dev->timeout_ms);
    if (n == 1 && resp == 0x00) {
      fprintf(stderr, "Sync OK\n");
      return 0;
    }
  }

  warnx("failed to sync with bootloader");
  return -1;
}

static int
ra_inquire(ra_device_t *dev) {
  uint8_t pkt[MAX_PKT_LEN];
  ssize_t pkt_len;

  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), INQ_CMD, NULL, 0, false);
  if (pkt_len < 0)
    return -1;

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  uint8_t resp[1];
  ssize_t n = ra_recv(dev, resp, 1, dev->timeout_ms);
  if (n < 0)
    return -1;

  fprintf(stderr, "inquire: n=%zd, resp=0x%02x\n", n, n > 0 ? resp[0] : 0);
  if (n == 0 || resp[0] == 0x00) {
    /* Not connected yet */
    return 0;
  }

  /* Read rest of response */
  uint8_t rest[6];
  n = ra_recv(dev, rest, 6, dev->timeout_ms);
  if (n < 6)
    return -1;

  /* Already connected */
  return 1;
}

static int
ra_confirm(ra_device_t *dev) {
  uint8_t cmd = GENERIC_CODE;
  uint8_t resp;

  for (int i = 0; i < dev->max_tries; i++) {
    if (write(dev->fd, &cmd, 1) != 1)
      continue;

    ssize_t n = ra_recv(dev, &resp, 1, dev->timeout_ms);
    if (n == 1) {
      if (resp == BOOT_CODE_M4) {
        fprintf(stderr, "Boot code 0xC3 (Cortex-M4/M23)\n");
        usleep(50000); /* Wait 50ms for bootloader to transition */
        return 0;
      }
      if (resp == BOOT_CODE_M33) {
        fprintf(stderr, "Boot code 0xC6 (Cortex-M33)\n");
        usleep(100000); /* Wait 100ms for bootloader to transition */
        return 0;
      }
      if (resp == 0xC5) {
        fprintf(stderr, "Boot code 0xC5 (Cortex-M85)\n");
        usleep(50000); /* Wait 50ms for bootloader to transition */
        return 0;
      }
      /* Unexpected response */
      fprintf(stderr, "unexpected response: 0x%02X\n", resp);
    } else if (n == 0) {
      fprintf(stderr, "no response (try %d/%d)\n", i + 1, dev->max_tries);
    } else {
      warn("read error: retry #%d", i);
    }
  }

  warnx("failed to establish connection after %d tries", dev->max_tries);
  return -1;
}

static speed_t
baudrate_to_speed(uint32_t baudrate) {
  switch (baudrate) {
  case 9600:
    return B9600;
  case 19200:
    return B19200;
  case 38400:
    return B38400;
  case 57600:
    return B57600;
  case 115200:
    return B115200;
#ifdef B230400
  case 230400:
    return B230400;
#endif
#ifdef B460800
  case 460800:
    return B460800;
#endif
#ifdef B500000
  case 500000:
    return B500000;
#endif
#ifdef B576000
  case 576000:
    return B576000;
#endif
#ifdef B921600
  case 921600:
    return B921600;
#endif
#ifdef B1000000
  case 1000000:
    return B1000000;
#endif
#ifdef B1152000
  case 1152000:
    return B1152000;
#endif
#ifdef B1500000
  case 1500000:
    return B1500000;
#endif
#ifdef B2000000
  case 2000000:
    return B2000000;
#endif
#ifdef B2500000
  case 2500000:
    return B2500000;
#endif
#ifdef B3000000
  case 3000000:
    return B3000000;
#endif
#ifdef B3500000
  case 3500000:
    return B3500000;
#endif
#ifdef B4000000
  case 4000000:
    return B4000000;
#endif
  default:
    return B0;
  }
}

int
ra_set_baudrate(ra_device_t *dev, uint32_t baudrate) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[16];
  uint8_t data[4];
  ssize_t pkt_len, n;

  /* Check if baudrate is supported by termios */
  speed_t speed = baudrate_to_speed(baudrate);
  if (speed == B0) {
    warnx("unsupported baud rate: %u", baudrate);
    return -1;
  }

  /* Pack baudrate as big-endian */
  data[0] = (baudrate >> 24) & 0xFF;
  data[1] = (baudrate >> 16) & 0xFF;
  data[2] = (baudrate >> 8) & 0xFF;
  data[3] = baudrate & 0xFF;

  pkt_len = ra_pack_pkt(pkt, sizeof(pkt), BAU_CMD, data, 4, false);
  if (pkt_len < 0)
    return -1;

  if (ra_send(dev, pkt, pkt_len) < 0)
    return -1;

  n = ra_recv(dev, resp, 7, 500);
  if (n < 7) {
    warnx("short response for baud rate command");
    return -1;
  }

  size_t data_len;
  uint8_t cmd;
  if (ra_unpack_pkt(resp, n, NULL, &data_len, &cmd) < 0) {
    warnx("baud rate setting failed");
    return -1;
  }

  /* Wait 1ms as per spec before changing local baudrate */
  usleep(1000);

  /* Change local serial port baudrate */
  if (set_serial_attrs(dev->fd, speed) < 0) {
    warn("failed to set local baud rate to %u", baudrate);
    return -1;
  }

  fprintf(stderr, "Baud rate changed to %u bps\n", baudrate);
  return 0;
}
