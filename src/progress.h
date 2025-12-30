/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Minimal terminal progress bar with optional callback for library integration
 */

#ifndef PROGRESS_H
#define PROGRESS_H

#include <stddef.h>

/*
 * Progress callback function type
 * Called on each progress update with current/total values
 * user_data: opaque pointer passed to progress_set_callback
 */
typedef void (*progress_cb_t)(size_t current, size_t total, const char *desc, void *user_data);

typedef struct {
  size_t total;
  size_t current;
  int width;
  const char *desc;
  progress_cb_t callback;
  void *user_data;
} progress_t;

void progress_init(progress_t *p, size_t total, const char *desc);
void progress_update(progress_t *p, size_t current);
void progress_finish(progress_t *p);

/*
 * Set a callback for progress updates (for library integration)
 * If set, the callback is called instead of printing to stderr
 */
void progress_set_callback(progress_t *p, progress_cb_t cb, void *user_data);

#endif /* PROGRESS_H */
