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
#include <time.h>

#include <libkdumpfile/kdumpfile.h>
#include <unistd.h>

#define GB (1UL << 30)
#define MB (1UL << 20)

struct progress {
	uint64_t total_bytes;
	uint64_t current_bytes;

	struct timespec start_time;
	struct timespec last_update;
	struct timespec update_interval;

	bool print;
};

struct timespec timespec_sub(const struct timespec *a, const struct timespec *b)
{
	struct timespec ret = *a;
	if (ret.tv_nsec < b->tv_nsec) {
		ret.tv_sec -= 1;
		ret.tv_nsec += 1000000000;
	}
	ret.tv_nsec -= b->tv_nsec;
	ret.tv_sec -= b->tv_sec;
	return ret;
}

long long timespec_cmp(const struct timespec *a, const struct timespec *b)
{
	if (a->tv_sec == b->tv_sec)
		return a->tv_nsec - b->tv_nsec;
	else
		return a->tv_sec - b->tv_sec;
}

void progress_update(struct progress *prog, bool force, uint64_t add_bytes)
{
	struct timespec now, since_last, elapsed;
	double percent, total_mb, curr_mb, seconds, mbps;

	/* Don't bother unless we're in verbose mode */
	if (!prog->print)
		return;

	/* Don't update unless we've passed the update interval. */
	prog->current_bytes += add_bytes;
	clock_gettime(CLOCK_MONOTONIC, &now);
	since_last = timespec_sub(&now, &prog->last_update);
	//fprintf(stderr, "since_last: %ld %ld\n", since_last.tv_sec, since_last.tv_nsec);
	//fprintf(stderr, "update_interval: %ld %ld\n", prog->update_interval.tv_sec, prog->update_interval.tv_nsec);
	if (!force && timespec_cmp(&since_last, &prog->update_interval) < 0)
		return;

	/* Compute and print progress */
	prog->last_update = now;
	percent = 100 * (double)prog->current_bytes / prog->total_bytes;
	total_mb = ((double)prog->total_bytes / (2 << 20));
	curr_mb = ((double)prog->current_bytes / (2 << 20));
	elapsed = timespec_sub(&now, &prog->start_time);
	seconds = (double)elapsed.tv_sec + (double)elapsed.tv_nsec / 1000000000;
	mbps = 0;
	if (seconds != 0)
		mbps = curr_mb / seconds;
	fprintf(stderr, "\r%10.2f / %10.2f MiB: %5.1f%% (%8.f MiB/s)",
		curr_mb, total_mb, percent, mbps);
	fflush(stderr);
}

void progress_complete(struct progress *prog)
{
	if (prog->print) {
		progress_update(prog, true, 0);
		fprintf(stderr, "\n");
	}
}

void progress_init(struct progress *prog, bool verbose, uint64_t total_bytes)
{
	prog->print = verbose;
	if (!verbose)
		return;
	prog->total_bytes = total_bytes;
	prog->current_bytes = 0;

	clock_gettime(CLOCK_MONOTONIC, &prog->start_time);
	prog->update_interval.tv_nsec = 200000000; /* 200ms */
	prog->update_interval.tv_sec = 0;
	prog->last_update.tv_sec = 0;
	prog->last_update.tv_nsec = 0;
}

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
	if (meminfo->page_size == 0) {
		fprintf(stderr, "warning: page_size set to zero, using a default of 4096\n");
		meminfo->page_size = 4096;
	}

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

struct vmcoreinfo_arg {
	int fd;
	bool should_continue;
	int found_count;
	bool search_within_page;
	kdump_ctx_t *ctx;
};

int check_vmcoreinfo(void *varg, uint64_t addr, uint64_t len, uint8_t *buf)
{
	struct vmcoreinfo_arg *arg = varg;
	void *found;
	if (!arg->search_within_page && strncmp("OSRELEASE=", (char *)buf, 10) == 0) {
		int vmcoreinfo_len = strlen((char *)buf);
		write(arg->fd, (char *)buf, vmcoreinfo_len);
		arg->found_count += 1;
		if (arg->should_continue)
			write(arg->fd, "---\n", 4);
		else
			return -1;
	} else if (arg->search_within_page){
		if(!(found = memmem(buf, len, "OSRELEASE=", 10)))
			return 0;
		char *str;
		kdump_status st = kdump_read_string(arg->ctx, KDUMP_MACHPHYSADDR,
						    addr + (found - (void *)buf), &str);
		if (st != KDUMP_OK) {
			fprintf(stderr, "error reading string: %s\n", kdump_strerror(st));
			return -1;
		}
		write(arg->fd, str, strlen(str));
		free(str);
		if (arg->should_continue)
			write(arg->fd, "---\n", 4);
		else
			return -1;
	}
	return 0;
}

