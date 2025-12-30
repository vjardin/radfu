/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Minimal terminal progress bar
 */

#include "progress.h"
#include <stdio.h>
#include <string.h>

#define BAR_WIDTH 30

void
progress_init(progress_t *p, size_t total, const char *desc) {
  p->total = total;
  p->current = 0;
  p->width = BAR_WIDTH;
  p->desc = desc;
  progress_update(p, 0);
}

void
progress_update(progress_t *p, size_t current) {
  p->current = current;

  int percent = 0;
  int filled = 0;

  if (p->total > 0) {
    percent = (int)((current * 100) / p->total);
    filled = (int)((current * p->width) / p->total);
  }

  if (filled > p->width)
    filled = p->width;
  if (percent > 100)
    percent = 100;

  char bar[BAR_WIDTH + 1];
  memset(bar, '.', p->width);
  memset(bar, '#', filled);
  bar[p->width] = '\0';

  fprintf(
      stderr, "\r%s: [%s] %3d%% (%zu/%zu)", p->desc ? p->desc : "", bar, percent, current, p->total
  );
  fflush(stderr);
}

void
progress_finish(progress_t *p) {
  progress_update(p, p->total);
  fprintf(stderr, "\n");
}
