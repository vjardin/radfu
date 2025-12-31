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

int
ra_usb_reset(void) {
  fprintf(stderr, "USB reset not supported on macOS\n");
  fprintf(stderr, "Please manually reset the board using the RESET button\n");
  return -1;
}
