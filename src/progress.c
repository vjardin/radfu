/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Minimal terminal progress bar with optional callback for library integration
 */

#ifndef _WIN32
#define _DEFAULT_SOURCE /* clock_gettime, CLOCK_MONOTONIC */
#endif

#include "progress.h"
#include <stdio.h>
#include <string.h>

#define BAR_WIDTH 30

/* Global quiet mode */
int progress_global_quiet = 0;

#ifdef _WIN32
/* Windows: use QueryPerformanceCounter for high-resolution timing */
static LARGE_INTEGER perf_freq;
static int perf_freq_init = 0;

static void
get_current_time(progress_time_t *t) {
  QueryPerformanceCounter(t);
}

static double
get_elapsed_secs(const progress_time_t *start) {
  LARGE_INTEGER now;
  if (!perf_freq_init) {
    QueryPerformanceFrequency(&perf_freq);
    perf_freq_init = 1;
  }
  QueryPerformanceCounter(&now);
  return (double)(now.QuadPart - start->QuadPart) / (double)perf_freq.QuadPart;
}
#else
/* POSIX: use clock_gettime with CLOCK_MONOTONIC */
static void
get_current_time(progress_time_t *t) {
  clock_gettime(CLOCK_MONOTONIC, t);
}

static double
get_elapsed_secs(const progress_time_t *start) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  double secs = (double)(now.tv_sec - start->tv_sec);
  secs += (double)(now.tv_nsec - start->tv_nsec) / 1e9;
  return secs;
}
#endif

void
progress_init(progress_t *p, size_t total, const char *desc) {
  p->total = total;
  p->current = 0;
  p->width = BAR_WIDTH;
  p->desc = desc;
  p->callback = NULL;
  p->user_data = NULL;
  p->quiet = progress_global_quiet;
  get_current_time(&p->start_time);
  progress_update(p, 0);
}

void
progress_set_callback(progress_t *p, progress_cb_t cb, void *user_data) {
  p->callback = cb;
  p->user_data = user_data;
}

void
progress_set_quiet(progress_t *p, int quiet) {
  p->quiet = quiet;
}

void
progress_update(progress_t *p, size_t current) {
  p->current = current;

  /* Use callback if set */
  if (p->callback != NULL) {
    p->callback(current, p->total, p->desc, p->user_data);
    /* If quiet, skip default output even after callback */
    if (p->quiet)
      return;
  }

  /* Skip output in quiet mode */
  if (p->quiet)
    return;

  /* Default: print to stderr */
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

  /* Calculate speed and ETA */
  double elapsed = get_elapsed_secs(&p->start_time);
  char speed_str[32] = "";
  char eta_str[32] = "";

  if (elapsed > 0.1 && current > 0) {
    /* Speed in KB/s */
    double speed = (double)current / elapsed / 1024.0;
    if (speed >= 1000.0)
      snprintf(speed_str, sizeof(speed_str), " %.1f MB/s", speed / 1024.0);
    else
      snprintf(speed_str, sizeof(speed_str), " %.1f KB/s", speed);

    /* ETA calculation */
    if (current < p->total && speed > 0) {
      double remaining_bytes = (double)(p->total - current);
      double eta_secs = remaining_bytes / (speed * 1024.0);
      if (eta_secs < 60)
        snprintf(eta_str, sizeof(eta_str), " ETA %ds", (int)eta_secs);
      else if (eta_secs < 3600)
        snprintf(
            eta_str, sizeof(eta_str), " ETA %dm%02ds", (int)(eta_secs / 60), (int)eta_secs % 60);
      else
        snprintf(eta_str,
            sizeof(eta_str),
            " ETA %dh%02dm",
            (int)(eta_secs / 3600),
            (int)(eta_secs / 60) % 60);
    }
  }

  fprintf(stderr,
      "\r%s: [%s] %3d%% (%zu/%zu)%s%s    ",
      p->desc ? p->desc : "",
      bar,
      percent,
      current,
      p->total,
      speed_str,
      eta_str);
  fflush(stderr);
}

void
progress_finish(progress_t *p) {
  progress_update(p, p->total);
  if (p->callback == NULL && !p->quiet)
    fprintf(stderr, "\n");
}
