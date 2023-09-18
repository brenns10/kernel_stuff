#define _GNU_SOURCE
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/version.h>

#include <libkdumpfile/kdumpfile.h>
#include "libcore.h"

struct sym {
	char *symbol;
	uint64_t addr;
};

struct kallsyms {
	struct sym *symbols;
	size_t *name_index;
	size_t count;
	char *strtab;
};

struct kcore {
	struct kallsyms ks;
	int core_fd;
	kdump_ctx_t *kdump_ctx;
	uint32_t kernel_version;

	/* Error info */
	kcore_status_t last_err;
	union {
		int kcore_errno;
		kdump_status kcore_kdump_status;
		char *kcore_formatted;
	};
};

// Forward declare for kallsyms
static kcore_status_t kcore_sym_lookup_index(kcore_t *kcore, const char *name, size_t *index);

//////////////////////////////////
// Errors

static const char *kcore_error_messages[] = {
	[KCORE_OK] = "Ok",
	[KCORE_ERR_CTF] = "libctf error",
	[KCORE_ERR_LIBKDUMPFILE] = "libkdumpfile error",
	[KCORE_ERR_OS] = "os error",
	[KCORE_NOT_FOUND] = "element not found",
	[KCORE_ALREADY_INITIALIZED] = "the kcore handle is already intialized",
	[KCORE_NOT_IMPLEMENTED] = "functionality is not yet implemented",
	[KCORE_MEMORY] = "memory allocation error",
	[KCORE_FMT] = "formatted error message (see kcore_fail)",
};

const char *kcore_errmsg(kcore_status_t status) {
	if (status >= KCORE_OK && status < _KCORE_MAX)
		return kcore_error_messages[status];
	return NULL;
}

static kcore_status_t set_err(kcore_t *ctx, kcore_status_t st)
{
	ctx->last_err = st;
	return st;
}

static kcore_status_t set_os_err(kcore_t *ctx)
{
	ctx->kcore_errno = errno;
	return set_err(ctx, KCORE_ERR_OS);
}

static kcore_status_t set_kdumpfile_err(kcore_t *ctx, kdump_status ks)
{
	ctx->kcore_kdump_status = ks;
	return set_err(ctx, KCORE_ERR_LIBKDUMPFILE);
}

kcore_status_t set_err_fmt(kcore_t *ctx, const char *fmt, ...)
{
	char *result = NULL;
	va_list vl;
	va_start(vl, fmt);
	vasprintf(&result, fmt, vl);
	va_end(vl);
	if (result) {
		ctx->kcore_formatted = result;
		return set_err(ctx, KCORE_FMT);
	} else {
		return set_err(ctx, KCORE_MEMORY);
	}
}

void kcore_fail(kcore_t *ctx, const char *fmt, ...)
{
	va_list vl;
	va_start(vl, fmt);
	vfprintf(stderr, fmt, vl);
	va_end(vl);
	if (ctx->last_err == KCORE_ERR_OS) {
		fprintf(stderr, ": kcore OS error: %s\n", strerror(ctx->kcore_errno));
	} else if (ctx->last_err == KCORE_ERR_LIBKDUMPFILE) {
		fprintf(stderr, ": kdumpfile error: %s\n", kdump_strerror(ctx->kcore_kdump_status));
	} else {
		fprintf(stderr, ": %s\n", kcore_errmsg(ctx->last_err));
	}
	exit(EXIT_FAILURE);
}

//////////////////////////////////
// Symbols

