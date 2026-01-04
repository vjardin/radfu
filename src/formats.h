/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * File format parsers and encoders for Intel HEX and Motorola S-record
 *
 * Design goal: small and simple implementation covering common use cases.
 * TODO: Consider using an upstream library (e.g., arkku/ihex, arkku/srec)
 * for full format compliance and edge case handling if needed.
 */

#ifndef FORMATS_H
#define FORMATS_H

#include <stddef.h>
#include <stdint.h>

/* Supported file formats (used for both input and output) */
typedef enum {
  FORMAT_AUTO, /* Auto-detect from file extension */
  FORMAT_BIN,  /* Raw binary */
  FORMAT_IHEX, /* Intel HEX */
  FORMAT_SREC, /* Motorola S-record */
} input_format_t;

/* Output format type (alias for clarity) */
typedef input_format_t output_format_t;

/* Parsed file data */
typedef struct {
  uint8_t *data;      /* Binary data buffer (caller must free) */
  size_t size;        /* Size of data in bytes */
  uint32_t base_addr; /* Base address from file (0 for binary) */
  int has_addr;       /* Non-zero if file contained address info */
} parsed_file_t;

/*
 * Detect input format from file extension
 * Returns FORMAT_BIN if extension not recognized
 */
input_format_t format_detect(const char *filename);

/*
 * Get format name string
 */
const char *format_name(input_format_t fmt);

/*
 * Parse input file according to format
 * If format is FORMAT_AUTO, detects from extension
 * out: parsed file data (caller must free out->data)
 * Returns: 0 on success, -1 on error
 */
int format_parse(const char *filename, input_format_t format, parsed_file_t *out);

/*
 * Parse Intel HEX file
 * Returns: 0 on success, -1 on error
 */
int ihex_parse(const char *filename, parsed_file_t *out);

/*
 * Parse Motorola S-record file
 * Returns: 0 on success, -1 on error
 */
int srec_parse(const char *filename, parsed_file_t *out);

/*
 * Parse raw binary file
 * Returns: 0 on success, -1 on error
 */
int bin_parse(const char *filename, parsed_file_t *out);

/*
 * Write data to file in specified format
 * If format is FORMAT_AUTO, detects from extension
 * Returns: 0 on success, -1 on error
 */
int format_write(
    const char *filename, output_format_t format, const uint8_t *data, size_t size, uint32_t addr);

/*
 * Write data as Intel HEX file
 * Returns: 0 on success, -1 on error
 */
int ihex_write(const char *filename, const uint8_t *data, size_t size, uint32_t addr);

/*
 * Write data as Motorola S-record file
 * Returns: 0 on success, -1 on error
 */
int srec_write(const char *filename, const uint8_t *data, size_t size, uint32_t addr);

#endif /* FORMATS_H */
