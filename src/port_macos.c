/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * macOS-specific serial port detection using IOKit
 */

#include "raconnect.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>
#include <IOKit/usb/IOUSBLib.h>

static bool
get_iokit_usb_property(io_service_t service, CFStringRef key, char *buf, size_t len) {
  CFTypeRef prop = IORegistryEntrySearchCFProperty(service,
      kIOServicePlane,
      key,
      kCFAllocatorDefault,
      kIORegistryIterateRecursively | kIORegistryIterateParents);
  if (prop == NULL)
    return false;

  bool result = false;
  if (CFGetTypeID(prop) == CFStringGetTypeID()) {
    result = CFStringGetCString((CFStringRef)prop, buf, len, kCFStringEncodingUTF8);
  } else if (CFGetTypeID(prop) == CFNumberGetTypeID()) {
    int value;
    if (CFNumberGetValue((CFNumberRef)prop, kCFNumberIntType, &value)) {
      snprintf(buf, len, "%04x", value);
      result = true;
    }
  }
  CFRelease(prop);
  return result;
}

static int
get_iokit_usb_int_property(io_service_t service, CFStringRef key) {
  CFTypeRef prop = IORegistryEntrySearchCFProperty(service,
      kIOServicePlane,
      key,
      kCFAllocatorDefault,
      kIORegistryIterateRecursively | kIORegistryIterateParents);
  if (prop == NULL)
    return -1;

  int value = -1;
  if (CFGetTypeID(prop) == CFNumberGetTypeID()) {
    CFNumberGetValue((CFNumberRef)prop, kCFNumberIntType, &value);
  }
  CFRelease(prop);
  return value;
}

void
ra_print_usb_info(const char *tty_name) {
  char vid[16] = "????", pid[16] = "????";
  char manufacturer[128] = "Unknown", product[128] = "Unknown", serial[64] = "N/A";

  /* Find the IOKit service for this serial port */
  CFMutableDictionaryRef match = IOServiceMatching(kIOSerialBSDServiceValue);
  if (match != NULL) {
    CFStringRef path_cf =
        CFStringCreateWithCString(kCFAllocatorDefault, tty_name, kCFStringEncodingUTF8);
    if (path_cf != NULL) {
      CFDictionarySetValue(match, CFSTR(kIOCalloutDeviceKey), path_cf);
      io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, match);
      match = NULL; /* IOServiceGetMatchingService consumes match */

      if (service != IO_OBJECT_NULL) {
        get_iokit_usb_property(service, CFSTR("idVendor"), vid, sizeof(vid));
        get_iokit_usb_property(service, CFSTR("idProduct"), pid, sizeof(pid));
        get_iokit_usb_property(
            service, CFSTR("USB Vendor Name"), manufacturer, sizeof(manufacturer));
        get_iokit_usb_property(service, CFSTR("USB Product Name"), product, sizeof(product));
        get_iokit_usb_property(service, CFSTR("USB Serial Number"), serial, sizeof(serial));
        IOObjectRelease(service);
      }
      CFRelease(path_cf);
    }
  }

  fprintf(stderr, "USB device: %s %s [%s:%s] serial=%s\n", manufacturer, product, vid, pid, serial);
  fprintf(stderr, "TTY port:   %s\n", tty_name);
}

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

uint32_t
ra_get_adapter_max_baudrate(const char *tty_name) {
  if (tty_name == NULL)
    return 115200;

  int vid = -1, pid = -1;

  CFMutableDictionaryRef match = IOServiceMatching(kIOSerialBSDServiceValue);
  if (match != NULL) {
    CFStringRef path_cf =
        CFStringCreateWithCString(kCFAllocatorDefault, tty_name, kCFStringEncodingUTF8);
    if (path_cf != NULL) {
      CFDictionarySetValue(match, CFSTR(kIOCalloutDeviceKey), path_cf);
      io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, match);
      match = NULL;

      if (service != IO_OBJECT_NULL) {
        vid = get_iokit_usb_int_property(service, CFSTR("idVendor"));
        pid = get_iokit_usb_int_property(service, CFSTR("idProduct"));
        IOObjectRelease(service);
      }
      CFRelease(path_cf);
    }
  }

  if (vid < 0 || pid < 0)
    return 115200;

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

  fprintf(stderr, "Unknown USB-serial adapter [%04x:%04x], using 115200 bps max\n", vid, pid);
  return 115200;
}

int
ra_find_port(char *buf, size_t len, char *tty_name, size_t tty_len) {
  io_iterator_t iter;
  io_service_t service;
  kern_return_t kr;

  CFMutableDictionaryRef match = IOServiceMatching(kIOSerialBSDServiceValue);
  if (match == NULL)
    return -1;

  CFDictionarySetValue(match, CFSTR(kIOSerialBSDTypeKey), CFSTR(kIOSerialBSDAllTypes));

  kr = IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter);
  if (kr != KERN_SUCCESS)
    return -1;

  while ((service = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
    int vid = get_iokit_usb_int_property(service, CFSTR("idVendor"));

    if (vid == RENESAS_VID) {
      CFStringRef path_cf = IORegistryEntryCreateCFProperty(
          service, CFSTR(kIOCalloutDeviceKey), kCFAllocatorDefault, 0);
      if (path_cf != NULL) {
        char path[PATH_MAX];
        if (CFStringGetCString(path_cf, path, sizeof(path), kCFStringEncodingUTF8)) {
          CFRelease(path_cf);
          IOObjectRelease(service);
          IOObjectRelease(iter);

          if (strlen(path) < len) {
            strncpy(buf, path, len);
            buf[len - 1] = '\0';
            if (tty_name != NULL && tty_len > 0) {
              strncpy(tty_name, path, tty_len);
              tty_name[tty_len - 1] = '\0';
            }
            return 0;
          }
          return -1;
        }
        CFRelease(path_cf);
      }
    }
    IOObjectRelease(service);
  }

  IOObjectRelease(iter);
  return -1;
}
