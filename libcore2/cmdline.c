#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "libcore.h"

void fail(const char *fmt, ...)
{
	va_list vl;
	va_start(vl, fmt);
	vfprintf(stderr, fmt, vl);
	va_end(vl);
	exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
	kcore_status_t ks;
	kcore_t *ctx = kcore_alloc();
	char *core = "/proc/kcore";
	char *ctf = NULL;

	if (!ctx)
		fail("memory allocation");

	if (argc >= 2)
		core = argv[1];
	if (argc >= 3)
		ctf = argv[2];

	ks = kcore_init(ctx, core, ctf);
	if (ks != KCORE_OK)
		kcore_fail(ctx, "kcore_init");

	uint64_t cmdline_ptr = kcore_sym_u64_n(ctx, "saved_command_line");
	uint32_t cmdline_len = kcore_sym_u32_n(ctx, "saved_command_line_len");

	char *cmdline = malloc(cmdline_len + 1);
	if (!cmdline)
		fail("allocation error");

	ks = kcore_read(ctx, cmdline_ptr, cmdline, cmdline_len);
	if (ks != KCORE_OK)
		kcore_fail(ctx, "kdump_read(cmdline)");

	cmdline[cmdline_len] = '\0';
	printf("cmdline: %s\n", cmdline);

	free(cmdline);
	kcore_free(ctx);
}
