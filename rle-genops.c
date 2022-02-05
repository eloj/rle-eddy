/*
	Run-Length Encoding & Decoding Table Generator
	Copyright (c) 2022, Eddy L O Jansson. Licensed under The MIT License.

	See https://github.com/eloj/rle-zoo

	TODO:
		Add flag field to indicate use of RLE_REP, RLE_CPY, RLE_LIT...
		Use bitmap to mark off used RLE_OPS, then bit ops can detect
			missing or ambigious encodings.
		Output necessary types or include-file when --genc

*/
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <limits.h>

typedef struct rle8 (*rle8_decode_fp)(uint8_t input);
typedef struct rle8 (*rle8_encode_fp)(struct rle8 cmd);

static int opt_usage, opt_genc;

static const char *gen_header = "// Generated by rle-genops from https://github.com/eloj/rle-zoo\n";

struct rle_parser {
	const char *name;
	rle8_encode_fp rle8_encode;
	rle8_decode_fp rle8_decode;
};

// NOTE: The order and value of these matter.
enum RLE_OP {
	RLE_OP_CPY,
	RLE_OP_REP,
	RLE_OP_LIT,
	RLE_OP_NOP,
	RLE_OP_INVALID,
};

struct rle8 {
	enum RLE_OP op;
	uint8_t cnt; // TODO: rename to 'arg'?
};

static const char *rle_op_cstr(enum RLE_OP op) {
	const char *res = "UNKNOWN";
	switch (op) {
		case RLE_OP_CPY:
			res = "CPY";
			break;
		case RLE_OP_REP:
			res = "REP";
			break;
		case RLE_OP_LIT:
			res = "LIT";
			break;
		case RLE_OP_NOP:
			res = "NOP";
			break;
		case RLE_OP_INVALID:
			res = "INVALID";
			break;
	}
	return res;
}

static struct rle8 rle8_decode_packbits(uint8_t input) {
	struct rle8 cmd;

	if (input > 0x80) {
		cmd.op = RLE_OP_REP;
		cmd.cnt = 1 - (int8_t)input;
	} else if (input < 0x80) {
		cmd.op = RLE_OP_CPY;
		cmd.cnt = input + 1;
	} else if (input == 0x80) {
		cmd.op = RLE_OP_NOP;
		cmd.cnt = 1;
	} else {
		// Impossible -- all decodings are valid
		cmd.op = RLE_OP_INVALID;
		cmd.cnt = 0;
	}

	assert((cmd.op != RLE_OP_REP) || (cmd.cnt <= 128 || cmd.cnt >= 2));
	assert((cmd.op != RLE_OP_CPY) || (cmd.cnt <= 128 || cmd.cnt >= 1));

	return cmd;
}

static struct rle8 rle8_encode_packbits(struct rle8 cmd) {
	struct rle8 res = { RLE_OP_INVALID, 0 };

	if (cmd.op == RLE_OP_REP) {
		if (cmd.cnt >= 2 && cmd.cnt <= 128) {
			res.op = RLE_OP_REP;
			res.cnt = 257 - cmd.cnt; // 1 - (int8_t)cmd.cnt;
		}
	} else if (cmd.op == RLE_OP_CPY) {
		if (cmd.cnt >= 1 && cmd.cnt <= 128) {
			res.op = RLE_OP_CPY;
			res.cnt = cmd.cnt - 1;
		}
	} else if (cmd.op == RLE_OP_NOP) {
		res.cnt = 0x80;
	}

	return res;
}

static struct rle8 rle8_decode_goldbox(uint8_t input) {
	struct rle8 cmd;

	if (input > 0x80) {
		cmd.op = RLE_OP_REP;
		cmd.cnt = (~input) + 1;
	} else if (input < 0x7e) {
		cmd.op = RLE_OP_CPY;
		cmd.cnt = input + 1;
	} else {
		cmd.op = RLE_OP_INVALID;
		cmd.cnt = 0;
	}

