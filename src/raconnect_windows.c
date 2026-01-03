/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Serial port communication for Renesas RA bootloader (Windows)
 */

#include "raconnect.h"
#include "rapacker.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SYNC_BYTE 0x00 /* Synchronization byte for connection */
#define GENERIC_CODE 0x55
#define BOOT_CODE_M4 0xC3  /* Cortex-M4/M23 (RA2/RA4 series) */
#define BOOT_CODE_M33 0xC6 /* Cortex-M33 (RA4M2/RA6 series) */
#define BOOT_CODE_M85 0xC5 /* Cortex-M85 (RA8 series) */

/* Forward declarations */
static int ra_sync(ra_device_t *dev);
static int ra_inquire(ra_device_t *dev);
static int ra_confirm(ra_device_t *dev);

void
ra_dev_init(ra_device_t *dev) {
  memset(dev, 0, sizeof(*dev));
  dev->fd = RA_INVALID_FD;
  dev->vendor_id = RENESAS_VID;
  dev->product_id = RENESAS_PID;
  dev->max_tries = MAX_TRIES;
  dev->timeout_ms = TIMEOUT_MS;
  dev->sel_area = 0;
  dev->baudrate = 9600; /* Initial baud rate for UART mode */
}

static int
set_serial_attrs(HANDLE hPort, DWORD baudrate) {
  DCB dcb;

  memset(&dcb, 0, sizeof(dcb));
  dcb.DCBlength = sizeof(dcb);

  if (!GetCommState(hPort, &dcb)) {
    fprintf(stderr, "GetCommState failed: %lu\n", GetLastError());
    return -1;
  }

  dcb.BaudRate = baudrate;
  dcb.ByteSize = 8;
  dcb.Parity = NOPARITY;
  dcb.StopBits = ONESTOPBIT;
  dcb.fBinary = TRUE;
  dcb.fParity = FALSE;
  dcb.fOutxCtsFlow = FALSE;
  dcb.fOutxDsrFlow = FALSE;
  dcb.fDtrControl = DTR_CONTROL_ENABLE;
  dcb.fDsrSensitivity = FALSE;
  dcb.fOutX = FALSE;
  dcb.fInX = FALSE;
  dcb.fRtsControl = RTS_CONTROL_ENABLE;
  dcb.fAbortOnError = FALSE;

  if (!SetCommState(hPort, &dcb)) {
    fprintf(stderr, "SetCommState failed: %lu\n", GetLastError());
    return -1;
  }

  /* Set timeouts */
  COMMTIMEOUTS timeouts;
  timeouts.ReadIntervalTimeout = MAXDWORD;
  timeouts.ReadTotalTimeoutMultiplier = 0;
  timeouts.ReadTotalTimeoutConstant = 100; /* 100ms read timeout */
  timeouts.WriteTotalTimeoutMultiplier = 0;
  timeouts.WriteTotalTimeoutConstant = 1000; /* 1s write timeout */

  if (!SetCommTimeouts(hPort, &timeouts)) {
    fprintf(stderr, "SetCommTimeouts failed: %lu\n", GetLastError());
    return -1;
  }

  return 0;
}

int
ra_open(ra_device_t *dev, const char *port) {
  char portbuf[256];
  char tty_name[64];
  bool auto_detect = false;
  char port_path[32];

  if (port == NULL) {
    if (dev->uart_mode) {
      fprintf(stderr, "UART mode requires explicit port (-p option)\n");
      return -1;
    }
    if (ra_find_port(portbuf, sizeof(portbuf), tty_name, sizeof(tty_name)) < 0) {
      fprintf(stderr, "no Renesas device found\n");
      return -1;
    }
    port = portbuf;
    auto_detect = true;
  } else {
    strncpy(tty_name, port, sizeof(tty_name) - 1);
    tty_name[sizeof(tty_name) - 1] = '\0';
  }

  /* Print device information */
  if (dev->uart_mode) {
    fprintf(stderr, "UART mode: %s\n", port);
  } else {
    ra_print_usb_info(tty_name);
    if (auto_detect)
      fprintf(stderr, "Auto-detected Renesas device\n");
  }

  /* Windows requires \\.\COMx format for COM ports >= 10 */
  if (_strnicmp(port, "COM", 3) == 0) {
    snprintf(port_path, sizeof(port_path), "\\\\.\\%s", port);
  } else if (_strnicmp(port, "\\\\.\\", 4) == 0) {
    strncpy(port_path, port, sizeof(port_path) - 1);
    port_path[sizeof(port_path) - 1] = '\0';
  } else {
    snprintf(port_path, sizeof(port_path), "\\\\.\\%s", port);
  }

  dev->fd = CreateFileA(
      port_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (dev->fd == RA_INVALID_FD) {
    fprintf(stderr, "failed to open %s: %lu\n", port, GetLastError());
    return -1;
  }

  if (set_serial_attrs(dev->fd, 9600) < 0) {
    fprintf(stderr, "failed to set serial attributes\n");
    CloseHandle(dev->fd);
    dev->fd = RA_INVALID_FD;
    return -1;
  }

  /* Flush any stale data in buffers */
  PurgeComm(dev->fd, PURGE_RXCLEAR | PURGE_TXCLEAR);

  /* Check if bootloader is already in command mode (from previous connection) */
  int already_connected = ra_inquire(dev);
  if (already_connected < 0) {
    CloseHandle(dev->fd);
    dev->fd = RA_INVALID_FD;
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
      CloseHandle(dev->fd);
      dev->fd = RA_INVALID_FD;
      return -1;
    }
    if (ra_confirm(dev) < 0) {
      CloseHandle(dev->fd);
      dev->fd = RA_INVALID_FD;
      return -1;
    }
  }

  return 0;
}

