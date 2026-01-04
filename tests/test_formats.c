/*
 * Copyright (C) Vincent Jardin <vjardin@free.fr> Free Mobile 2025
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Unit tests for input file format parsers
 */

#define _DEFAULT_SOURCE

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/compat.h"
#include "../src/formats.h"

static char temp_dir[256];

static int
setup(void **state) {
  (void)state;
  snprintf(temp_dir, sizeof(temp_dir), "%s%ctest_formats.XXXXXX", get_temp_dir(), path_separator());
  if (mkdtemp(temp_dir) == NULL)
    return -1;
  return 0;
}

static int
teardown(void **state) {
  (void)state;
  char cmd[512];
#ifdef _WIN32
  snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\"", temp_dir);
#else
  snprintf(cmd, sizeof(cmd), "rm -rf '%s'", temp_dir);
#endif
  return system(cmd);
}

static void
write_file(const char *filename, const char *content) {
  FILE *fp = fopen(filename, "w");
  assert_non_null(fp);
  fputs(content, fp);
  fclose(fp);
}

/*
 * Format detection tests
 */

static void
test_format_detect_bin(void **state) {
  (void)state;
  assert_int_equal(format_detect("firmware.bin"), FORMAT_BIN);
  assert_int_equal(format_detect("test.dat"), FORMAT_BIN);
  assert_int_equal(format_detect("noext"), FORMAT_BIN);
}

static void
test_format_detect_ihex(void **state) {
  (void)state;
  assert_int_equal(format_detect("firmware.hex"), FORMAT_IHEX);
  assert_int_equal(format_detect("firmware.HEX"), FORMAT_IHEX);
  assert_int_equal(format_detect("test.ihex"), FORMAT_IHEX);
}

static void
test_format_detect_srec(void **state) {
  (void)state;
  assert_int_equal(format_detect("firmware.srec"), FORMAT_SREC);
  assert_int_equal(format_detect("firmware.s19"), FORMAT_SREC);
  assert_int_equal(format_detect("firmware.s28"), FORMAT_SREC);
  assert_int_equal(format_detect("firmware.s37"), FORMAT_SREC);
  assert_int_equal(format_detect("firmware.mot"), FORMAT_SREC);
}

static void
test_format_name(void **state) {
  (void)state;
  assert_string_equal(format_name(FORMAT_AUTO), "auto");
  assert_string_equal(format_name(FORMAT_BIN), "binary");
  assert_string_equal(format_name(FORMAT_IHEX), "Intel HEX");
  assert_string_equal(format_name(FORMAT_SREC), "Motorola S-record");
}

/*
 * Binary parser tests
 */

static void
test_bin_parse(void **state) {
  (void)state;
  char filename[512];
  parsed_file_t out;
  uint8_t data[] = { 0x00, 0x20, 0x00, 0x20, 0xC1, 0x01, 0x00, 0x00 };

  snprintf(filename, sizeof(filename), "%s/test.bin", temp_dir);

  FILE *fp = fopen(filename, "wb");
  assert_non_null(fp);
  fwrite(data, 1, sizeof(data), fp);
  fclose(fp);

  assert_int_equal(bin_parse(filename, &out), 0);
  assert_int_equal(out.size, sizeof(data));
  assert_int_equal(out.base_addr, 0);
  assert_int_equal(out.has_addr, 0);
  assert_memory_equal(out.data, data, sizeof(data));

  free(out.data);
}

/*
 * Intel HEX parser tests
 */

static void
test_ihex_simple(void **state) {
  (void)state;
  char filename[512];
  parsed_file_t out;

  /* Simple Intel HEX with 16 bytes at address 0x0000 */
  const char *ihex = ":10000000000102030405060708090A0B0C0D0E0F78\n"
                     ":00000001FF\n";

  snprintf(filename, sizeof(filename), "%s/test.hex", temp_dir);
  write_file(filename, ihex);

  assert_int_equal(ihex_parse(filename, &out), 0);
  assert_int_equal(out.size, 16);
  assert_int_equal(out.base_addr, 0);
  assert_int_equal(out.has_addr, 1);

  for (int i = 0; i < 16; i++)
    assert_int_equal(out.data[i], i);

  free(out.data);
}

static void
test_ihex_extended_addr(void **state) {
  (void)state;
  char filename[512];
  parsed_file_t out;

  /* Intel HEX with extended linear address (0x08000000) */
  const char *ihex = ":020000040800F2\n"
                     ":10000000DEADBEEFCAFEBABE010203040506070854\n"
                     ":00000001FF\n";

  snprintf(filename, sizeof(filename), "%s/test_ext.hex", temp_dir);
  write_file(filename, ihex);

  assert_int_equal(ihex_parse(filename, &out), 0);
  assert_int_equal(out.size, 16);
  assert_int_equal(out.base_addr, 0x08000000);
  assert_int_equal(out.has_addr, 1);
  assert_int_equal(out.data[0], 0xDE);
  assert_int_equal(out.data[1], 0xAD);

  free(out.data);
}

