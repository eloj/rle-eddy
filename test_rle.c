/*
	RLE Zoo Encode & Decode Tests
	Copyright (c) 2022, Eddy L O Jansson. Licensed under The MIT License.

	See https://github.com/eloj/rle-zoo
*/
#define _GNU_SOURCE

#define UTILITY_IMPLEMENTATION
#include "utility.h"

#define RLE_ZOO_GOLDBOX_IMPLEMENTATION
#include "rle_goldbox.h"
#define RLE_ZOO_PACKBITS_IMPLEMENTATION
#include "rle_packbits.h"
#define RLE_ZOO_PCX_IMPLEMENTATION
#include "rle_pcx.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <ctype.h>
#include <sys/mman.h>

#define RED "\e[1;31m"
#define GREEN "\e[0;32m"
#define YELLOW "\e[1;33m"
#define NC "\e[0m"

static int debug = 1; // Output debug hex dumps for failed tests.
static int hex_always = 0;
static int hex_show_offset = 1;
static int flag_roundtrip = 1;

static int num_roundtrip = 0;

struct test {
	uint8_t *input;
	size_t len;
	char *actions;
	size_t expected_size;
	uint32_t expected_hash;	// CRC32c for now
};

// TODO: Variant selection code + tables can be shared.
typedef size_t (*rle_fp)(const uint8_t *src, size_t slen, uint8_t *dest, size_t dlen);

struct rle_t {
	const char *name;
	rle_fp compress;
	rle_fp decompress;
} rle_variants[] = {
	{
		.name = "goldbox",
		.compress = goldbox_compress,
		.decompress = goldbox_decompress
	},
	{
		.name = "packbits",
		.compress = packbits_compress,
		.decompress = packbits_decompress
	},
	{
		.name = "pcx",
		.compress = pcx_compress,
		.decompress = pcx_decompress
	},
};

static const size_t NUM_VARIANTS = sizeof(rle_variants)/sizeof(rle_variants[0]);

static struct rle_t* get_rle_by_name(const char *name) {
	for (size_t i = 0 ; i < NUM_VARIANTS ; ++i) {
		if (strcmp(name, rle_variants[i].name) == 0) {
			return &rle_variants[i];
		}
	}
	return NULL;
}

/*
	TODO: Should use some other digest with a simple plain-c implementation.
*/
__attribute__ ((target ("sse4.2")))
static uint32_t crc32c(uint32_t crc, const void *data, size_t len) {
	const uint8_t *src = data;

	for (size_t i=0 ; i < len ; ++i) {
		crc = __builtin_ia32_crc32qi(crc, *src++);
	}

	return crc;
}


#define TEST_ERRMSG(fmt, ...) \
	fprintf(stderr, "%s:%zu:" RED " error: " NC fmt "\n", filename, line_no __VA_OPT__(,) __VA_ARGS__)
#define TEST_WARNMSG(fmt, ...) \
	fprintf(stderr, "%s:%zu:" YELLOW " warning: " NC fmt "\n", filename, line_no __VA_OPT__(,) __VA_ARGS__)

// This either compress or decompress the output of a test to check it against the original input.
static int roundtrip(struct rle_t *rle, struct test *te, uint8_t *inbuf, size_t inbuf_len, int compress) {
	rle_fp rle_func = compress ? rle->compress : rle->decompress;

	uint8_t *tmp_buf = malloc(te->len);

	size_t res = rle_func(inbuf, inbuf_len, tmp_buf, te->len);

	int cmp = memcmp(tmp_buf, te->input, te->len);
	if (cmp != 0) {
		printf("expected from %scompressed test input:\n", compress ? "" : "de");
		fprint_hex(stdout, te->input, te->len, 32, "\n", hex_show_offset);
		fflush(stdout);
		printf("\n");
		printf("got:\n");
		fprint_hex(stdout, tmp_buf, res, 32, "\n", hex_show_offset);
		fflush(stdout);
		printf("\n");
	} else {
		cmp = res != te->len;
	}

	free(tmp_buf);

	++num_roundtrip;

	return cmp;
}

