/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Windows-specific serial port detection using SetupAPI
 */

#include "raconnect.h"

#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <initguid.h>
#include <ntddser.h>
#include <devpkey.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Known USB-serial adapters and their max reliable baud rates
 */
struct usb_serial_adapter {
  uint16_t vid;
  uint16_t pid;
  uint32_t max_baud;
  const char *name;
};

static const struct usb_serial_adapter known_adapters[] = {
  /* FTDI */
  { 0x0403, 0x6001, 3000000, "FTDI FT232R" },
  { 0x0403, 0x6010, 3000000, "FTDI FT2232" },
  { 0x0403, 0x6011, 3000000, "FTDI FT4232" },
  { 0x0403, 0x6014, 4000000, "FTDI FT232H" },
  { 0x0403, 0x6015, 3000000, "FTDI FT231X" },
  /* Silicon Labs */
  { 0x10c4, 0xea60, 1000000, "CP2102"      },
  { 0x10c4, 0xea61, 2000000, "CP2104"      },
  { 0x10c4, 0xea70, 3000000, "CP2105"      },
  /* WCH */
  { 0x1a86, 0x7523, 2000000, "CH340"       },
  { 0x1a86, 0x5523, 2000000, "CH341"       },
  /* Prolific */
  { 0x067b, 0x2303, 1000000, "PL2303"      },
  { 0x067b, 0x23a3, 1000000, "PL2303HXD"   },
  { 0,      0,      0,       NULL          }
};

/*
 * Parse VID and PID from Windows hardware ID string
 * Format: USB\VID_045B&PID_0261&...
 */
static int
parse_vid_pid(const char *hwid, unsigned int *vid, unsigned int *pid) {
  const char *vid_ptr = strstr(hwid, "VID_");
  const char *pid_ptr = strstr(hwid, "PID_");

  if (vid_ptr == NULL || pid_ptr == NULL)
    return -1;

  if (sscanf(vid_ptr, "VID_%04X", vid) != 1)
    return -1;
  if (sscanf(pid_ptr, "PID_%04X", pid) != 1)
    return -1;

  return 0;
}

/*
 * Get the COM port name from a device info set entry
 */
