/*
 * ctfdictprint.c: a very basic CTF "hello world" program.
 *
 * This could be useful as a starting off point for building a more complicated
 * testing program, or you could use it to test building and linking against
 * libctf.
 */
#include <stdio.h>
#include <stdlib.h>

#include <ctf-api.h>


void perror_fail(const char *msg)
{
	perror(msg);
	exit(EXIT_FAILURE);
}

size_t read_file(const char *file, char **buf_ret)
{
	size_t size, amt;
	char *buf;
	FILE *f = fopen(file, "r");

	if (!f)
		perror_fail("Error opening CTF file");

	if (fseek(f, 0, SEEK_END) == -1)
		perror_fail("Error seeking to end of CTF file");

	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc(size + 1);
	if (!buf)
		perror_fail("Allocation failed");

	amt = fread(buf, 1, size, f);
	if (amt != size)
		perror_fail("Error reading CTF file");

	fclose(f);
	buf[size] = '\0';
	*buf_ret = buf;
	return size;
}

ctf_archive_t *open_ctf(const char *file)
{
	ctf_archive_t *arc;
	int errnum;
	ctf_sect_t data = {0};
	data.cts_size = read_file(file, (char **)&data.cts_data);
	arc = ctf_arc_bufopen(&data, NULL, NULL, &errnum);
	if (!arc) {
		fprintf(stderr, "ctf_arc_bufopen \"%s\": %s\n", file,
		        ctf_errmsg(errnum));
		exit(EXIT_FAILURE);
	}
	return arc;
}

int visit_dict(ctf_dict_t *fp, const char *name, void *arg)
{
	printf("%s\n", name);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: ctfdump VMLINUX.CTFA\n");
		return EXIT_FAILURE;
	}

	ctf_archive_t *arc = open_ctf(argv[1]);
	ctf_archive_iter(arc, visit_dict, NULL);
	ctf_close(arc);
	return EXIT_SUCCESS;
}