static int run_rle_test(struct rle_t *rle, struct test *te, const char *filename, size_t line_no) {
	// printf("INPUT:%.*s (%zu bytes)\n", (int)te->len, te->input, te->len);

	// Take the max of the input and expected sizes as base estimate for temporary buffer.
	size_t tmp_size = te->len;
	if (te->expected_size > tmp_size)
		tmp_size = te->expected_size;
	tmp_size *= 4;
	assert(tmp_size < 1L << 24);
	uint8_t *tmp_buf = malloc(tmp_size);

	uint32_t input_hash = crc32c((uint32_t)~0, te->input, te->len) ^ (uint32_t)~0;

	int retval = 0;

	char *action = te->actions;
	int no_roundtrip = strchr(action, '-') != NULL;

	// COMPRESS
	if (*action == 'c') {
		// First do a length-determination check on the input.
		size_t len_check = rle->compress(te->input, te->len, NULL, 0);
		if (len_check != te->expected_size) {
			TEST_ERRMSG("expected compressed size %zu, got %zu.", te->expected_size, len_check);
			retval = 1;
		}

		// Next compress the input into the oversized buffer, and verify length remains the same.
		assert(len_check <= tmp_size);
		size_t res = rle->compress(te->input, te->len, tmp_buf, tmp_size);
		if (res != len_check) {
			TEST_ERRMSG("compressed output length differs from determined value %zu, got %zu.", len_check, res);
			retval = 1;
		}

		uint32_t res_hash = crc32c((uint32_t)~0, tmp_buf, res) ^ (uint32_t)~0;

		// Now decompress with the output byte-tight, to check for dest range-check errors.
		size_t res_tight = rle->compress(te->input, te->len, tmp_buf, len_check);
		if (res_tight != len_check) {
			TEST_ERRMSG("compressed output length for tight buffer differs from determined value %zu, got %zu.", len_check, res_tight);
			retval = 1;
		}

		uint32_t res_tight_hash = crc32c((uint32_t)~0, tmp_buf, res_tight) ^ (uint32_t)~0;
		if (res_hash != te->expected_hash) {
			TEST_ERRMSG("expected compressed hash 0x%08x, got 0x%08x.", te->expected_hash, res_hash);
			retval = 1;
		}

		// Verify there's no content diff between the oversized output buffer and the tight one.
		if (res_tight_hash != res_hash) {
			TEST_ERRMSG("compressed hash mismatch; 0x%08x vs 0x%08x.", res_tight_hash, res_hash);
			retval = 1;
		}

		if (flag_roundtrip && !no_roundtrip && roundtrip(rle, te, tmp_buf, te->expected_size, 0) != 0) {
			TEST_ERRMSG("re-decompressed data does not match original input!");
			retval = 1;
		}

		if ((debug && retval != 0) || hex_always) {
			fprint_hex(stdout, tmp_buf, res, 32, "\n", hex_show_offset); printf("\n");
			fflush(stdout);
		}
	}
	// DECOMPRESS
	if (*action == 'd') {
		// First do a length-determination check on the input.
		size_t len_check = rle->decompress(te->input, te->len, NULL, 0);
		if (len_check != te->expected_size) {
			TEST_ERRMSG("expected decompressed size %zu, got %zu.", te->expected_size, len_check);
			retval = 1;
		}

		// Next decompress the input into the oversized buffer, and verify length remains the same.
		assert(len_check <= tmp_size);
		size_t res = rle->decompress(te->input, te->len, tmp_buf, tmp_size);
		if (res != len_check) {
			TEST_ERRMSG("decompressed output length differs from determined value %zu, got %zu.", len_check, res);
			retval = 1;
		}

		uint32_t res_hash = crc32c((uint32_t)~0, tmp_buf, res) ^ (uint32_t)~0;

		// Now decompress with the output byte-tight, to check for dest range-check errors.
		size_t res_tight = rle->decompress(te->input, te->len, tmp_buf, len_check);
		if (res_tight != len_check) {
			TEST_ERRMSG("decompressed output length for tight buffer differs from determined value %zu, got %zu.", len_check, res_tight);
			retval = 1;
		}

		uint32_t res_tight_hash = crc32c((uint32_t)~0, tmp_buf, res_tight) ^ (uint32_t)~0;
		if (res_tight_hash != te->expected_hash) {
			TEST_ERRMSG("expected decompressed hash 0x%08x, got 0x%08x.", te->expected_hash, res_hash);
			retval = 1;
		}

		// Verify there's no content diff between the oversized output buffer and the tight one.
		if (res_tight_hash != res_hash) {
			TEST_ERRMSG("decompressed hash mismatch; 0x%08x vs 0x%08x.", res_tight_hash, res_hash);
			retval = 1;
		}

		if (flag_roundtrip && !no_roundtrip && roundtrip(rle, te, tmp_buf, te->expected_size, 1) != 0) {
			TEST_ERRMSG("re-compressed data does not match original input!");
			retval = 1;
		}

		if ((debug && retval != 0) || hex_always) {
			fprint_hex(stdout, tmp_buf, res, 32, "\n", hex_show_offset); printf("\n");
			fflush(stdout);
		}
	}

	free(tmp_buf);

	return retval;
}