static void
test_ihex_segment_addr(void **state) {
  (void)state;
  char filename[512];
  parsed_file_t out;

  /* Intel HEX with extended segment address */
  const char *ihex = ":020000021000EC\n"
                     ":04000000AABBCCDDEE\n"
                     ":00000001FF\n";

  snprintf(filename, sizeof(filename), "%s/test_seg.hex", temp_dir);
  write_file(filename, ihex);

  assert_int_equal(ihex_parse(filename, &out), 0);
  assert_int_equal(out.size, 4);
  assert_int_equal(out.base_addr, 0x00010000);
  assert_int_equal(out.has_addr, 1);

  free(out.data);
}

static void
test_ihex_bad_checksum(void **state) {
  (void)state;
  char filename[512];
  parsed_file_t out;

  /* Bad checksum (should be 78, not 79) */
  const char *ihex = ":10000000000102030405060708090A0B0C0D0E0F79\n"
                     ":00000001FF\n";

  snprintf(filename, sizeof(filename), "%s/bad.hex", temp_dir);
  write_file(filename, ihex);

  assert_int_equal(ihex_parse(filename, &out), -1);
}

static void
test_ihex_no_eof(void **state) {
  (void)state;
  char filename[512];
  parsed_file_t out;

  /* Missing EOF record */
  const char *ihex = ":10000000000102030405060708090A0B0C0D0E0F78\n";

  snprintf(filename, sizeof(filename), "%s/noeof.hex", temp_dir);
  write_file(filename, ihex);

  assert_int_equal(ihex_parse(filename, &out), -1);
}

/*
 * Motorola S-record parser tests
 */

static void
test_srec_s19(void **state) {
  (void)state;
  char filename[512];
  parsed_file_t out;

  /* S19 format: 16-bit addresses */
  const char *srec = "S00600004844521B\n"
                     "S1130000000102030405060708090A0B0C0D0E0F74\n"
                     "S9030000FC\n";

  snprintf(filename, sizeof(filename), "%s/test.s19", temp_dir);
  write_file(filename, srec);

  assert_int_equal(srec_parse(filename, &out), 0);
  assert_int_equal(out.size, 16);
  assert_int_equal(out.base_addr, 0);
  assert_int_equal(out.has_addr, 1);

  for (int i = 0; i < 16; i++)
    assert_int_equal(out.data[i], i);

  free(out.data);
}

static void
test_srec_s2(void **state) {
  (void)state;
  char filename[512];
  parsed_file_t out;

  /* S2 format: 24-bit addresses */
  const char *srec = "S0030000FC\n"
                     "S214080000DEADBEEFCAFEBABE010203040506070847\n"
                     "S804000000FB\n";

  snprintf(filename, sizeof(filename), "%s/test_s2.srec", temp_dir);
  write_file(filename, srec);

  assert_int_equal(srec_parse(filename, &out), 0);
  assert_int_equal(out.size, 16);
  assert_int_equal(out.base_addr, 0x080000);
  assert_int_equal(out.has_addr, 1);
  assert_int_equal(out.data[0], 0xDE);
  assert_int_equal(out.data[1], 0xAD);

  free(out.data);
}

static void
test_srec_s3(void **state) {
  (void)state;
  char filename[512];
  parsed_file_t out;

  /* S3 format: 32-bit addresses */
  const char *srec = "S0030000FC\n"
                     "S31508000000AABBCCDD112233445566778899AABBCCA6\n"
                     "S70500000000FA\n";

  snprintf(filename, sizeof(filename), "%s/test_s3.srec", temp_dir);
  write_file(filename, srec);

  assert_int_equal(srec_parse(filename, &out), 0);
  assert_int_equal(out.size, 16);
  assert_int_equal(out.base_addr, 0x08000000);
  assert_int_equal(out.has_addr, 1);
  assert_int_equal(out.data[0], 0xAA);

  free(out.data);
}

static void
test_srec_bad_checksum(void **state) {
  (void)state;
  char filename[512];
  parsed_file_t out;

  /* Bad checksum (should be 74, not 75) */
  const char *srec = "S1130000000102030405060708090A0B0C0D0E0F75\n"
                     "S9030000FC\n";

  snprintf(filename, sizeof(filename), "%s/bad.srec", temp_dir);
  write_file(filename, srec);

  assert_int_equal(srec_parse(filename, &out), -1);
}