	assert((cmd.op != RLE_OP_REP) || (cmd.cnt <= 127 || cmd.cnt > 0));
	assert((cmd.op != RLE_OP_CPY) || (cmd.cnt <= 126 || cmd.cnt > 0));

	return cmd;
}

static struct rle8 rle8_encode_goldbox(struct rle8 cmd) {
	struct rle8 res = { RLE_OP_INVALID, 0 };

	if (cmd.op == RLE_OP_REP) {
		if (cmd.cnt >= 1 && cmd.cnt <= 127) {
			res.op = RLE_OP_REP;
			res.cnt = 1 + (~cmd.cnt);
		}
	} else if (cmd.op == RLE_OP_CPY) {
		if (cmd.cnt >= 1 && cmd.cnt <= 126) {
			res.op = RLE_OP_CPY;
			res.cnt = cmd.cnt - 1;
		}
	} // else if (cmd.op == RLE_OP_INVALID) res.cnt = 0x80; // XXX: Not sure why we'd want this. Add it as NOP in that case.

	return res;
}

static struct rle8 rle8_decode_pcx(uint8_t input) {
	struct rle8 cmd;

	if ((input & 0xC0) == 0xC0) {
		cmd.op = RLE_OP_REP;
		cmd.cnt = input & 0x3F;
	} else {
		cmd.op = RLE_OP_LIT;
		cmd.cnt = input;
	}

	assert((cmd.op != RLE_OP_REP) || (cmd.cnt <= 191 || cmd.cnt >= 0));
	assert((cmd.op != RLE_OP_LIT) || (cmd.cnt <= 63  || cmd.cnt >= 0));

	return cmd;
}

static struct rle8 rle8_encode_pcx(struct rle8 cmd) {
	struct rle8 res = { RLE_OP_INVALID, 0 };

	if (cmd.op == RLE_OP_REP) {
		if (cmd.cnt <= 63) {
			res.op = RLE_OP_REP;
			res.cnt = 0xC0 | cmd.cnt;
		}
	} else if (cmd.op == RLE_OP_LIT) {
		if (cmd.cnt <= 191) {
			res.op = RLE_OP_LIT;
			res.cnt = cmd.cnt;
		}
	}

	return res;
}

static int rle8_display_ops(struct rle_parser *p) {
	printf("// Automatically generated code table for RLE8 variant '%s'\n", p->name);
	printf("%s", gen_header);
	for (int i=0 ; i < 256 ; ++i) {
		uint8_t b = i;

		struct rle8 cmd = p->rle8_decode(b);

		if (cmd.op != RLE_OP_INVALID) {
			struct rle8 b_recode = p->rle8_encode(cmd);
			printf("0x%02x (%d/%d) => %s %d\n", b, b, (int8_t)b, rle_op_cstr(cmd.op), cmd.cnt);

			if (b != b_recode.cnt) {
				printf("ERROR: reencode mismatch: %s %d => 0x%02x\n", rle_op_cstr(cmd.op), cmd.cnt, b_recode.cnt);
				return 1;
			}
		} else {
			printf("0x%02x (%d/%d) => %s\n", b, b, (int8_t)b, rle_op_cstr(cmd.op));
		}
	}
	return 0;
}

static void rle8_generate_decode_table(struct rle_parser *p) {
	printf("\n// Decode table for RLE8 variant '%s'\n", p->name);

	printf("static struct rle8 rle8_tbl_decode_%s[256] = {\n", p->name);

	for (int i=0 ; i < 256 ; ++i) {
		uint8_t b = i;
		struct rle8 cmd = p->rle8_decode(b);
		printf(" /* %02X */ { RLE_OP_%s, %3d }", b, rle_op_cstr(cmd.op), cmd.cnt);
		if (i < 255) printf(",");
		if ((i < 255) && ((i+1) % 4) == 0) printf("\n");
	}

	printf("\n};\n");
}