static int map_file(const char *filename, void **data, size_t *size) {
	FILE *f = fopen(filename, "rb");
	if (!f) {
		return 1;
	}

	int res = fseek(f, 0, SEEK_END);
	if (res != 0) {
		errx(1, "System has broken fseek() -- guess we should have used fstat instead, huh.");
	}
	size_t len = ftell(f);
	fseek(f, 0, SEEK_SET);

	void *base = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fileno(f), 0);
	if (base == MAP_FAILED) {
		fclose(f);
		return 2;
	}
	fclose(f);

	*data = base;
	*size = len;

	return 0;
}

int main(int argc, char *argv[]) {
	const char *filename = argc > 1 ? argv[1] : "rle-tests.suite";

	FILE *f = fopen(filename, "rb");
	if (!f) {
		fprintf(stderr, "ERROR: Could not open input file '%s': %m\n", filename);
		exit(1);
	}

	char *line = NULL;
	size_t failed_tests = 0;
	size_t line_len = 0;
	size_t line_no = 0;
	ssize_t nread;
	while ((nread = getline(&line, &line_len, f)) != -1) {
		++line_no;
		if (line_len < 3 || line[0] == '#' || line[0] == ';') {
			continue;
		}
		if (strncmp(line, "---", 3) == 0) {
			TEST_WARNMSG("end-marker hit");
			break;
		}
		// Parse input line
		// goldbox c "AAAAAAAAAAAAAAAA" 2 0xhash
		char *method = NULL;
		char *input = NULL;
		struct test te = {};
		int exsize = 0;
		unsigned int exhash = 0;

		// TODO: Parsing the hex this way is bad, e.g adding a hex digit up front still pass.
		int parsed = sscanf(line, "%ms %ms %ms %i %x", &method, &te.actions, &input, &exsize, &exhash);
		if (parsed >= 3) {
			printf("<< %s", line);
			struct rle_t * rle = get_rle_by_name(method);
			if (rle) {
				if (exsize < 0) {
					TEST_WARNMSG("invalid expected size '%d'", exsize);
					goto nexttest;
				}

				te.expected_size = exsize;
				te.expected_hash = exhash;

				if (input[0] == '@') {
					// Read input from file.
					void *raw = NULL;
					size_t raw_len = 0;
					if (map_file(input + 1, &raw, &raw_len) != 0) {
						TEST_WARNMSG("file error reading '%s': %m", input+1);
						goto nexttest;
					} else {
						te.len = raw_len;
						te.input = malloc(te.len);
						memcpy(te.input, raw, te.len);
						munmap(raw, raw_len);
					}
				} else if (input[0] == '"') {
					int err;
					te.len = expand_escapes(input + 1, strlen(input + 1) - 1, NULL, 0, &err);
					if (err == 0) {
						// NOTE: I intentionally malloc the data, to give valgrind the best chance to detect OOB reads.
						te.input = malloc(te.len);
						te.len = expand_escapes(input + 1, strlen(input + 1) - 1, (char*)te.input, te.len, &err);
						assert(err == 0);
					} else {
						TEST_WARNMSG("invalid escape sequence at position %zu, err %d\n", te.len, err);
						goto nexttest;
					}

				} else {
					TEST_WARNMSG("invalid input format");
					goto nexttest;
				}
				if (run_rle_test(rle, &te, filename, line_no) != 0) {
					++failed_tests;
				}
			} else {
				TEST_WARNMSG("unknown method '%s'", method);
			}

nexttest:
			free(te.input);
			free(te.actions);
		}
		free(input);
		free(method);
	}
	free(line);
	fclose(f);

	if (flag_roundtrip == 0) {
		printf(YELLOW "Warning: Roundtripping disabled -- test coverage decreased!" NC "\n");
	}

	if (failed_tests == 0) {
		printf(GREEN "All tests of '%s' passed. (incl. %d roundtrip checks)" NC "\n", filename, num_roundtrip);
	} else {
		fprintf(stderr, RED "%zu test failures in suite '%s'." NC "\n", failed_tests, filename);
		exit(1);
	}

	return 0;
}