static kcore_status_t read_proc_kallsyms(kcore_t *ctx, struct kallsyms *ks)
{
	char *line = NULL;
	size_t line_size = 0;
	ssize_t res;
	size_t line_number = 1;
	size_t alloc = 0;
	kcore_status_t st;
	size_t sballoc = 0, sbsize = 0;
	FILE *fp = fopen("/proc/kallsyms", "r");
	if (!fp)
		return set_os_err(ctx);

	memset(ks, 0, sizeof(*ks));

	while ((res = getline(&line, &line_size, fp)) != -1) {
		char *save = NULL;
		char *name, *addr_str, *type_str, *mod, *addr_rem;
		char type;
		uint64_t addr;
		size_t name_len;

		addr_str = strtok_r(line, " \t\r\n", &save);
		type_str = strtok_r(NULL,"  \t\r\n", &save);
		name = strtok_r(NULL,"  \t\r\n", &save);
		mod = strtok_r(NULL,"  \t\r\n", &save);

		if (!addr_str || !type_str || !name) {
			st = set_err_fmt(ctx, "error parsing /proc/kallsyms line %zu", line_number);
			goto err;
		}
		if (mod)
			break;
		type = *type_str;
		addr = strtoul(addr_str, &addr_rem, 16);
		if (*addr_rem) {
			/* addr_rem should be set to the first un-parsed character, and
			 * since the entire string should be a valid base 16 integer,
			 * we expect it to be \0 */
			st = set_err_fmt(ctx, "Invalid address \"%s\" in kallsyms line %zu",
					 addr_str, line_number);
			goto err;
		}

		if (ks->count >= alloc) {
			alloc = alloc ? alloc * 2 : 1024;
			ks->symbols = realloc(ks->symbols, alloc * sizeof(ks->symbols[0]));
			if (!ks->symbols) {
				st = set_err(ctx, KCORE_MEMORY);
				goto err;
			}
		}
		name_len = strlen(name) + 1;
		while (sbsize + name_len >= sballoc) {
			sballoc = sballoc ? sballoc * 2 : 1024;
			ks->strtab = realloc(ks->strtab, sballoc);
			if (!ks->strtab) {
				st = set_err(ctx, KCORE_MEMORY);
				goto err;
			}
		}
		strncpy(ks->strtab + sbsize, name, name_len);
		ks->symbols[ks->count].addr = addr;
		ks->symbols[ks->count].symbol = (char *)sbsize;
		ks->count++;
		line_number++;
		sbsize += name_len;
	}
	/* We stored offsets into the symbol field, to avoid the changing base
	 * pointer as we realloc ks->strtab. We can now properly set them. */
	for (size_t i = 0; i < ks->count; i++)
		ks->symbols[i].symbol += (size_t)ks->strtab;

	fclose(fp);
	free(line);
	return KCORE_OK;

err:
	if (fp)
		fclose(fp);
	free(ks->symbols);
	free(ks->strtab);
	ks->symbols = NULL;
	ks->strtab = NULL;
	return st;
}

static int symnamecmp(const void *lhs, const void *rhs, void *arg)
{
	struct kallsyms *ks = arg;
	const size_t *left = lhs, *right = rhs;
	return strcmp(ks->symbols[*left].symbol, ks->symbols[*right].symbol);
}

static kcore_status_t index_names(kcore_t *ctx, struct kallsyms *ks)
{
	ks->name_index = calloc(ks->count, sizeof(ks->name_index[0]));
	if (!ks->name_index)
		return set_err(ctx, KCORE_MEMORY);

	for (size_t i = 0; i < ks->count; i++) {
		ks->name_index[i] = i;
	}
	qsort_r(ks->name_index, ks->count, sizeof(ks->name_index[0]), symnamecmp, ks);
	return KCORE_OK;
}

/*
 * Since Linux kernel commit 73bbb94466fd3 ("kallsyms: support "big" kernel
 * symbols"), the "kallsyms_names" array may use the most significant bit to
 * indicate that the initial element for each symbol (normally representing the
 * number of tokens in the symbol) requires two bytes.
 *
 * Unfortunately, that means that values 128-255 are now ambiguous: on older
 * kernels, they should be interpreted literally, but on newer kernels, they
 * require treating as a two byte sequence. Since the commit included no changes
 * to the symbol names or vmcoreinfo, there's no way to detect it except via
 * heuristics.
 *
 * The commit in question is a new feature and not likely to be backported to
 * stable, so our heuristic is that it was first included in kernel 6.1.
 * However, we first check the environment variable KCORE_KALLSYMS_LONG: if it
 * exists, then we use its first character to determine our behavior: 1, y, Y
 * all indicate that we should use long names. 0, n, N all indicate that we
 * should not.
 */
