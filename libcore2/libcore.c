#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "libcore.h"

struct sym {
	char *symbol;
	uint64_t addr;
};

struct kallsyms {
	struct sym *symbols;
	size_t *name_index;
	size_t count;
};

struct kcore {
	struct kallsyms ks;
	int core_fd;
	kdump_ctx_t *kdump_ctx;

	/* Error info */
	kcore_status_t last_err;
	union {
		int kcore_errno;
		kdump_status kcore_kdump_status;
		char *kcore_formatted;
	};
};

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
	FILE *fp = fopen("/proc/kallsyms", "r");
	if (!fp)
		return set_os_err(ctx);

	memset(ks, 0, sizeof(*ks));

	while ((res = getline(&line, &line_size, fp)) != -1) {
		char *save = NULL;
		char *name, *addr_str, *type_str, *mod, *addr_rem;
		char type;
		uint64_t addr;

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
		ks->symbols[ks->count].addr = addr;
		ks->symbols[ks->count].symbol = strdup(name);
		ks->count++;
		line_number++;
	}

	fclose(fp);
	free(line);
	return KCORE_OK;

err:
	if (fp)
		fclose(fp);
	free(ks->symbols);
	ks->symbols = NULL;
	return st;
}

static void kallsyms_free(struct kallsyms *ks)
{
	free(ks->symbols);
	free(ks->name_index);
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

kcore_status_t kcore_sym_lookup(kcore_t *kcore, const char *name, uint64_t *addr)
{
	struct symsearch ss = {&kcore->ks, name};
	void *res = bsearch(&ss, kcore->ks.name_index, kcore->ks.count,
			    sizeof(kcore->ks.name_index[0]), symsearchcmp);
	if (!res)
		return KCORE_NOT_FOUND;
	size_t *index = res;
	*addr = kcore->ks.symbols[*index].addr;
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

	/* Initialize kallsyms */
	if (strcmp(path, "/proc/kcore") == 0) {
		st = read_proc_kallsyms(ctx, &ctx->ks);
		if (st != KDUMP_OK)
			goto out;
		st = index_names(ctx, &ctx->ks);
		if (st != KDUMP_OK)
			goto out;
	} else {
		st = set_err(ctx, KCORE_NOT_IMPLEMENTED);
		goto out;
	}

	return KCORE_OK;
out:
	free(ctx->ks.name_index);
	free(ctx->ks.symbols);
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
