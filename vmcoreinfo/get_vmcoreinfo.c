/**
 * Print the vmcoreinfo note of a vmcore to stdout. Supports ELF or kdump
 * compressed ("diskdump") formats, thanks to libkdumpfile.
 *
 * gcc -g -o get_vmcoreinfo get_vmcoreinfo.c -lkdumpfile
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <fcntl.h>

#include <libkdumpfile/kdumpfile.h>

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

int main(int argc, char **argv)
{
	kdump_ctx_t *ctx;
	kdump_status ks;
	int fd;
	char *vmcoreinfo;

	if (argc != 2)
		fail("usage: %s VMCORE\n", argv[0]);

	fd = open(argv[1], O_RDONLY);
	if (fd < 0)
		perror_fail("open");

	ctx = kdump_new();
	if (!ctx)
		fail("kdump_new() failed\n");

	ks = kdump_set_number_attr(ctx, KDUMP_ATTR_FILE_FD, fd);
	if (ks != KDUMP_OK)
		fail("kdump_set_number_attr(KDUMP_ATTR_FILE_FD): %s\n",
		     kdump_get_err(ctx));

	ks = kdump_set_string_attr(ctx, KDUMP_ATTR_OSTYPE, "linux");
	if (ks != KDUMP_OK)
		fail("kdump_set_string_attr(KDUMP_ATTR_OSTYPE): %s\n",
		     kdump_get_err(ctx));

	ks = kdump_vmcoreinfo_raw(ctx, &vmcoreinfo);
	if (ks != KDUMP_OK)
		fail("kdump_vmcoreinfo_raw: %s\n", kdump_get_err(ctx));

	printf("%s", vmcoreinfo);
	return EXIT_SUCCESS;
}