static void rle8_generate_encode_tables(struct rle_parser *p) {
	printf("\n// Encode tables for RLE8 variant '%s'\n", p->name);

	// TODO: This should be autodetected by max of valid CPY/REP/LIT encoding, rest padded to -1
	int max_len = 256;
	printf("static int16_t rle8_tbl_encode_%s[][%d] = {\n", p->name, max_len);

	printf("\t// RLE_OP_CPY 0..%d\n", max_len - 1);
	printf("\t{ ");
	for (int i=0 ; i < max_len ; ++i) {
		if (i > 0) printf(", ");
		struct rle8 cmd = { RLE_OP_CPY, i };
		struct rle8 code = p->rle8_encode(cmd);
		if (code.op != RLE_OP_INVALID)
			printf("0x%02x", code.cnt);
		else
			printf("-1");
	}
	printf(" },\n");

	printf("\t// RLE_OP_REP 0..%d\n", max_len - 1);
	printf("\t{ ");
	for (int i=0 ; i < max_len ; ++i) {
		if (i > 0) printf(", ");
		struct rle8 cmd = { RLE_OP_REP, i };
		struct rle8 code = p->rle8_encode(cmd);
		if (code.op != RLE_OP_INVALID)
			printf("0x%02x", code.cnt);
		else
			printf("-1");
	}
	printf(" },\n");

	printf("\t// RLE_OP_LIT 0..%d\n", max_len - 1);
	printf("\t{ ");
	for (int i=0 ; i < max_len ; ++i) {
		if (i > 0) printf(", ");
		struct rle8 cmd = { RLE_OP_LIT, i };
		struct rle8 code = p->rle8_encode(cmd);
		if (code.op != RLE_OP_INVALID)
			printf("0x%02x", code.cnt);
		else
			printf("-1");
	}
	printf(" },");

	printf("\n};\n");
}

static void rle8_generate_table(struct rle_parser *p) {
	// TODO: This should be autodetected by max of valid CPY/REP/LIT encoding, rest padded to -1
	int max_len = 256;

	int minmax[3][2] = { { INT_MAX, INT_MIN }, { INT_MAX, INT_MIN }, { INT_MAX, INT_MIN } };
	int op_usage[RLE_OP_INVALID + 1] = { 0 };

	// Determine REP and CPY limits:
	for (int i=0 ; i < 256 ; ++i) {
		uint8_t b = i;
		struct rle8 cmd = p->rle8_decode(b);
		switch (cmd.op) {
			case RLE_OP_CPY:
			case RLE_OP_REP:
			case RLE_OP_LIT:
				if (cmd.cnt < minmax[cmd.op][0])
					minmax[cmd.op][0] = cmd.cnt;
				if (cmd.cnt > minmax[cmd.op][1])
					minmax[cmd.op][1] = cmd.cnt;
				break;
			case RLE_OP_NOP:
			case RLE_OP_INVALID:
				/* NOP */
				break;
		}
		op_usage[cmd.op]++;
	}

	char buf[1024];
	int wp = 0;
	for (int i = RLE_OP_CPY ; i < RLE_OP_INVALID ; ++i) {
		if (op_usage[i] > 0) {
			wp += snprintf(buf + wp, sizeof(buf) - 1 - wp, "%sRLE_OP_%s /* %d */", wp == 0 ? "" : " | ", rle_op_cstr(i), op_usage[i]);
		}
	}

	printf("\nstatic struct rle8_tbl rle8_table_%s = {\n", p->name);
	printf("\t\"%s\",\n", p->name);
	printf("\t%s,\n", buf); // enum RLE_OP op_used;
	printf("\t%d,\n", max_len);
	printf("\t{\n");
	printf("\t\t&rle8_tbl_encode_%s[RLE_OP_CPY][0],\n", p->name);
	printf("\t\t&rle8_tbl_encode_%s[RLE_OP_REP][0],\n", p->name);
	printf("\t\t&rle8_tbl_encode_%s[RLE_OP_LIT][0],\n", p->name);
	printf("\t},\n");
	printf("\trle8_tbl_decode_%s,\n", p->name);
	printf("\t{\n");
	printf("\t\t{ %d, %d },\n", minmax[RLE_OP_CPY][0], minmax[RLE_OP_CPY][1]);
	printf("\t\t{ %d, %d },\n", minmax[RLE_OP_REP][0], minmax[RLE_OP_REP][1]);
	printf("\t\t{ %d, %d },\n", minmax[RLE_OP_LIT][0], minmax[RLE_OP_LIT][1]);
	printf("\t}\n};\n");

};