static bool guess_long_names(kcore_t *ctx)
{
	const char *env = getenv("KCORE_KALLSYMS_LONG");

	if (env) {
		if (*env == '1' || *env == 'y' || *env == 'Y')
			return true;
		else if (*env == '0' || *env == 'n' || *env == 'N')
			return false;
	}

	return ctx->kernel_version >= KERNEL_VERSION(6, 1, 0);
}

struct kallsyms_locations {
	uint64_t kallsyms_names;
	uint64_t kallsyms_token_table;
	uint64_t kallsyms_token_index;
	uint64_t kallsyms_num_syms;
	uint64_t kallsyms_offsets;
	uint64_t kallsyms_relative_base;
	uint64_t kallsyms_addresses;
	uint64_t _stext;
};

static kcore_status_t kallsyms_get_locations(kcore_t *ctx, struct kallsyms_locations *loc)
{
	kdump_status ks;
	#define SYM(name) \
		ks = kdump_vmcoreinfo_symbol(ctx->kdump_ctx, #name, &loc->name); \
		if (ks != KDUMP_OK && ks != KDUMP_ERR_NODATA) \
			return set_kdumpfile_err(ctx, ks)
	SYM(kallsyms_names);
	SYM(kallsyms_token_table);
	SYM(kallsyms_token_index);
	SYM(kallsyms_num_syms);
	SYM(kallsyms_offsets);
	SYM(kallsyms_relative_base);
	SYM(kallsyms_addresses);
	SYM(_stext);
	#undef SYM

	if (!(loc->kallsyms_names && loc->kallsyms_token_table
	    && loc->kallsyms_token_index && loc->kallsyms_num_syms))
		return set_err_fmt(ctx, "The symbols kallsyms_names, kallsyms_token_table, "
				   "kallsyms_token_index, and kallsyms_num_syms were not "
				   "found in the VMCOREINFO note. There is not enough info "
				   "to use internal kallsyms.");

	return KCORE_OK;
}

/*
 * This struct contains the tables necessary to reconstruct kallsyms names.
 *
 * vmlinux (core kernel) kallsyms names are compressed using table compression.
 * There is some description of it in the kernel's "scripts/kallsyms.c", but
 * this is a brief overview that should make the code below comprehensible.
 *
 * Table compression uses the remaining 128 characters not defined by ASCII and
 * maps them to common substrings (e.g. the prefix "write_"). Each name is
 * represented as a sequence of bytes which refers to strings in this table.
 * The two arrays below comprise this table:
 *
 *   - token_table: this is one long string with all of the tokens concatenated
 *     together, e.g. "a\0b\0c\0...z\0write_\0read_\0..."
 *   - token_index: this is a 256-entry long array containing the index into
 *     token_table where you'll find that token's string.
 *
 * To decode a string, for each byte you simply index into token_index, then use
 * that to index into token_table, and copy that string into your buffer.
 *
 * The actual kallsyms symbol names are concatenated into a buffer called
 * "names". The first byte in a name is the length (in tokens, not decoded
 * bytes) of the symbol name (though see below for a discussion of "long name"
 * support). The remaining "length" bytes are decoded via the table as described
 * above. The first decoded byte is a character representing what type of symbol
 * this is (e.g. text, data structure, etc).
 */
struct kallsyms_reader {
	uint32_t num_syms;
	uint8_t *names;
	char *token_table;
	uint16_t *token_index;
	bool long_names;
};

/*
 * Copy the kernel's table into process memory - without attempting to decode
 * anything just yet
 */
static kcore_status_t
kallsyms_copy_tables(kcore_t *ctx, struct kallsyms_reader *kr,
		     struct kallsyms_locations *loc)
{
	kcore_status_t st;
	const size_t token_index_size = (UINT8_MAX + 1) * sizeof(uint16_t);
	uint64_t last_token;
	size_t token_table_size, names_idx;
	char data;
	uint8_t len_u8;
	int len;

	/* Read num_syms from vmcore */
	st = kcore_read_u32(ctx, loc->kallsyms_num_syms, &kr->num_syms);
	if (st != KCORE_OK)
		return st;

	/* Read the constant-sized token_index table (256 entries) */
	kr->token_index = malloc(token_index_size);
	if (!kr->token_index)
		return set_err(ctx, KCORE_MEMORY);
	st = kcore_read(ctx, loc->kallsyms_token_index, kr->token_index, token_index_size);
	if (st != KCORE_OK)
		return st;

	/*
	 * Find the end of the last token, so we get the overall length of
	 * token_table. Then copy the token_table into host memory.
	 */
	last_token = loc->kallsyms_token_table + kr->token_index[UINT8_MAX];
	do {
		st = kcore_read_u8(ctx, last_token, (uint8_t *)&data);
		if (st != KCORE_OK)
			return st;

		last_token++;
	} while (data);
	token_table_size = last_token - loc->kallsyms_token_table + 1;
	kr->token_table = malloc(token_table_size);
	if (!kr->token_table)
		return set_err(ctx, KCORE_MEMORY);
	st = kcore_read(ctx, loc->kallsyms_token_table, kr->token_table,
			token_table_size);
	if (st != KCORE_OK)
		return st;

	/* Now find the end of the names array by skipping through it, then copy
	 * that into host memory. */
	names_idx = 0;
	kr->long_names = guess_long_names(ctx);
	for (size_t i = 0; i < kr->num_syms; i++) {
		st = kcore_read_u8(ctx, loc->kallsyms_names + names_idx, &len_u8);
		if (st != KCORE_OK)
			return st;
		len = len_u8;
		if ((len & 0x80) && kr->long_names) {
			st = kcore_read_u8(ctx, loc->kallsyms_names + names_idx + 1,
					   &len_u8);
			if (st != KCORE_OK)
				return st;
			len = (len & 0x7F) | (len_u8 << 7);
			names_idx++;
		}
		names_idx += len + 1;
	}
	kr->names = malloc(names_idx);
	if (!kr->names)
		return set_err(ctx, KCORE_MEMORY);
	st = kcore_read(ctx, loc->kallsyms_names, kr->names, names_idx);
	if (st != KCORE_OK)
		return st;

	return KCORE_OK;
}

static unsigned int
kallsyms_expand_symbol(struct kallsyms_reader *kr, unsigned int offset,
		       char *result, size_t maxlen, char *kind_ret,
		       size_t *bytes_ret)
{
	uint8_t *data = &kr->names[offset];
	unsigned int len = *data;
	bool skipped_first = false;
	size_t bytes = 0;

	if ((len & 0x80) && kr->long_names) {
		data++;
		offset++;
		len = (0x7F & len) | (*data << 7);
	}

	offset += len + 1;
	data += 1;
	while (len) {
		char *token_ptr = &kr->token_table[kr->token_index[*data]];
		while (*token_ptr) {
			if (skipped_first) {
				if (maxlen <= 1)
					goto tail;
				*result = *token_ptr;
				result++;
				maxlen--;
				bytes++;
			} else {
				if (kind_ret)
					*kind_ret = *token_ptr;
				skipped_first = true;
			}
			token_ptr++;
		}

		data++;
		len--;
	}

tail:
	*result = '\0';
	bytes++;
	*bytes_ret = bytes;
	return offset;
}

static kcore_status_t
kallsyms_create_symbol_array(kcore_t *ctx, struct kallsyms_reader *kr)
{
	uint8_t token_lengths[UINT8_MAX+1];

	/* Compute the length of each token */
	for (int i = 0; i <= UINT8_MAX; i++) {
		token_lengths[i] = strlen(&kr->token_table[kr->token_index[i]]);
	}

	/* Now compute the length of all symbols together */
	size_t names_idx = 0;
	size_t length = 0;
	for (int i = 0; i < kr->num_syms; i++) {
		unsigned int num_tokens = kr->names[names_idx];
		if ((num_tokens & 0x80) && kr->long_names)
			num_tokens = (num_tokens & 0x7F) | (kr->names[++names_idx] << 7);
		for (int j = names_idx + 1; j < names_idx + num_tokens + 1; j++)
			length += token_lengths[kr->names[j]];
		length++; /* nul terminator */
		names_idx += num_tokens + 1;
	}

	ctx->ks.strtab = malloc(length);
	ctx->ks.symbols = calloc(kr->num_syms, sizeof(*ctx->ks.symbols));
	ctx->ks.count = kr->num_syms;
	if (!ctx->ks.strtab || !ctx->ks.symbols)
		return set_err(ctx, KCORE_MEMORY);

	names_idx = 0;
	size_t symbols_idx = 0;
	for (int i = 0; i < kr->num_syms; i++) {
		size_t bytes = 0;
		names_idx = kallsyms_expand_symbol(kr, names_idx,
						   ctx->ks.strtab + symbols_idx,
						   length - symbols_idx, NULL,
						   &bytes);
		ctx->ks.symbols[i].symbol = &ctx->ks.strtab[symbols_idx];
		symbols_idx += bytes;
	}
	return KCORE_OK;
}

/* Compute an address via the CONFIG_KALLSYMS_ABSOLUTE_PERCPU method */
static uint64_t absolute_percpu(uint64_t base, int32_t val)
{
	if (val >= 0)
		return (uint64_t) val;
	else
		return base - 1 - val;
}

/**
 * Load the kallsyms address information from @a prog
 *
 * Just as symbol name loading is complex, so is address loading. Addresses may
 * be stored directly as an array of pointers, but more commonly, they are
 * stored as an array of 32-bit integers which are related to an offset. This
 * function decodes the addresses into a plain array of 64-bit addresses.
 *
 * @param prog The program to read from
 * @param kr The symbol registry to fill
 * @param vi vmcoreinfo containing necessary symbols
 * @returns NULL on success, or error
 */
static kcore_status_t
kallsyms_load_addresses(kcore_t *ctx, struct kallsyms_locations *loc)
{
	kcore_status_t st = KCORE_OK;

	/* NOTE: assumes 64 bit architecture with same byte order */

	if (loc->kallsyms_addresses) {
		/*
		 * The kallsyms addresses are stored as plain addresses in an
		 * array of unsigned long! Read it and copy it into the syms.
		 */
		uint64_t *addresses = calloc(ctx->ks.count, sizeof(addresses[0]));
		if (!addresses)
			return set_err(ctx, KCORE_MEMORY);
		st = kcore_read(ctx, loc->kallsyms_addresses, addresses,
				ctx->ks.count * sizeof(addresses[0]));
		if (st != KCORE_OK) {
			free(addresses);
			return st;
		}
		for (int i = 0; i < ctx->ks.count; i++)
			ctx->ks.symbols[i].addr = addresses[i];
		free(addresses);
	} else {
		/*
		 * The kallsyms addresses are stored in an array of 4-byte
		 * values, which can be interpreted in two ways:
		 * (1) if CONFIG_KALLSYMS_ABSOLUTE_PERCPU is enabled, then
		 *     positive values are addresses, and negative values are
		 *     offsets from a base address.
		 * (2) otherwise, the 4-byte values are directly used as
		 *     addresses
		 * First, read the values, then figure out which way to
		 * interpret them.
		 */
		uint64_t relative_base;
		st = kcore_read_u64(ctx, loc->kallsyms_relative_base,
				    &relative_base);
		if (st != KCORE_OK)
			return st;

		uint32_t *addr32 = malloc(ctx->ks.count * sizeof(addr32[0]));
		if (!addr32)
			return set_err(ctx, KCORE_MEMORY);

		st = kcore_read(ctx, loc->kallsyms_offsets, addr32,
					ctx->ks.count * sizeof(addr32[0]));
		if (st != KCORE_OK) {
			free(addr32);
			return st;
		}

		/*
		 * Now that we've read the offsets data, we need to determine
		 * how to interpret them. To do this, use the _stext symbol. We
		 * have the correct value from vmcoreinfo. Compute it both ways
		 * and pick the correct interpretation.
		 */
		size_t stext_idx;
		st = kcore_sym_lookup_index(ctx, "_stext", &stext_idx);
		if (st != KCORE_OK) {
			free(addr32);
			return set_err_fmt(ctx, "Could not find _stext symbol in kallsyms");
		}

		uint64_t stext_abs = relative_base + addr32[stext_idx];
		uint64_t stext_pcpu = absolute_percpu(relative_base, (int32_t)addr32[stext_idx]);
		if (stext_abs == loc->_stext) {
			for (int i = 0; i < ctx->ks.count; i++)
				ctx->ks.symbols[i].addr = relative_base + addr32[i];
		} else if (stext_pcpu == loc->_stext) {
			for (int i = 0; i < ctx->ks.count; i++)
				ctx->ks.symbols[i].addr = absolute_percpu(relative_base, (int32_t)addr32[i]);
		} else {
			st = set_err_fmt(ctx, "Unable to interpret kallsyms address data");
		}
		free(addr32);
	}
	return st;
}

static kcore_status_t read_kallsyms_vmcoreinfo(kcore_t *ctx)
{
	kcore_status_t st;
	struct kallsyms_locations loc = {0};
	struct kallsyms_reader reader = {0};

	st = kallsyms_get_locations(ctx, &loc);
	if (st != KCORE_OK)
		goto err;

	st = kallsyms_copy_tables(ctx , &reader, &loc);
	if (st != KCORE_OK)
		goto err;

	st = kallsyms_create_symbol_array(ctx, &reader);
	if (st != KCORE_OK)
		goto err;

	/* Need name index in order to search for symbol in load_addresses */
	st = index_names(ctx, &ctx->ks);
	if (st != KCORE_OK)
		goto err;

	st = kallsyms_load_addresses(ctx, &loc);
err:
	free(reader.names);
	free(reader.token_index);
	free(reader.token_table);
	return st;
}

static void kallsyms_free(struct kallsyms *ks)
{
	free(ks->symbols);
	free(ks->name_index);
	free(ks->strtab);
}

struct symsearch {
	struct kallsyms *ks;
	const char *query;
};

static int symsearchcmp(const void *key, const void *memb)
{
	const struct symsearch *ss = key;
	const size_t *mem = memb;
	return strcmp(ss->query, ss->ks->symbols[*mem].symbol);
}

static kcore_status_t kcore_sym_lookup_index(kcore_t *kcore, const char *name, size_t *index)
{
	struct symsearch ss = {&kcore->ks, name};
	void *res = bsearch(&ss, kcore->ks.name_index, kcore->ks.count,
			    sizeof(kcore->ks.name_index[0]), symsearchcmp);
	if (!res)
		return set_err(kcore, KCORE_NOT_FOUND);
	*index = *(size_t *)res;
	return KCORE_OK;
}
kcore_status_t kcore_sym_lookup(kcore_t *kcore, const char *name, uint64_t *addr)
{
	struct symsearch ss = {&kcore->ks, name};
	void *res = bsearch(&ss, kcore->ks.name_index, kcore->ks.count,
			    sizeof(kcore->ks.name_index[0]), symsearchcmp);
	if (!res)
		return set_err(kcore, KCORE_NOT_FOUND);
	*addr = kcore->ks.symbols[*(size_t *)res].addr;
	return KCORE_OK;
}

//////////////////////////////////
// Memory access

#define read(tp) \
	kcore_status_t kcore_read_u ## tp (kcore_t *ctx, uint64_t addr, uint ## tp ## _t *val) { \
		size_t read_len = sizeof(*val); \
		kdump_status s = kdump_read(ctx->kdump_ctx, KDUMP_KVADDR, addr, val, &read_len); \
		if (s != KDUMP_OK) \
			return set_kdumpfile_err(ctx, s); \
		return KCORE_OK; \
	}
