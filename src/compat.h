/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Platform compatibility layer
 *
 * This header provides a unified interface for platform-specific functionality.
 * Include this header instead of using #ifdef _WIN32 throughout the codebase.
 */

#ifndef COMPAT_H
#define COMPAT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

#ifdef _WIN32
/*
 * Windows platform
 */
#include <windows.h>
#include <basetsd.h>
#include <io.h>
#include <process.h>
#include <fcntl.h>

/* Type definitions */
typedef HANDLE ra_fd_t;
typedef SSIZE_T ssize_t;
#define RA_INVALID_FD INVALID_HANDLE_VALUE

/* PATH_MAX equivalent */
#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif

/* POSIX to Windows file operation mappings */
#define open _open
#define read _read
#define write _write
#define close _close
#define unlink _unlink

/* POSIX to Windows file mode flag mappings */
#define O_CREAT _O_CREAT
#define O_TRUNC _O_TRUNC
#define O_RDONLY _O_RDONLY
#define O_WRONLY _O_WRONLY
#define O_RDWR _O_RDWR
#define S_IRUSR _S_IREAD
#define S_IWUSR _S_IWRITE

/* String functions */
#define strcasecmp _stricmp

/* getopt - use local implementation */
#include "getopt.h"

/* mkstemp - implemented in compat.c */
int mkstemp(char *tmpl);

/*
 * err.h compatibility - Windows doesn't have <err.h>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

static inline void
warn(const char *fmt, ...) {
  va_list ap;
  DWORD err = GetLastError();
  char errbuf[256];

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);

  FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      NULL,
      err,
      0,
      errbuf,
      sizeof(errbuf),
      NULL);
  size_t len = strlen(errbuf);
  if (len > 0 && errbuf[len - 1] == '\n')
    errbuf[--len] = '\0';
  if (len > 0 && errbuf[len - 1] == '\r')
    errbuf[--len] = '\0';

  fprintf(stderr, ": %s\n", errbuf);
}

static inline void
warnx(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, "\n");
}

static inline void
err(int eval, const char *fmt, ...) {
  va_list ap;
  DWORD error = GetLastError();
  char errbuf[256];

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);

  FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      NULL,
      error,
      0,
      errbuf,
      sizeof(errbuf),
      NULL);
  size_t len = strlen(errbuf);
  if (len > 0 && errbuf[len - 1] == '\n')
    errbuf[--len] = '\0';
  if (len > 0 && errbuf[len - 1] == '\r')
    errbuf[--len] = '\0';

  fprintf(stderr, ": %s\n", errbuf);
  exit(eval);
}

static inline void
errx(int eval, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, "\n");
  exit(eval);
}

#else
/*
 * POSIX platforms (Linux, macOS)
 */
#include <sys/types.h>
#include <unistd.h>
#include <strings.h>
#include <getopt.h>
#include <err.h>

typedef int ra_fd_t;
#define RA_INVALID_FD -1

#endif /* _WIN32 */

/*
 * Cross-platform functions (implemented in compat.c)
 */

/* Get temp directory path */
const char *get_temp_dir(void);

/* Get path separator character */
static inline char
path_separator(void) {
#ifdef _WIN32
  return '\\';
#else
  return '/';
#endif
}

#endif /* COMPAT_H */