static int
get_com_port_name(HDEVINFO dev_info, SP_DEVINFO_DATA *dev_data, char *port_name, size_t port_len) {
  HKEY hKey;
  DWORD type;
  DWORD size = (DWORD)port_len;

  hKey = SetupDiOpenDevRegKey(dev_info, dev_data, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
  if (hKey == INVALID_HANDLE_VALUE)
    return -1;

  if (RegQueryValueExA(hKey, "PortName", NULL, &type, (LPBYTE)port_name, &size) != ERROR_SUCCESS) {
    RegCloseKey(hKey);
    return -1;
  }

  RegCloseKey(hKey);
  return 0;
}

/*
 * Get USB device properties (VID, PID, manufacturer, product, serial)
 */
static int
get_usb_device_info(HDEVINFO dev_info,
    SP_DEVINFO_DATA *dev_data,
    unsigned int *vid,
    unsigned int *pid,
    char *manufacturer,
    size_t mfg_len,
    char *product,
    size_t prod_len,
    char *serial,
    size_t serial_len) {
  char hwid[512];
  DWORD size;
  DWORD type;

  /* Get hardware ID to extract VID/PID */
  size = sizeof(hwid);
  if (!SetupDiGetDeviceRegistryPropertyA(
          dev_info, dev_data, SPDRP_HARDWAREID, &type, (PBYTE)hwid, size, &size)) {
    return -1;
  }

  if (parse_vid_pid(hwid, vid, pid) < 0)
    return -1;

  /* Get manufacturer */
  size = (DWORD)mfg_len;
  if (!SetupDiGetDeviceRegistryPropertyA(
          dev_info, dev_data, SPDRP_MFG, &type, (PBYTE)manufacturer, size, &size)) {
    strncpy(manufacturer, "Unknown", mfg_len);
    manufacturer[mfg_len - 1] = '\0';
  }

  /* Get device description (product name) */
  size = (DWORD)prod_len;
  if (!SetupDiGetDeviceRegistryPropertyA(
          dev_info, dev_data, SPDRP_DEVICEDESC, &type, (PBYTE)product, size, &size)) {
    strncpy(product, "Unknown", prod_len);
    product[prod_len - 1] = '\0';
  }

  /* Try to get serial number from location info or instance ID */
  size = (DWORD)serial_len;
  if (!SetupDiGetDeviceInstanceIdA(dev_info, dev_data, serial, size, &size)) {
    strncpy(serial, "N/A", serial_len);
    serial[serial_len - 1] = '\0';
  } else {
    /* Extract serial from instance ID if present (after last backslash) */
    char *last_sep = strrchr(serial, '\\');
    if (last_sep != NULL) {
      memmove(serial, last_sep + 1, strlen(last_sep));
    }
  }

  return 0;
}

void
ra_print_usb_info(const char *tty_name) {
  HDEVINFO dev_info;
  SP_DEVINFO_DATA dev_data;
  DWORD idx = 0;
  char port_name[32];
  unsigned int vid, pid;
  char manufacturer[128], product[128], serial[64];

  dev_info = SetupDiGetClassDevsA(
      &GUID_DEVINTERFACE_COMPORT, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (dev_info == INVALID_HANDLE_VALUE)
    return;

  dev_data.cbSize = sizeof(SP_DEVINFO_DATA);

  while (SetupDiEnumDeviceInfo(dev_info, idx++, &dev_data)) {
    if (get_com_port_name(dev_info, &dev_data, port_name, sizeof(port_name)) < 0)
      continue;

    /* Check if this is the port we're looking for */
    if (_stricmp(port_name, tty_name) != 0)
      continue;

    if (get_usb_device_info(dev_info,
            &dev_data,
            &vid,
            &pid,
            manufacturer,
            sizeof(manufacturer),
            product,
            sizeof(product),
            serial,
            sizeof(serial)) == 0) {
      fprintf(stderr,
          "USB device: %s %s [%04X:%04X] serial=%s\n",
          manufacturer,
          product,
          vid,
          pid,
          serial);
      fprintf(stderr, "COM port:   %s\n", tty_name);
    }
    break;
  }

  SetupDiDestroyDeviceInfoList(dev_info);
}

uint32_t
ra_get_adapter_max_baudrate(const char *tty_name) {
  HDEVINFO dev_info;
  SP_DEVINFO_DATA dev_data;
  DWORD idx = 0;
  char port_name[32];
  unsigned int vid, pid;
  char manufacturer[128], product[128], serial[64];

  if (tty_name == NULL)
    return 115200;

  dev_info = SetupDiGetClassDevsA(
      &GUID_DEVINTERFACE_COMPORT, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (dev_info == INVALID_HANDLE_VALUE)
    return 115200;

  dev_data.cbSize = sizeof(SP_DEVINFO_DATA);

  while (SetupDiEnumDeviceInfo(dev_info, idx++, &dev_data)) {
    if (get_com_port_name(dev_info, &dev_data, port_name, sizeof(port_name)) < 0)
      continue;

    if (_stricmp(port_name, tty_name) != 0)
      continue;

    if (get_usb_device_info(dev_info,
            &dev_data,
            &vid,
            &pid,
            manufacturer,
            sizeof(manufacturer),
            product,
            sizeof(product),
            serial,
            sizeof(serial)) < 0) {
      SetupDiDestroyDeviceInfoList(dev_info);
      return 115200;
    }

    SetupDiDestroyDeviceInfoList(dev_info);

    /* Look up adapter in known list */
    for (const struct usb_serial_adapter *a = known_adapters; a->name != NULL; a++) {
      if (a->vid == vid && a->pid == pid) {
        if (a->max_baud >= 1000000) {
          fprintf(stderr, "Adapter: %s (max %.0f Mbps)\n", a->name, a->max_baud / 1000000.0);
        } else {
          fprintf(stderr, "Adapter: %s (max %.0f Kbps)\n", a->name, a->max_baud / 1000.0);
        }
        return a->max_baud;
      }
    }

    /* Unknown adapter */
    fprintf(stderr, "Unknown USB-serial adapter [%04X:%04X], using 115200 bps max\n", vid, pid);
    return 115200;
  }

  SetupDiDestroyDeviceInfoList(dev_info);
  return 115200;
}

int
ra_find_port(char *buf, size_t len, char *tty_name, size_t tty_len) {
  HDEVINFO dev_info;
  SP_DEVINFO_DATA dev_data;
  DWORD idx = 0;
  char port_name[32];
  unsigned int vid, pid;
  char manufacturer[128], product[128], serial[64];

  dev_info = SetupDiGetClassDevsA(
      &GUID_DEVINTERFACE_COMPORT, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (dev_info == INVALID_HANDLE_VALUE) {
    fprintf(stderr, "Failed to enumerate COM ports\n");
    return -1;
  }

  dev_data.cbSize = sizeof(SP_DEVINFO_DATA);

  while (SetupDiEnumDeviceInfo(dev_info, idx++, &dev_data)) {
    if (get_com_port_name(dev_info, &dev_data, port_name, sizeof(port_name)) < 0)
      continue;

    if (get_usb_device_info(dev_info,
            &dev_data,
            &vid,
            &pid,
            manufacturer,
            sizeof(manufacturer),
            product,
            sizeof(product),
            serial,
            sizeof(serial)) < 0)
      continue;

    /* Check for Renesas device */
    if (vid == RENESAS_VID) {
      SetupDiDestroyDeviceInfoList(dev_info);

      if (strlen(port_name) < len) {
        strncpy(buf, port_name, len);
        buf[len - 1] = '\0';
        if (tty_name != NULL && tty_len > 0) {
          strncpy(tty_name, port_name, tty_len);
          tty_name[tty_len - 1] = '\0';
        }
        return 0;
      }
      return -1;
    }
  }

  SetupDiDestroyDeviceInfoList(dev_info);
  fprintf(stderr, "no Renesas device found\n");
  return -1;
}