read(64)
read(32)
read(16)
read(8)
#undef read

kcore_status_t kcore_read(kcore_t *ctx, uint64_t addr, void *buf, size_t len)
{
	kdump_status s = kdump_read(ctx->kdump_ctx, KDUMP_KVADDR, addr, buf, &len);
	if (s != KDUMP_OK)
		return set_kdumpfile_err(ctx, s);
	return KCORE_OK;
}

//////////////////////////////////
// Initialization & destruction

kcore_t *kcore_alloc(void)
{
	kcore_t *ctx = malloc(sizeof(*ctx));
	if (ctx) {
		memset(ctx, 0, sizeof(*ctx));
		ctx->core_fd = -1;
	}
	return ctx;
}

static bool use_kallsyms_vmcoreinfo(void)
{
	const char *env = getenv("KCORE_USE_KALLSYMS_VMCOREINFO");
	return env && (*env == 'y' || *env == 'Y' || *env == '1');
}

kcore_status_t kcore_init(kcore_t *ctx, const char *path, const char *ctf)
{
	kdump_status ks;
	kcore_status_t st;

	if (ctx->core_fd != -1)
		return set_err(ctx, KCORE_ALREADY_INITIALIZED);

	ctx->core_fd = open(path, O_RDONLY);
	if (ctx->core_fd < 0) {
		st = set_os_err(ctx);
		goto out;
	}

	/* Initialize libkdumpfile */
	ctx->kdump_ctx = kdump_new();
	if (!ctx->kdump_ctx) {
		st = set_err(ctx, KCORE_MEMORY);
		goto out;
	}
	ks = kdump_set_number_attr(ctx->kdump_ctx, KDUMP_ATTR_FILE_FD, ctx->core_fd);
	if (ks != KDUMP_OK) {
		st = set_kdumpfile_err(ctx, ks);
		goto out;
	}
	ks = kdump_set_string_attr(ctx->kdump_ctx, KDUMP_ATTR_OSTYPE, "linux");
	if (ks != KDUMP_OK) {
		st = set_kdumpfile_err(ctx, ks);
		goto out;
	}
	kdump_num_t version_code;
	ks = kdump_get_number_attr(ctx->kdump_ctx, KDUMP_ATTR_LINUX_VERSION_CODE, &version_code);
	if (ks != KDUMP_OK) {
		st = set_kdumpfile_err(ctx, ks);
		goto out;
	}
	ctx->kernel_version = (uint32_t)version_code;

	/* Initialize kallsyms */
	if (strcmp(path, "/proc/kcore") == 0 && !use_kallsyms_vmcoreinfo()) {
		st = read_proc_kallsyms(ctx, &ctx->ks);
		if (st != KCORE_OK)
			goto out;
		st = index_names(ctx, &ctx->ks);
		if (st != KCORE_OK)
			goto out;
	} else {
		st = read_kallsyms_vmcoreinfo(ctx);
		if (st != KCORE_OK)
			goto out;
	}

	return KCORE_OK;
out:
	free(ctx->ks.name_index);
	free(ctx->ks.symbols);
	free(ctx->ks.strtab);
	if(ctx->kdump_ctx) {
		kdump_free(ctx->kdump_ctx);
		ctx->kdump_ctx = NULL;
	}
	ctx->core_fd = -1;
	return st;
}

void kcore_free(kcore_t *ctx)
{
	kallsyms_free(&ctx->ks);
	close(ctx->core_fd);
	free(ctx);
}