static void
test_srec_no_end(void **state) {
  (void)state;
  char filename[512];
  parsed_file_t out;

  /* Missing end record */
  const char *srec = "S1130000000102030405060708090A0B0C0D0E0F74\n";

  snprintf(filename, sizeof(filename), "%s/noend.srec", temp_dir);
  write_file(filename, srec);

  assert_int_equal(srec_parse(filename, &out), -1);
}

/*
 * Real-world test vectors from web sources
 * Sources:
 *   Intel HEX: https://en.wikipedia.org/wiki/Intel_HEX
 *   S-record: https://manpages.ubuntu.com/manpages/focal/en/man5/srec.5.html
 */

static void
test_ihex_multirecord_wikipedia(void **state) {
  (void)state;
  char filename[512];
  parsed_file_t out;

  /* Wikipedia example: 4 data records at 0x0100-0x013F (64 bytes) */
  const char *ihex = ":10010000214601360121470136007EFE09D2190140\n"
                     ":100110002146017E17C20001FF5F16002148011928\n"
                     ":10012000194E79234623965778239EDA3F01B2CAA7\n"
                     ":100130003F0156702B5E712B722B732146013421C7\n"
                     ":00000001FF\n";

  snprintf(filename, sizeof(filename), "%s/wikipedia.hex", temp_dir);
  write_file(filename, ihex);

  assert_int_equal(ihex_parse(filename, &out), 0);
  assert_int_equal(out.size, 64);
  assert_int_equal(out.base_addr, 0x0100);
  assert_int_equal(out.has_addr, 1);
  /* First data byte: 0x21 */
  assert_int_equal(out.data[0], 0x21);
  /* Last data byte: 0xC7 at offset 63, which is record 4 checksum byte - no!
   * The last data byte before checksum in record 4 is 0x21 (at offset 0x3E) */
  assert_int_equal(out.data[63], 0x21);

  free(out.data);
}

static void
test_ihex_crlf_endings(void **state) {
  (void)state;
  char filename[512];
  parsed_file_t out;

  /* Windows-style CRLF line endings */
  const char *ihex = ":10000000000102030405060708090A0B0C0D0E0F78\r\n"
                     ":00000001FF\r\n";

  snprintf(filename, sizeof(filename), "%s/crlf.hex", temp_dir);
  write_file(filename, ihex);

  assert_int_equal(ihex_parse(filename, &out), 0);
  assert_int_equal(out.size, 16);
  assert_int_equal(out.base_addr, 0);

  free(out.data);
}

static void
test_ihex_lowercase(void **state) {
  (void)state;
  char filename[512];
  parsed_file_t out;

  /* Lower-case hex digits (valid per spec) */
  const char *ihex = ":10000000deadbeefcafebabe010203040506070854\n"
                     ":00000001ff\n";

  snprintf(filename, sizeof(filename), "%s/lower.hex", temp_dir);
  write_file(filename, ihex);

  assert_int_equal(ihex_parse(filename, &out), 0);
  assert_int_equal(out.size, 16);
  assert_int_equal(out.data[0], 0xDE);
  assert_int_equal(out.data[1], 0xAD);
  assert_int_equal(out.data[2], 0xBE);
  assert_int_equal(out.data[3], 0xEF);

  free(out.data);
}

static void
test_srec_multirecord_manpage(void **state) {
  (void)state;
  char filename[512];
  parsed_file_t out;

  /* Ubuntu man page example with S5 count record */
  const char *srec = "S00600004844521B\n"
                     "S1130000285F245F2212226A000424290008237C2A\n"
                     "S11300100002000800082629001853812341001813\n"
                     "S113002041E900084E42234300182342000824A952\n"
                     "S107003000144ED492\n"
                     "S5030004F8\n"
                     "S9030000FC\n";

  snprintf(filename, sizeof(filename), "%s/manpage.s19", temp_dir);
  write_file(filename, srec);

  assert_int_equal(srec_parse(filename, &out), 0);
  /* 16 + 16 + 16 + 4 = 52 bytes */
  assert_int_equal(out.size, 52);
  assert_int_equal(out.base_addr, 0);
  assert_int_equal(out.has_addr, 1);
  /* First data byte */
  assert_int_equal(out.data[0], 0x28);
  /* Last record starts at 0x30, has 4 bytes: 00 14 4E D4 */
  assert_int_equal(out.data[48], 0x00);
  assert_int_equal(out.data[49], 0x14);
  assert_int_equal(out.data[50], 0x4E);
  assert_int_equal(out.data[51], 0xD4);

  free(out.data);
}

