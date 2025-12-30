/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Minimal terminal progress bar
 */

#ifndef PROGRESS_H
#define PROGRESS_H

#include <stddef.h>

typedef struct {
  size_t total;
  size_t current;
  int width;
  const char *desc;
} progress_t;

void progress_init(progress_t *p, size_t total, const char *desc);
void progress_update(progress_t *p, size_t current);
void progress_finish(progress_t *p);

#endif /* PROGRESS_H */