void
ra_close(ra_device_t *dev) {
  if (dev->fd != RA_INVALID_FD) {
    /* In UART mode, silently reset to 9600 so next connection can sync */
    if (dev->uart_mode && dev->baudrate > 9600) {
      uint8_t pkt[MAX_PKT_LEN];
      uint8_t data[4] = { 0, 0, 0x25, 0x80 }; /* 9600 bps big-endian */
      ssize_t len = ra_pack_pkt(pkt, sizeof(pkt), BAU_CMD, data, 4, false);
      ra_send(dev, pkt, len);
    }
    CloseHandle(dev->fd);
    dev->fd = RA_INVALID_FD;
  }
}

ssize_t
ra_send(ra_device_t *dev, const uint8_t *data, size_t len) {
  if (dev->fd == RA_INVALID_FD) {
    SetLastError(ERROR_INVALID_HANDLE);
    return -1;
  }

  DWORD bytes_written;
  if (!WriteFile(dev->fd, data, (DWORD)len, &bytes_written, NULL)) {
    fprintf(stderr, "write failed: %lu\n", GetLastError());
    return -1;
  }

  return (ssize_t)bytes_written;
}

ssize_t
ra_recv(ra_device_t *dev, uint8_t *buf, size_t len, int timeout_ms) {
  if (dev->fd == RA_INVALID_FD) {
    SetLastError(ERROR_INVALID_HANDLE);
    return -1;
  }

  /* Set read timeout */
  COMMTIMEOUTS timeouts;
  timeouts.ReadIntervalTimeout = MAXDWORD;
  timeouts.ReadTotalTimeoutMultiplier = 0;
  timeouts.ReadTotalTimeoutConstant = (DWORD)timeout_ms;
  timeouts.WriteTotalTimeoutMultiplier = 0;
  timeouts.WriteTotalTimeoutConstant = 1000;

  if (!SetCommTimeouts(dev->fd, &timeouts)) {
    fprintf(stderr, "SetCommTimeouts failed: %lu\n", GetLastError());
    return -1;
  }

  size_t total = 0;
  while (total < len) {
    DWORD bytes_read;

    /* Use shorter timeout for continuation reads after initial data */
    if (total > 0) {
      timeouts.ReadTotalTimeoutConstant = 20;
      SetCommTimeouts(dev->fd, &timeouts);
    }

    if (!ReadFile(dev->fd, buf + total, (DWORD)(len - total), &bytes_read, NULL)) {
      fprintf(stderr, "read failed: %lu\n", GetLastError());
      return -1;
    }

    if (bytes_read == 0)
      break;

    total += bytes_read;
  }

  return (ssize_t)total;
}

