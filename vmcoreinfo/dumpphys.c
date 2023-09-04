/**
 * Dump the (uncompressed) physical memory from a core out to stdout.
 * Alternatively, search for a vmcoreinfo note inside that physical memory and
 * output it if we find it.
 *
 * gcc -g -o dumphys{,.c} -lkdumpfile
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <fcntl.h>
#include <assert.h>
#include <string.h>
#include <getopt.h>

#include <libkdumpfile/kdumpfile.h>
#include <unistd.h>

#define GB (1UL << 30)
#define MB (1UL << 20)

void fail(const char *fmt, ...)
{
	va_list vl;
	va_start(vl, fmt);
	vfprintf(stderr, fmt, vl);
	va_end(vl);
	exit(EXIT_FAILURE);
}

void perror_fail(const char *msg)
{
	perror(msg);
	exit(EXIT_FAILURE);
}

struct memory_info {
	kdump_num_t max_pfn;
	kdump_num_t page_size;
	kdump_num_t page_shift;
	kdump_bmp_t *pagemap;
};

void get_memory_info(kdump_ctx_t *ctx, struct memory_info *meminfo)
{
	kdump_status ks;
	kdump_attr_t bitmap_attr;

	ks = kdump_get_number_attr(ctx, "max_pfn", &meminfo->max_pfn);
	if (ks != KDUMP_OK)
		fail("kdump_get_number_attr(max_pfn): %s\n",
		     kdump_get_err(ctx));

	ks = kdump_get_number_attr(ctx, KDUMP_ATTR_PAGE_SIZE, &meminfo->page_size);
	if (ks != KDUMP_OK)
		fail("kdump_get_number_attr(KDUMP_ATTR_PAGE_SIZE): %s\n",
		     kdump_get_err(ctx));

	ks = kdump_get_number_attr(ctx, KDUMP_ATTR_PAGE_SHIFT, &meminfo->page_shift);
	if (ks != KDUMP_OK)
		fail("kdump_get_number_attr(KDUMP_ATTR_PAGE_SHIFT): %s\n",
		     kdump_get_err(ctx));

	ks = kdump_get_attr(ctx, KDUMP_ATTR_FILE_PAGEMAP, &bitmap_attr);
	if (ks != KDUMP_OK)
		fail("kdump_get_attr(KDUMP_ATTR_FILE_PAGEMAP): %s\n",
		     kdump_get_err(ctx));
	assert(bitmap_attr.type == KDUMP_BITMAP);
	meminfo->pagemap = bitmap_attr.val.bitmap;
}

typedef int (*page_fn)(void *, uint64_t addr, uint64_t len, uint8_t *buf);

int dump_page(void *arg, uint64_t addr, uint64_t len, uint8_t *buf)
{
	int *pFd = arg;
	int rv = write(*pFd, buf, len);
	return (rv < 0) ? -1 : 0;
}

int check_vmcoreinfo(void *arg, uint64_t addr, uint64_t len, uint8_t *buf)
{
	int *pFd = arg;
	if (strncmp("OSRELEASE=", (char *)buf, 10) == 0) {
		int len = strlen((char *)buf);
		write(*pFd, (char *)buf, len);
		return -1;
	}
	return 0;
}

int for_each_present_page(kdump_ctx_t *ctx, struct memory_info *mi, page_fn fn, void *arg, bool verbose, bool persist)
{
	kdump_status ks;
	kdump_addr_t addr = 0;
	bool nodata = false;
	kdump_addr_t max_addr = mi->max_pfn << mi->page_shift;
	uint64_t pages_read = 0;

	char *buf = malloc(mi->page_size);

	while (1) {
		kdump_addr_t begin, end;

		ks = kdump_bmp_find_set(mi->pagemap, &addr);
		if (ks == KDUMP_ERR_NODATA)
			break;
		if (ks != KDUMP_OK) {
			fail("kdump_bmp_find_set: %s", kdump_bmp_get_err(mi->pagemap));
		}
		begin = addr;
		ks = kdump_bmp_find_clear(mi->pagemap, &addr);
		if (ks != KDUMP_OK) {
			fail("kdump_bmp_find_set: %s", kdump_bmp_get_err(mi->pagemap));
		}
		end = addr;
		if (verbose)
			fprintf(stderr, "Data present range: page frames 0x%lx - 0x%lx\n", begin, end);

		for (kdump_addr_t offset = begin; offset < end; offset++) {
			size_t len = mi->page_size;
			ks = kdump_read(ctx, KDUMP_MACHPHYSADDR, offset << mi->page_shift, buf, &len);
			if (ks != KDUMP_OK) {
				fprintf(stderr, "kdump_read: %s\n", kdump_get_err(ctx));
				if (!persist)
					exit(EXIT_FAILURE);
				continue;
			}
			pages_read += 1;
			int rv = fn(arg, offset << mi->page_shift, mi->page_size - len, (uint8_t *)buf);
			if (rv < 0) {
				free(buf);
				return rv;
			}
		}
	}
	if (verbose)
		fprintf(stderr, "Processed %lu present pages (total: %lu). That's %lu MiB / %lu MiB\n",
			pages_read, mi->max_pfn,
			(pages_read << mi->page_shift) / MB, (mi->max_pfn << mi->page_shift) / MB);
	return 0;
}

void help(void)
{
	puts(
		"usage: dumpphys [OPTIONS] -c VMCORE [-o OUTPUT]\n"
		"\n"
		"Dumps raw memory contents from a vmcore (ELF or kdump) to stdout, or to the\n"
		"file indicated by OUTPUT. Alternatively, if --vmcoreinfo is provided, searches\n"
		"for any page that looks like a vmcoreinfo page and outputs the first one to\n"
		"stdout or the file indicated by OUTPUT.\n"
		"\n"
		"  -c, --core VMCORE    specifies the vmcore to read (required)\n"
		"  -o, --output OUTPUT  specifies where to write output (default: stdout)\n"
		"  --vmcoreinfo, -i     if flag is active, searches for vmcoreinfo rather than\n"
		"                       dumping all memory contents.\n"
		"  --verbose, -v        prints information about progress to stderr\n"
		"  --persist, -p        continue trying to read pages of data even after we\n"
		"                       encounter a read error\n"
		"  --help, -h           print this message and exit"
	);
	exit(EXIT_SUCCESS);
}

int main(int argc, char **argv)
{
	kdump_ctx_t *ctx;
	kdump_status ks;
	struct memory_info mi;
	int in_fd = -1;
	int out_fd = STDOUT_FILENO;
	int rv;
	page_fn op = dump_page;
	bool verbose = false;
	bool persist = false;

	int opt;
	const char *shopt = "c:o:ivhp";
	static struct option lopt[] = {
		{"core",       required_argument, NULL, 'c'},
		{"output",     required_argument, NULL, 'o'},
		{"vmcoreinfo", no_argument,       NULL, 'i'},
		{"verbose",    no_argument,       NULL, 'v'},
		{"help",       no_argument,       NULL, 'h'},
		{"persist",    no_argument,       NULL, 'p'},
		{0},
	};
	while ((opt = getopt_long(argc, argv, shopt, lopt, NULL)) != -1) {
		switch (opt) {
			case 'h':
				help();
				break;
			case 'c':
				in_fd = open(optarg, O_RDONLY);
				if (in_fd < 0)
					perror_fail("open vmcore");
				break;
			case 'o':
				out_fd = open(optarg, O_WRONLY);
				if (out_fd < 0)
					perror_fail("open output");
				break;
			case 'i':
				op = check_vmcoreinfo;
				break;
			case 'v':
				verbose = true;
				break;
			case 'p':
				persist = true;
				break;
			default:
				fprintf(stderr, "Invalid argument\n");
				exit(EXIT_FAILURE);
		}
	}

	if (in_fd < 0)
		fail("--core is a required argument! See -h for help output.");

	ctx = kdump_new();
	if (!ctx)
		fail("kdump_new() failed\n");

	ks = kdump_set_number_attr(ctx, KDUMP_ATTR_FILE_FD, in_fd);
	if (ks != KDUMP_OK)
		fail("kdump_set_number_attr(KDUMP_ATTR_FILE_FD): %s\n",
		     kdump_get_err(ctx));

	ks = kdump_set_string_attr(ctx, KDUMP_ATTR_OSTYPE, "linux");
	if (ks != KDUMP_OK)
		fail("kdump_set_string_attr(KDUMP_ATTR_OSTYPE): %s\n",
		     kdump_get_err(ctx));

	get_memory_info(ctx, &mi);
	rv = for_each_present_page(ctx, &mi, op, &out_fd, verbose, persist);
	if (rv == 0 && op == check_vmcoreinfo) {
		fprintf(stderr, "error: could not find anything that looks like vmcoreinfo\n");
	}
	kdump_free(ctx);
	return rv ? EXIT_FAILURE : EXIT_SUCCESS;
}