static void rle8_generate_c_tables(struct rle_parser *p) {
	printf("%s", gen_header);

	rle8_generate_decode_table(p);
	rle8_generate_encode_tables(p);
	rle8_generate_table(p);
}

struct rle_parser parsers[] = {
	{
		"goldbox",
		rle8_encode_goldbox,
		rle8_decode_goldbox
	},
	{
		"packbits",
		rle8_encode_packbits,
		rle8_decode_packbits
	},
	{
		"pcx",
		rle8_encode_pcx,
		rle8_decode_pcx
	}
};
static const size_t NUM_VARIANTS = sizeof(parsers)/sizeof(parsers[0]);

static struct rle_parser* get_parser_by_name(const char *name) {
	for (size_t i = 0 ; i < NUM_VARIANTS ; ++i) {
		if (strcmp(name, parsers[i].name) == 0) {
			return &parsers[i];
		}
	}
	return NULL;
}

static void print_variants(void) {
	printf("\nAvailable variants:\n");
	struct rle_parser *p = parsers;
	for (size_t i = 0 ; i < NUM_VARIANTS ; ++i) {
		printf("  %s\n", p->name);
		++p;
	}
}

static void usage(const char *argv) {
	printf("%s [OPTION] <variant>\n\n", argv);

	printf("Options:\n");
	printf("  --genc - Generate C tables.\n");

	print_variants();
}

static int parse_args(int *argc, char ***argv) {
	const char *arg = NULL;
	char **l_argv = *argv + 1;
	int l_argc = *argc - 1;
	int retval = 0;

	while ((arg = *l_argv) != NULL) {
		// "argv[argc] shall be a null pointer", section 5.1.2.2.1
		// const char *value = *l_argv;

		if (arg[0] == '-') {
			++l_argv;
			--l_argc;
			++arg;

			if (arg[0] == '-') {
				// long option
				++arg;
				if (strcmp(arg, "help") == 0) {
					opt_usage = 1;
				} else if (strcmp(arg, "genc") == 0) {
					opt_genc = 1;
				} else {
					retval = 1;
					break;
				}
			} else {
				// short option
				if (strcmp(arg, "h") == 0) {
					opt_usage = 1;
				} else {
					retval = 1;
					break;
				}
			}
		} else {
			break;
		}
	}

	if (retval != 0) {
		fprintf(stderr, "%s: unrecognized option: --%s\n", *argv[0], arg);
		fprintf(stderr, "Try '%s --help' for more information.\n", *argv[0]);
	}

	*argc = l_argc;
	*argv = l_argv;

	return retval;
}

int main(int argc, char *argv[]) {
	char **org_argv = argv;

	if (parse_args(&argc, &argv) != 0) {
		return 1;
	}

	if (opt_usage) {
		usage(org_argv[0]);
		return 1;
	}

	char *variant = argv[0];
	struct rle_parser *p = argc > 0 ? get_parser_by_name(variant) : NULL;

	if (!p) {
		if (variant) {
			fprintf(stderr, "error: Unknown variant '%s'\n", variant);
			print_variants();
		} else {
			usage(org_argv[0]);
		}
		return 2;
	}

	if (opt_genc)
		rle8_generate_c_tables(p);
	else
		rle8_display_ops(p);

	return 0;
}