int count_pages(kdump_ctx_t *ctx, struct memory_info *mi, page_fn fn, void *arg, bool verbose, bool persist)
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

		struct progress prog;
		progress_init(&prog, verbose, (end - begin) << mi->page_shift);
		for (kdump_addr_t offset = begin; offset < end; offset++) {
			size_t len = mi->page_size;
			ks = kdump_read(ctx, KDUMP_MACHPHYSADDR, offset << mi->page_shift, buf, &len);
			progress_update(&prog, false, mi->page_size); /* it's a "success" regardless */
			if (ks != KDUMP_OK) {
				fprintf(stderr, "\nkdump_read: %s\n", kdump_get_err(ctx));
				if (!persist)
					exit(EXIT_FAILURE);
				continue;
			}
			pages_read += 1;
			int rv = fn(arg, offset << mi->page_shift, len, (uint8_t *)buf);
			if (rv < 0) {
				progress_complete(&prog);
				free(buf);
				return rv;
			}
		}
		progress_complete(&prog);
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
		"  -I                   same as -i, but keeps searching after finding one\n"
		"  --flexible, -f       when searching for vmcoreinfo, also search outside of\n"
		"                       page boundaries. Useful for older kernels\n"
		"  --verbose, -v        prints information about progress to stderr\n"
		"  --persist, -p        continue trying to read pages of data even after we\n"
		"                       encounter a read error\n"
		"  --help, -h           print this message and exit\n"
		"\n"
		"Stephen Brennan <stephen.s.brennan@oracle.com>"
	);
	exit(EXIT_SUCCESS);
}

int main(int argc, char **argv)
{
	kdump_ctx_t *ctx;
	kdump_status ks;
	struct memory_info mi;
	int in_fd = -1;
	int rv;
	page_fn op = dump_page;
	bool verbose = false;
	bool persist = false;
	struct vmcoreinfo_arg via = {
		.fd = STDOUT_FILENO,
		.should_continue = false,
		.search_within_page = false,
	};

	int opt;
	const char *shopt = "c:o:iIvhpf";
	static struct option lopt[] = {
		{"core",       required_argument, NULL, 'c'},
		{"output",     required_argument, NULL, 'o'},
		{"vmcoreinfo", no_argument,       NULL, 'i'},
		{"verbose",    no_argument,       NULL, 'v'},
		{"help",       no_argument,       NULL, 'h'},
		{"persist",    no_argument,       NULL, 'p'},
		{"flexible",   no_argument,       NULL, 'f'},
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
				via.fd = open(optarg, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
				if (via.fd < 0)
					perror_fail("open output");
				break;
			case 'I':
				via.should_continue = true;
			case 'i':
				op = check_vmcoreinfo;
				break;
			case 'v':
				verbose = true;
				break;
			case 'p':
				persist = true;
				break;
			case 'f':
				via.search_within_page = true;
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

	via.ctx = ctx;
	ks = kdump_set_number_attr(ctx, KDUMP_ATTR_FILE_FD, in_fd);
	if (ks != KDUMP_OK)
		fail("kdump_set_number_attr(KDUMP_ATTR_FILE_FD): %s\n",
		     kdump_get_err(ctx));

	ks = kdump_set_string_attr(ctx, KDUMP_ATTR_OSTYPE, "linux");
	if (ks != KDUMP_OK)
		fail("kdump_set_string_attr(KDUMP_ATTR_OSTYPE): %s\n",
		     kdump_get_err(ctx));

	get_memory_info(ctx, &mi);
	rv = count_pages(ctx, &mi, op, &via, verbose, persist);
	if (op == check_vmcoreinfo && via.found_count > 1)
		fprintf(stderr, "found %d vmcoreinfo-like notes\n", via.found_count);
	else if (rv == 0 && via.found_count == 0 && op == check_vmcoreinfo)
		fprintf(stderr, "error: could not find anything that looks like vmcoreinfo\n");
	kdump_free(ctx);
	return rv ? EXIT_FAILURE : EXIT_SUCCESS;
}
