/*
	RLE Zoo fuzzing driver for use with afl-fuzz.
*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>

#define RLE_ZOO_GOLDBOX_IMPLEMENTATION
#include "rle_goldbox.h"
#define RLE_ZOO_PACKBITS_IMPLEMENTATION
#include "rle_packbits.h"
#define RLE_ZOO_PCX_IMPLEMENTATION
#include "rle_pcx.h"

/* this lets the source compile without afl-clang-fast/lto */
#ifndef __AFL_FUZZ_TESTCASE_LEN

ssize_t       fuzz_len;
unsigned char fuzz_buf[1024000];

#define __AFL_FUZZ_TESTCASE_LEN fuzz_len
#define __AFL_FUZZ_TESTCASE_BUF fuzz_buf
#define __AFL_FUZZ_INIT() void sync(void);
#define __AFL_LOOP(x) \
	((fuzz_len = read(0, fuzz_buf, sizeof(fuzz_buf))) > 0 ? 1 : 0)
#define __AFL_INIT() sync()
#endif

__AFL_FUZZ_INIT();

#pragma clang optimize off
#pragma GCC optimize("O0")

int main() {
	uint8_t dest[1024];

#ifdef __AFL_HAVE_MANUAL_CONTROL
	__AFL_INIT();
#endif

	uint8_t *input = __AFL_FUZZ_TESTCASE_BUF;
	ssize_t resc = 0;
	ssize_t resd = 0;

	while (__AFL_LOOP(5000)) {
		size_t len = __AFL_FUZZ_TESTCASE_LEN;

		resc = goldbox_compress(input, len, dest, sizeof(dest));
		resd = goldbox_decompress(input, len, dest, sizeof(dest));

		resc += packbits_compress(input, len, dest, sizeof(dest));
		resd += packbits_decompress(input, len, dest, sizeof(dest));

		resc += pcx_compress(input, len, dest, sizeof(dest));
		resd += pcx_decompress(input, len, dest, sizeof(dest));
	}
	printf("resc=%zd, resd=%zd\n", resc, resd);
	return 0;
}