static void
test_srec_crlf_endings(void **state) {
  (void)state;
  char filename[512];
  parsed_file_t out;

  /* Windows-style CRLF line endings */
  const char *srec = "S00600004844521B\r\n"
                     "S1130000000102030405060708090A0B0C0D0E0F74\r\n"
                     "S9030000FC\r\n";

  snprintf(filename, sizeof(filename), "%s/crlf.s19", temp_dir);
  write_file(filename, srec);

  assert_int_equal(srec_parse(filename, &out), 0);
  assert_int_equal(out.size, 16);
  assert_int_equal(out.base_addr, 0);

  free(out.data);
}

static void
test_srec_lowercase(void **state) {
  (void)state;
  char filename[512];
  parsed_file_t out;

  /* Lower-case hex digits (valid per spec) */
  const char *srec = "s00600004844521b\n"
                     "s1130000deadbeefcafebabe010203040506070850\n"
                     "s9030000fc\n";

  snprintf(filename, sizeof(filename), "%s/lower.s19", temp_dir);
  write_file(filename, srec);

  assert_int_equal(srec_parse(filename, &out), 0);
  assert_int_equal(out.size, 16);
  assert_int_equal(out.data[0], 0xDE);
  assert_int_equal(out.data[1], 0xAD);
  assert_int_equal(out.data[2], 0xBE);
  assert_int_equal(out.data[3], 0xEF);

  free(out.data);
}

/*
 * format_parse auto-detection tests
 */

static void
test_format_parse_auto_ihex(void **state) {
  (void)state;
  char filename[512];
  parsed_file_t out;

  const char *ihex = ":04000000AABBCCDDEE\n"
                     ":00000001FF\n";

  snprintf(filename, sizeof(filename), "%s/auto.hex", temp_dir);
  write_file(filename, ihex);

  assert_int_equal(format_parse(filename, FORMAT_AUTO, &out), 0);
  assert_int_equal(out.size, 4);
  assert_int_equal(out.has_addr, 1);

  free(out.data);
}

static void
test_format_parse_auto_srec(void **state) {
  (void)state;
  char filename[512];
  parsed_file_t out;

  const char *srec = "S1070000AABBCCDDEA\n"
                     "S9030000FC\n";

  snprintf(filename, sizeof(filename), "%s/auto.s19", temp_dir);
  write_file(filename, srec);

  assert_int_equal(format_parse(filename, FORMAT_AUTO, &out), 0);
  assert_int_equal(out.size, 4);
  assert_int_equal(out.has_addr, 1);

  free(out.data);
}

static void
test_format_parse_explicit(void **state) {
  (void)state;
  char filename[512];
  parsed_file_t out;

  /* File with .dat extension but Intel HEX content */
  const char *ihex = ":04000000AABBCCDDEE\n"
                     ":00000001FF\n";

  snprintf(filename, sizeof(filename), "%s/data.dat", temp_dir);
  write_file(filename, ihex);

  /* Auto would treat as binary, explicit IHEX parses correctly */
  assert_int_equal(format_parse(filename, FORMAT_IHEX, &out), 0);
  assert_int_equal(out.size, 4);
  assert_int_equal(out.has_addr, 1);

  free(out.data);
}

int
main(void) {
  const struct CMUnitTest tests[] = {
    /* Format detection tests */
    cmocka_unit_test(test_format_detect_bin),
    cmocka_unit_test(test_format_detect_ihex),
    cmocka_unit_test(test_format_detect_srec),
    cmocka_unit_test(test_format_name),

    /* Binary parser tests */
    cmocka_unit_test(test_bin_parse),

    /* Intel HEX parser tests */
    cmocka_unit_test(test_ihex_simple),
    cmocka_unit_test(test_ihex_extended_addr),
    cmocka_unit_test(test_ihex_segment_addr),
    cmocka_unit_test(test_ihex_bad_checksum),
    cmocka_unit_test(test_ihex_no_eof),

    /* Motorola S-record parser tests */
    cmocka_unit_test(test_srec_s19),
    cmocka_unit_test(test_srec_s2),
    cmocka_unit_test(test_srec_s3),
    cmocka_unit_test(test_srec_bad_checksum),
    cmocka_unit_test(test_srec_no_end),

    /* Real-world test vectors */
    cmocka_unit_test(test_ihex_multirecord_wikipedia),
    cmocka_unit_test(test_ihex_crlf_endings),
    cmocka_unit_test(test_ihex_lowercase),
    cmocka_unit_test(test_srec_multirecord_manpage),
    cmocka_unit_test(test_srec_crlf_endings),
    cmocka_unit_test(test_srec_lowercase),

    /* format_parse auto-detection tests */
    cmocka_unit_test(test_format_parse_auto_ihex),
    cmocka_unit_test(test_format_parse_auto_srec),
    cmocka_unit_test(test_format_parse_explicit),
  };

  return cmocka_run_group_tests(tests, setup, teardown);
}
