#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <byteswap.h>

#include <elf.h>

static void fail(const char *msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(EXIT_FAILURE);
}

static void perror_fail(const char *pfx)
{
	perror(pfx);
	exit(EXIT_FAILURE);
}

static void usage(void)
{
	puts("usage: phys2virt COREFILE");
	puts("Modifies the ELF COREFILE so that load segments have their virtual");
	puts("address value copied from the physical address field.");
	exit(EXIT_SUCCESS);
}

static int endian(void)
{
	union {
		uint32_t ival;
		char     cval[4];
	} data;
	data.ival = 1;
	if (data.cval[0])
		return ELFDATA2LSB;
	else
		return ELFDATA2MSB;
}

int main(int argc, char **argv)
{
	char *filename;
	FILE *f;
	Elf64_Ehdr hdr;
	Elf64_Phdr *phdrs;
	off_t phoff;
	int phnum, phentsize;

	if (argc != 2 || strcmp(argv[1], "-h") == 0)
		usage();

	filename = argv[1];
	f = fopen(filename,  "r+");
	if (!f)
		perror_fail("open");

	if (fread(&hdr, sizeof(hdr), 1, f) != 1)
		perror_fail("read elf header");

	if (memcmp(hdr.e_ident, ELFMAG, 4) != 0)
		fail("not an ELF file");

	if (hdr.e_ident[EI_CLASS] != ELFCLASS64)
		fail("file is not 64-bits: unsupported");

	if (endian() != hdr.e_ident[EI_DATA]) {
		phoff = bswap_64(hdr.e_phoff);
		phnum = bswap_16(hdr.e_phnum);
		phentsize = bswap_16(hdr.e_phentsize);
	} else {
		phoff = hdr.e_phoff;
		phnum = hdr.e_phnum;
		phentsize = hdr.e_phentsize;
	}
	if (phentsize != sizeof(Elf64_Phdr))
		fail("error: mismatch between phentsize and sizeof(Elf64_Phdr)");

	if (fseek(f, phoff, SEEK_SET) < 0)
		perror_fail("fseek");

	phdrs = calloc(phnum, phentsize);
	if (!phdrs)
		fail("error: allocation error");

	if (fread(phdrs, phentsize, phnum, f) != phnum)
		perror_fail("fread phdrs");

	for (int i = 0; i < phnum; i++)
		phdrs[i].p_vaddr = phdrs[i].p_paddr;

	if (fseek(f, phoff, SEEK_SET) < 0)
		perror_fail("fseek");
	if (fwrite(phdrs, phentsize, phnum, f) != phnum)
		perror_fail("fwrite phdrs");

	fclose(f);
	return EXIT_SUCCESS;
}