static int
ra_sync(ra_device_t *dev) {
  const uint8_t sync[] = { SYNC_BYTE, SYNC_BYTE, SYNC_BYTE };
  uint8_t resp;
  DWORD bytes_written;

  /* Send 3 consecutive SYNC_BYTEs until device responds with SYNC_BYTE */
  for (int i = 0; i < dev->max_tries; i++) {
    if (!WriteFile(dev->fd, sync, sizeof(sync), &bytes_written, NULL) ||
        bytes_written != sizeof(sync))
      continue;

    ssize_t n = ra_recv(dev, &resp, 1, dev->timeout_ms);
    if (n == 1 && resp == SYNC_BYTE) {
      fprintf(stderr, "Sync OK\n");
      return 0;
    }
  }

  fprintf(stderr, "failed to sync with bootloader\n");
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

  uint8_t resp[4];
  ssize_t n = ra_recv(dev, resp, 1, dev->timeout_ms);
  if (n < 0)
    return -1;

  if (n == 0 || resp[0] == SYNC_BYTE) {
    /* Not connected yet (received sync echo, not command response) */
    return 0;
  }

  /* Read packet header (LNH, LNL, RES) to get length */
  n = ra_recv(dev, resp, 3, dev->timeout_ms);
  if (n < 3)
    return -1;

  /* Calculate remaining bytes: data_len + SUM + ETX */
  uint16_t data_len = ((uint16_t)resp[0] << 8) | resp[1];
  size_t remaining = (data_len > 1 ? data_len - 1 : 0) + 2;

  /* Drain the rest of the response to clear the buffer */
  uint8_t drain[256];
  while (remaining > 0) {
    size_t to_read = remaining > sizeof(drain) ? sizeof(drain) : remaining;
    n = ra_recv(dev, drain, to_read, dev->timeout_ms);
    if (n <= 0)
      break;
    remaining -= n;
  }

  /* Already connected */
  return 1;
}

static int
ra_confirm(ra_device_t *dev) {
  uint8_t cmd = GENERIC_CODE;
  uint8_t resp;
  DWORD bytes_written;

  for (int i = 0; i < dev->max_tries; i++) {
    if (!WriteFile(dev->fd, &cmd, 1, &bytes_written, NULL) || bytes_written != 1)
      continue;

    ssize_t n = ra_recv(dev, &resp, 1, dev->timeout_ms);
    if (n == 1) {
      if (resp == BOOT_CODE_M4) {
        fprintf(stderr, "Boot code 0xC3 (Cortex-M4/M23)\n");
        return 0;
      }
      if (resp == BOOT_CODE_M33) {
        fprintf(stderr, "Boot code 0xC6 (Cortex-M33)\n");
        return 0;
      }
      if (resp == BOOT_CODE_M85) {
        fprintf(stderr, "Boot code 0xC5 (Cortex-M85)\n");
        return 0;
      }
      /* Unexpected response */
      fprintf(stderr, "unexpected response: 0x%02X\n", resp);
    } else if (n == 0) {
      fprintf(stderr, "no response (try %d/%d)\n", i + 1, dev->max_tries);
    } else {
      fprintf(stderr, "read error: retry #%d\n", i);
    }
  }

  fprintf(stderr, "failed to establish connection after %d tries\n", dev->max_tries);
  return -1;
}

uint32_t
ra_best_baudrate(uint32_t max) {
  /* Rates in descending order */
  static const uint32_t rates[] = {
    4000000,
    3500000,
    3000000,
    2500000,
    2000000,
    1500000,
    1152000,
    1000000,
    921600,
    576000,
    500000,
    460800,
    230400,
    115200,
    57600,
    38400,
    19200,
    9600,
  };

  for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
    if (rates[i] <= max)
      return rates[i];
  }
  return 9600;
}

int
ra_set_baudrate(ra_device_t *dev, uint32_t baudrate) {
  uint8_t pkt[MAX_PKT_LEN];
  uint8_t resp[16];
  uint8_t data[4];
  ssize_t pkt_len, n;

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

  n = ra_recv(dev, resp, sizeof(resp), 500);
  if (n < 7) {
    fprintf(stderr, "short response for baud rate command (got %zd bytes)\n", n);
    return -1;
  }

  size_t data_len;
  uint8_t cmd;
  if (ra_unpack_pkt(resp, n, NULL, &data_len, &cmd) < 0) {
    fprintf(stderr, "baud rate setting failed\n");
    return -1;
  }

  /* Wait 1ms as per spec before changing local baudrate */
  Sleep(1);

  /* Change local serial port baudrate */
  if (set_serial_attrs(dev->fd, baudrate) < 0) {
    fprintf(stderr, "failed to set local baud rate to %u\n", baudrate);
    return -1;
  }

  dev->baudrate = baudrate;

  if (baudrate >= 1000000) {
    fprintf(stderr, "Baud rate changed to %.1f Mbps\n", baudrate / 1000000.0);
  } else if (baudrate >= 1000) {
    fprintf(stderr, "Baud rate changed to %.1f Kbps\n", baudrate / 1000.0);
  } else {
    fprintf(stderr, "Baud rate changed to %u bps\n", baudrate);
  }
  return 0;
}
