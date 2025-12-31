/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Linux-specific serial port detection using sysfs
 */

#define _DEFAULT_SOURCE

#include "raconnect.h"

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <limits.h>
#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

void
ra_print_usb_info(const char *tty_name) {
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

int
ra_find_port(char *buf, size_t len, char *tty_name, size_t tty_len) {
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

/*
 * Find USB device path in sysfs for Renesas RA bootloader
 * Returns: 0 on success with usb_dev filled (e.g., "1-6"), -1 on error
 */
static int
find_usb_device(char *usb_dev, size_t len) {
  DIR *dir;
  struct dirent *ent;
  char vid_path[PATH_MAX];
  char pid_path[PATH_MAX];
  char vid_str[16], pid_str[16];

  dir = opendir("/sys/bus/usb/devices");
  if (dir == NULL)
    return -1;

  while ((ent = readdir(dir)) != NULL) {
    /* Skip entries that don't look like USB device paths (e.g., "1-6") */
    if (ent->d_name[0] == '.' || strchr(ent->d_name, ':') != NULL)
      continue;

    snprintf(vid_path, sizeof(vid_path), "/sys/bus/usb/devices/%s/idVendor", ent->d_name);
    snprintf(pid_path, sizeof(pid_path), "/sys/bus/usb/devices/%s/idProduct", ent->d_name);

    if (read_sysfs_str(vid_path, vid_str, sizeof(vid_str)) < 0)
      continue;
    if (read_sysfs_str(pid_path, pid_str, sizeof(pid_str)) < 0)
      continue;

    unsigned int vid, pid;
    if (sscanf(vid_str, "%x", &vid) != 1 || sscanf(pid_str, "%x", &pid) != 1)
      continue;

    if (vid == RENESAS_VID && pid == RENESAS_PID) {
      closedir(dir);
      strncpy(usb_dev, ent->d_name, len);
      usb_dev[len - 1] = '\0';
      return 0;
    }
  }

  closedir(dir);
  return -1;
}

/*
 * USB power reset via sysfs authorized file.
 *
 * Note: This only resets the USB bus connection, NOT the MCU power.
 * The bootloader requires a hardware RESET (button press) to restart
 * its handshake sequence. This function waits for user confirmation
 * after the USB reset.
 */
int
ra_usb_reset(void) {
  char usb_dev[64];
  char auth_path[PATH_MAX];
  FILE *f;

  /* Find the Renesas USB device */
  if (find_usb_device(usb_dev, sizeof(usb_dev)) < 0) {
    warnx("Renesas RA USB Boot device not found");
    warnx("Check: J16 shorted, USB connected, board powered");
    return -1;
  }

  snprintf(auth_path, sizeof(auth_path), "/sys/bus/usb/devices/%s/authorized", usb_dev);

  fprintf(stderr, "Resetting USB device %s...\n", usb_dev);

  /* Deauthorize (unbind) */
  f = fopen(auth_path, "w");
  if (f == NULL) {
    if (errno == EACCES || errno == EPERM) {
      char exe_path[PATH_MAX];
      ssize_t plen = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
      if (plen > 0) {
        exe_path[plen] = '\0';
      } else {
        strcpy(exe_path, "radfu");
      }
      warnx("no permission to reset USB device %s", usb_dev);
      warnx("hint: grant CAP_DAC_OVERRIDE capability:");
      warnx("  sudo setcap cap_dac_override+ep %s", exe_path);
      warnx("or run with sudo");
    } else {
      warn("failed to open %s for writing", auth_path);
    }
    return -1;
  }
  fprintf(f, "0\n");
  fclose(f);

  /* Wait for device to disappear */
  usleep(500000); /* 500ms */

  /* Re-authorize (rebind) */
  f = fopen(auth_path, "w");
  if (f == NULL) {
    warn("failed to re-authorize USB device %s", usb_dev);
    return -1;
  }
  fprintf(f, "1\n");
  fclose(f);

  /* Wait for device to re-enumerate */
  fprintf(stderr, "Waiting for device to re-enumerate...\n");

  /* Poll for device to reappear (up to 5 seconds) */
  for (int i = 0; i < 10; i++) {
    usleep(500000); /* 500ms */
    if (find_usb_device(usb_dev, sizeof(usb_dev)) == 0) {
      fprintf(stderr, "Device re-enumerated as %s\n", usb_dev);
      fprintf(stderr, "Press RESET button on board, then press ENTER...");
      fflush(stderr);
      /* Wait for user to press ENTER */
      int c;
      while ((c = getchar()) != '\n' && c != EOF)
        ;
      return 0;
    }
  }

  warnx("device did not re-enumerate after USB reset");
  return -1;
}
