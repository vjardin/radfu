/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Platform compatibility layer implementation
 */

#include "compat.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <sys/stat.h>
#include <direct.h>

/*
 * mkstemp replacement for Windows
 * Template must end with XXXXXX which gets replaced with unique chars
 * Returns fd on success, -1 on error
 */
int
mkstemp(char *tmpl) {
  size_t len = strlen(tmpl);
  if (len < 6)
    return -1;

  char *suffix = tmpl + len - 6;
  if (strcmp(suffix, "XXXXXX") != 0)
    return -1;

  /* Generate unique suffix using pid and counter */
  static int counter = 0;
  snprintf(suffix, 7, "%04x%02x", (unsigned)_getpid() & 0xFFFF, counter++ & 0xFF);

  return _open(tmpl, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
}

/*
 * mkdtemp replacement for Windows
 * Template must end with XXXXXX which gets replaced with unique chars
 * Returns template on success, NULL on error
 */
char *
mkdtemp(char *tmpl) {
  size_t len = strlen(tmpl);
  if (len < 6)
    return NULL;

  char *suffix = tmpl + len - 6;
  if (strcmp(suffix, "XXXXXX") != 0)
    return NULL;

  /* Generate unique suffix using pid and counter */
  static int counter = 0;
  snprintf(suffix, 7, "%04x%02x", (unsigned)_getpid() & 0xFFFF, counter++ & 0xFF);

  if (_mkdir(tmpl) != 0)
    return NULL;

  return tmpl;
}

/*
 * Get temp directory path for Windows
 * Tries TEMP, then TMP, falls back to current directory
 */
const char *
get_temp_dir(void) {
  const char *tmpdir = getenv("TEMP");
  if (tmpdir == NULL)
    tmpdir = getenv("TMP");
  if (tmpdir == NULL)
    tmpdir = ".";
  return tmpdir;
}

#else /* POSIX */

/*
 * Get temp directory path for POSIX
 * Tries TMPDIR, falls back to /tmp
 */
const char *
get_temp_dir(void) {
  const char *tmpdir = getenv("TMPDIR");
  if (tmpdir == NULL)
    tmpdir = "/tmp";
  return tmpdir;
}

#endif /* _WIN32 */
