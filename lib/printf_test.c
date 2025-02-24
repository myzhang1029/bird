/*
 *	BIRD Library -- String Functions Tests
 *
 *	(c) 2015 CZ.NIC z.s.p.o.
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

#include "test/birdtest.h"

#include "lib/string.h"

#define BSPRINTF(nw, res, buf, fmt, ...) \
  do { \
    int n = bsprintf(buf, fmt, ##__VA_ARGS__); \
    bt_assert_msg(n == nw, "fmt=\"%s\" returns n=%d, want %d", fmt, n, nw); \
    bt_assert_msg(buf[n] == 0, "fmt=\"%s\" buf[%d] should be \'\\0\', found 0x%02x", fmt, n, buf[n]); \
    bt_assert_msg(memcmp(buf, res, nw) == 0, "fmt=\"%s\" writes \"%*s\", want \"%*s\"", fmt, (n < nw ? n : nw), buf, nw, res); \
  } while (0)

static int
t_simple(void)
{
  char buf[256];
  memset(buf, 0xa5, 256);

  BSPRINTF(0, "", buf, "", NULL);
  BSPRINTF(1, "%", buf, "%%", NULL);
  BSPRINTF(2, "%%", buf, "%%%%", NULL);

  BSPRINTF(1, "\x00", buf, "%c", 0);
  BSPRINTF(1, "@", buf, "@", 64);
  BSPRINTF(1, "\xff", buf, "%c", 0xff);

  const char *io_error_str = lp_strdup(tmp_linpool, strerror(EIO));
  const int io_error_len = strlen(io_error_str);

  errno = EIO;
  BSPRINTF(io_error_len, io_error_str, buf, "%m");
  errno = 0;

  BSPRINTF(io_error_len, io_error_str, buf, "%M", EIO);

  BSPRINTF(11, "TeSt%StRiNg", buf, "%s", "TeSt%StRiNg");

  if (sizeof(void *) == 4)
    BSPRINTF(8, "1a15600d", buf, "%p", (void *) 0x1a15600d);
  else
    BSPRINTF(16, "00000fee1a15600d", buf, "%p", (void *) 0xfee1a15600d);

  s64 ln = 0;
  BSPRINTF(10, "TeStStRiNg", buf, "TeStS%lntRiNg", &ln);
  bt_assert_msg(ln == 5, "fmt=\"TeStS%%lntRiNg\", &ln makes ln=%ld, want 5", ln);

  BSPRINTF(2, "%d", buf, "%%d", 1);
  BSPRINTF(1, "1", buf, "%d", 1);
  BSPRINTF(2, "+1", buf, "%+d", 1);
  BSPRINTF(2, " 1", buf, "% d", 1);
  BSPRINTF(2, "-1", buf, "%d", -1);
  BSPRINTF(11, "-2147483648", buf, "%d", INT32_MIN);
  BSPRINTF(10,  "2147483647", buf, "%d", INT32_MAX);

  BSPRINTF(1,  "0", buf, "%u", 0x0);
  BSPRINTF(10, "4294967295", buf, "%u", 0xFFFFFFFF);

  BSPRINTF(4,  "-100", buf, "%ld", (s64) -100);
  BSPRINTF(3,   "100", buf, "%ld", (s64)  100);
  BSPRINTF(20, "-9223372036854775808", buf, "%ld", INT64_MIN);
  BSPRINTF(19,  "9223372036854775807", buf, "%ld", INT64_MAX);

  BSPRINTF(3,  "0 8", buf, "%lu %lu", U64(0), U64(8));
  BSPRINTF(20, "18446744073709551615", buf, "%lu", UINT64_MAX);

  return 1;
}

static int
t_router_id(void)
{
  char buf[256];

  BSPRINTF(7, "1.2.3.4", buf, "%R", (u32) 0x01020304);
  BSPRINTF(15, "240.224.208.192", buf, "%R", (u32) 0xF0E0D0C0);
  BSPRINTF(23, "01:02:03:04:05:06:07:08", buf, "%lR", (u64) 0x0102030405060708);
  BSPRINTF(23, "f0:e0:d0:c0:b0:a0:90:80", buf, "%lR", (u64) 0xF0E0D0C0B0A09080);

  return 1;
}

static int
t_time(void)
{
  char buf[256];

  BSPRINTF(7, "123.456", buf, "%t", (btime) 123456789);
  BSPRINTF(7, "123.456", buf, "%2t", (btime) 123456789);
  BSPRINTF(8, " 123.456", buf, "%8t", (btime) 123456789);
  BSPRINTF(4, " 123", buf, "%4.0t", (btime) 123456789);
  BSPRINTF(8, "123.4567", buf, "%8.4t", (btime) 123456789);
  BSPRINTF(9, "0123.4567", buf, "%09.4t", (btime) 123456789);
  BSPRINTF(12, "  123.456789", buf, "%12.10t", (btime) 123456789);
  BSPRINTF(8, " 123.004", buf, "%8t", (btime) 123004 MS);

  return 1;
}

static int
t_bstrcmp(void)
{
  bt_assert(bstrcmp("aa", "aa") == 0);
  bt_assert(bstrcmp("aa", "bb") < 0);
  bt_assert(bstrcmp("bb", "aa") > 0);
  bt_assert(bstrcmp(NULL, NULL) == 0);
  bt_assert(bstrcmp(NULL, "bb") == -1);
  bt_assert(bstrcmp("bb", NULL) == 1);

  return 1;
}

static int
t_fmt_order(void)
{
  struct fmt_order_tv {
    u64 value;
    int decimals;
    u64 threshold;
    const char *expect;
  } test_vectors [] = {
    { 9999, 1, 10000, "9999  " },
    { 10001, 1, 10000, "9.8 k" },
    { 10001, 2, 10000, "9.77 k" },
    { 10001, 3, 10000, "9.767 k" },
    { 1048575, 0, 10000, "1024 k" },
    { 1048575, 1, 10000, "1024.0 k" },
    { 1048575, 2, 10000, "1024.00 k" },
    { 1048575, 3, 10000, "1023.999 k" },
    { 1048575, 4, 10000, "1023.9990 k" },
    { 1048575, 5, 10000, "1023.99902 k" },
    { 1048575, 6, 10000, "1023.999023 k" },
    { 1048575, 0, 1000, "1 M" },
    { 1048575, 1, 1000, "1.0 M" },
    { 1048575, 2, 1000, "1.00 M" },
    { 1048575, 3, 1000, "1.000 M" },
    { 1048575, 4, 1000, "1.0000 M" },
    { 1048575, 5, 1000, "1.00000 M" },
    { 1048575, 6, 1000, "0.999999 M" },
    { 1048577, 6, 10000, "1024.000977 k" },
    { 1048577, 6, 1000, "1.000001 M" },
    { 1048577, 6, 100, "1.000001 M" },
    { 1048577, 6, 10, "1.000001 M" },
    { 1048577, 6, 1, "1.000001 M" },
    { 10000000000000, 6, 10000, "9313.225746 G" },
    { 10000000000000, 6, 1000, "9.094947 T" },
    { 123456789123456789, 0, 1000, "110 P" },
    { 123456789123456789, 4, 1000, "109.6517 P" },
    { 123456789123456789, 7, 1000, "109.6516559 P" },
    { 1234567890123456789, 0, 1000, "1 E" },
    { 1234567890123456789, 1, 1000, "1.1 E" },
    { 1234567890123456789, 2, 1000, "1.07 E" },
    { 1234567890123456789, 3, 1000, "1.071 E" },
    { 1234567890123456789, 4, 1000, "1.0708 E" },
    { 1234567890123456789, 5, 1000, "1.07082 E" },
    { 1234567890123456789, 6, 1000, "1.070817 E" },
    { 1234567890123456789, 7, 1000, "1.0708170 E" },
    { 9444732965739290427U, 3, 1000, "8.192 E" },
    { 9444732965739290427U, 6, 1000, "8.192000 E" },
    { 18446744073709551615U, 2, 1000, "16.00 E" },
  };

  for (int i = 0; i < (int)ARRAY_SIZE(test_vectors); i++)
  {
    const char *result = fmt_order(test_vectors[i].value, test_vectors[i].decimals, test_vectors[i].threshold);
    const char *expect = test_vectors[i].expect;

    bt_assert_msg(strncmp(result, expect, strlen(expect)) == 0,
		  "case %d, result \"%s\", expect \"%s\"", i, result, expect);
  }

  return 1;
}

int
main(int argc, char *argv[])
{
  bt_init(argc, argv);

  bt_test_suite(t_simple, "printf without varargs");
  bt_test_suite(t_router_id, "print router id");
  bt_test_suite(t_time, "print time");
  bt_test_suite(t_bstrcmp, "bstrcmp");
  bt_test_suite(t_fmt_order, "fmt_order");

  return bt_exit_value();
}
