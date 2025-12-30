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
#include <limits.h>
#include <linux/limits.h>
#include <stdio.h>
#include <string.h>

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
