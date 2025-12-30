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

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define GENERIC_CODE 0x55
#define BOOT_CODE_M4 0xC3  /* Cortex-M4/M23 (RA2/RA4 series) */
#define BOOT_CODE_M33 0xC6 /* Cortex-M33 (RA6 series) */

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

static int
read_sysfs_str(const char *path, char *buf, size_t len) {
  FILE *f = fopen(path, "r");
  if (f == NULL)
    return -1;
  if (fgets(buf, len, f) == NULL) {
    fclose(f);
    return -1;
  }
  fclose(f);
  /* Remove trailing newline */
  size_t slen = strlen(buf);
  if (slen > 0 && buf[slen - 1] == '\n')
    buf[slen - 1] = '\0';
  return 0;
}

static void
print_usb_info(const char *tty_name) {
  char path[512];
  char vid[16], pid[16], manufacturer[128], product[128], serial[64];

  snprintf(path, sizeof(path), "/sys/class/tty/%s/device/../idVendor", tty_name);
  if (read_sysfs_str(path, vid, sizeof(vid)) < 0)
    strcpy(vid, "????");

  snprintf(path, sizeof(path), "/sys/class/tty/%s/device/../idProduct", tty_name);
  if (read_sysfs_str(path, pid, sizeof(pid)) < 0)
    strcpy(pid, "????");

  snprintf(path, sizeof(path), "/sys/class/tty/%s/device/../manufacturer", tty_name);
  if (read_sysfs_str(path, manufacturer, sizeof(manufacturer)) < 0)
    strcpy(manufacturer, "Unknown");

  snprintf(path, sizeof(path), "/sys/class/tty/%s/device/../product", tty_name);
  if (read_sysfs_str(path, product, sizeof(product)) < 0)
    strcpy(product, "Unknown");

  snprintf(path, sizeof(path), "/sys/class/tty/%s/device/../serial", tty_name);
  if (read_sysfs_str(path, serial, sizeof(serial)) < 0)
    strcpy(serial, "N/A");

  fprintf(stderr, "USB device: %s %s [%s:%s] serial=%s\n", manufacturer, product, vid, pid, serial);
  fprintf(stderr, "TTY port:   /dev/%s\n", tty_name);
}

static int
find_port(char *buf, size_t len, char *tty_name, size_t tty_len) {
  DIR *dir;
  struct dirent *ent;
  char path[PATH_MAX];
  char vid_path[PATH_MAX];
  char vid_str[16];
  FILE *f;

  dir = opendir("/sys/class/tty");
  if (dir == NULL)
    return -1;

  while ((ent = readdir(dir)) != NULL) {
    if (strncmp(ent->d_name, "ttyACM", 6) != 0 && strncmp(ent->d_name, "ttyUSB", 6) != 0)
      continue;

    snprintf(vid_path, sizeof(vid_path), "/sys/class/tty/%s/device/../idVendor", ent->d_name);
    f = fopen(vid_path, "r");
    if (f == NULL)
      continue;

    if (fgets(vid_str, sizeof(vid_str), f) != NULL) {
      unsigned int vid;
      if (sscanf(vid_str, "%x", &vid) == 1 && vid == RENESAS_VID) {
        snprintf(path, sizeof(path), "/dev/%s", ent->d_name);
        fclose(f);
        closedir(dir);
        if (strlen(path) < len) {
          strncpy(buf, path, len);
          buf[len - 1] = '\0';
          if (tty_name != NULL && tty_len > 0) {
            strncpy(tty_name, ent->d_name, tty_len);
            tty_name[tty_len - 1] = '\0';
          }
          return 0;
        }
        return -1;
      }
    }
    fclose(f);
  }

  closedir(dir);
  return -1;
}

int
ra_open(ra_device_t *dev, const char *port) {
  char portbuf[256];
  char tty_name[64];
  bool auto_detect = false;

  if (port == NULL) {
    if (find_port(portbuf, sizeof(portbuf), tty_name, sizeof(tty_name)) < 0) {
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
  print_usb_info(tty_name);
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

  /* Try to establish connection */
  int status = ra_inquire(dev);
  if (status < 0) {
    close(dev->fd);
    dev->fd = -1;
    return -1;
  }

  if (status == 0) {
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
        return 0;
      }
      if (resp == BOOT_CODE_M33) {
        fprintf(stderr, "Boot code 0xC6 (Cortex-M33)\n");
        return 0;
      }
    }

    if (n < 0)
      warn("timeout: retry #%d", i);
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
  case 230400:
    return B230400;
  case 460800:
    return B460800;
  case 500000:
    return B500000;
  case 576000:
    return B576000;
  case 921600:
    return B921600;
  case 1000000:
    return B1000000;
  case 1152000:
    return B1152000;
  case 1500000:
    return B1500000;
  case 2000000:
    return B2000000;
  case 2500000:
    return B2500000;
  case 3000000:
    return B3000000;
  case 3500000:
    return B3500000;
  case 4000000:
    return B4000000;
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
