/*
 * Edit the "Build ID" note of an ELF file.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

#include <unistd.h>
#include <fcntl.h>

#include <elf.h>

struct elf_notehdr {
	uint32_t namesz;
	uint32_t descsz;
	uint32_t type;
	char name[0];
};

static size_t pad4(size_t val)
{
	if (val & 3)
		return (val & (~(size_t)3)) + 4;
	return val;
}

static char *note_desc(struct elf_notehdr *nhdr)
{
	return &nhdr->name[pad4(nhdr->namesz)];
}

static struct elf_notehdr *next_note(struct elf_notehdr *nhdr)
{
	return (struct elf_notehdr *)&nhdr->name[
		pad4(nhdr->namesz) + pad4(nhdr->descsz)];
}

static bool end_notes(void *start, size_t len, void *ptr)
{
	return ((uintptr_t)ptr - (uintptr_t)start) >= len;
}

static inline char nibble_to_hex(uint8_t input)
{
	if (input >= 0 && input < 10)
		return '0' + input;
	else if (input >= 10 && input < 16)
		return 'a' + input - 10;
	assert(0);
}

static inline uint8_t hex_to_nibble(char input)
{
	if (input >= '0' && input <= '9')
		return input - '0';
	else if (input >= 'a' && input <= 'f')
		return input - 'a' + 10;
	else if (input >= 'A' && input <= 'F')
		return input - 'A' + 10;
	assert(0);
}

static char *to_hex(uint8_t *data, int size)
{
	char *hex_data = calloc(1, size * 2 + 1);
	for (int i = 0; i < size; i++) {
		char byte = data[i];
		hex_data[2 * i] = nibble_to_hex((byte & 0xF0) >> 4);
		hex_data[2 * i + 1] = nibble_to_hex((byte & 0xF));
	}
	return hex_data;
}

static uint8_t *from_hex(char *hex_data, int hex_size)
{
	uint8_t *data;
	int size;
	assert(hex_size % 2 == 0);
	size = hex_size / 2;
	data = calloc(1, size);
	for (int i = 0; i < size; i++) {
		char byte = 0;
		byte |= hex_to_nibble(hex_data[2 * i]) << 4;
		byte |= hex_to_nibble(hex_data[2 * i + 1]);
		data[i] = byte;
	}
	return data;
}

#define BUILDID_SIZE 20

struct buildid_info {
	uint64_t data_offset;
	uint8_t *bytes;
	char *hex;
};

static void *fetch_data(int fd, uint64_t offset, size_t len)
{
	int rv;
	void *data;

	if (lseek(fd, offset, SEEK_SET) == (loff_t) -1) {
		fprintf(stderr, "error seeking to notes location 0x%lx\n", offset);
		perror("lseek");
		return NULL;
	}

	data = calloc(len, 1);
	if ((rv = read(fd, data, len)) != len) {
		fprintf(stderr, "error: read notes data failed (%d)\n", rv);
		if (rv < 0)
			perror("read");
		free(data);
		return NULL;
	}
	return data;
}

static int find_buildid(int fd, uint64_t offset, size_t len, struct buildid_info *info_out)
{
	void *data = fetch_data(fd, offset, len);
	struct elf_notehdr *nhdr;
	if (!data)
		return -1;

	nhdr = data;
	while (!end_notes(data, len, nhdr)) {
		if ((strcmp("GNU", nhdr->name) == 0) &&
		    (nhdr->type == NT_GNU_BUILD_ID)) {
			assert(nhdr->descsz == BUILDID_SIZE);

			size_t desc_offset_in_sect = (void *)note_desc(nhdr) - data;
			info_out->data_offset = offset + desc_offset_in_sect;
			info_out->bytes = malloc(BUILDID_SIZE);
			memcpy(info_out->bytes, note_desc(nhdr), BUILDID_SIZE);
			info_out->hex = to_hex(info_out->bytes, BUILDID_SIZE);
			free(data);
			return 1;
		}
		nhdr = next_note(nhdr);
	}
	free(data);
	return 0;
}

static int find_notes_phdr(Elf64_Ehdr *ehdr, void *entries, int start,
			   uint64_t *offset_out, uint64_t *size_out)
{
	for (int i = start; i < ehdr->e_phnum; i++) {
		/* Program header size may not match Elf64_Phdr, do it manually */
		Elf64_Phdr *phdr = entries + i * ehdr->e_phentsize;
		if (phdr->p_type != PT_NOTE)
			continue;

		*offset_out = phdr->p_offset;
		*size_out = phdr->p_filesz;
		return i;
	}
	return -1;
}

static int find_buildid_phdr(int fd, Elf64_Ehdr *ehdr, struct buildid_info *info_out)
{
	void *phdr;
	int start = 0;
	uint64_t offset, size;
	int rv;

	if (!ehdr->e_phnum) {
		fprintf(stderr, "ELF file has no program header\n");
		return 0;
	}
	phdr = fetch_data(fd, ehdr->e_phoff, ehdr->e_phnum * ehdr->e_phentsize);
	if (!phdr)
		return -1;

	while ((start = find_notes_phdr(ehdr, phdr, start, &offset, &size)) >= 0) {
		fprintf(stderr, "Found NOTES section in program header index %d\n", start);
		rv = find_buildid(fd, offset, size, info_out);

		/*
		 * Continue searching on 0 (not found). Otherwise, either we
		 * found it, or had an error. Either way, we should return.
		 */
		if (rv != 0)
			goto out;

		fprintf(stderr, "Build ID not present here, continuing...\n");
		start += 1; /* continue from next */
	}
	fprintf(stderr, "Program header did not contain NOTES segment with Build ID note.\n");

out:
	free(phdr);
	return rv;
}

static int find_notes_shdr(Elf64_Ehdr *ehdr, void *entries, int start,
			   uint64_t *offset_out, uint64_t *size_out)
{
	for (int i = start; i < ehdr->e_shnum; i++) {
		/* Program header size may not match Elf64_Shdr, do it manually */
		Elf64_Shdr *shdr = entries + i * ehdr->e_shentsize;
		if (shdr->sh_type != SHT_NOTE)
			continue;

		*offset_out = shdr->sh_offset;
		*size_out = shdr->sh_size;
		return i;
	}
	return -1;
}

static int find_buildid_shdr(int fd, Elf64_Ehdr *ehdr, struct buildid_info *info_out)
{
	void *shdr;
	int start = 0;
	uint64_t offset, size;
	int rv;

	if (!ehdr->e_shnum) {
		fprintf(stderr, "ELF file has no section header\n");
		return 0;
	}
	shdr = fetch_data(fd, ehdr->e_shoff, ehdr->e_shnum * ehdr->e_shentsize);
	if (!shdr)
		return -1;

	while ((start = find_notes_shdr(ehdr, shdr, start, &offset, &size)) >= 0) {
		fprintf(stderr, "Found NOTES section in section header index %d\n", start);
		rv = find_buildid(fd, offset, size, info_out);

		/*
		 * Continue searching on 0 (not found). Otherwise, either we
		 * found it, or had an error. Either way, we should return.
		 */
		if (rv != 0)
			goto out;

		fprintf(stderr, "Build ID not present here, continuing...\n");
		start += 1; /* continue from next */
	}
	fprintf(stderr, "Section header did not contain NOTES segment with Build ID note.\n");

out:
	free(shdr);
	return rv;
}

static int find_build_id(int fd, struct buildid_info *info_out)
{
	int rv;
	Elf64_Ehdr ehdr;

	if ((rv = read(fd, &ehdr, sizeof(ehdr))) != sizeof(ehdr)) {
		fprintf(stderr, "read ELF header failed (%d)\n", rv);
		if (rv < 0)
			perror("read");
		return -1;
	}

	if (!(ehdr.e_ident[0] == ELFMAG0 && ehdr.e_ident[1] == ELFMAG1 &&
	      ehdr.e_ident[2] == ELFMAG2 && ehdr.e_ident[3] == ELFMAG3)) {
		fprintf(stderr, "error: not an ELF file\n");
		printf("%d %d %d %d\n", ehdr.e_ident[0],ehdr.e_ident[1],ehdr.e_ident[2],ehdr.e_ident[3]);
		return -1;
	}
	/*
	 * Since we're going to be disregarding libelf later and directly
	 * editing the bits and bytes of the file, let's enforce our assumption
	 * that we're doing 64 bit, little endian, as is the standard of AMD64.
	 *
	 * Support for other formats is left as an exercise for the reader ;)
	 */
	if (ehdr.e_ident[EI_CLASS] != ELFCLASS64) {
		fprintf(stderr, "error: we only support ELF 64\n");
		return -1;
	}
	if (ehdr.e_ident[EI_DATA] != ELFDATA2LSB) {
		fprintf(stderr, "we only support little endian\n");
		return -1;
	}

	/*
	 * As far as I can tell, you can find a program header OR section header
	 * which specifies an ELF note - see PT_NOTE and SHT_NOTE respectively.
	 * I've only ever seen the build ID included in a section, which has the
	 * name .note.gnu.build-id.
	 *
	 * However, I originally wrote some of my ELF wrangling code for linux
	 * kernel core dumps, and those have a "vmcoreinfo" note inside a
	 * segment of type PT_NOTE in the program header - which is not
	 * mentioned in the section header.
	 *
	 * So I have code for both cases, though I doubt that they are needed.
	 */
	rv = find_buildid_phdr(fd, &ehdr, info_out);
	if (rv != 0)
		return rv;

	return find_buildid_shdr(fd, &ehdr, info_out);
}

static int write_new_buildid(int fd, size_t offset, uint8_t *data)
{
	if (lseek(fd, offset, SEEK_SET) < 0) {
		fprintf(stderr, "error: seeking to build id bytes\n");
		perror("lseek");
		return -1;
	}

	if (write(fd, data, BUILDID_SIZE) < 0) {
		perror("write");
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct buildid_info info;
	char *newid_hex;
	uint8_t *newid_bytes;
	int elf_fd, rv = 0;

	if (argc != 3) {
		fprintf(stderr, " usage : %s ELF-FILE BUILD-ID", argv[0]);
		return -1;
	}
	newid_hex = argv[2];
	if (strlen(newid_hex) != 40) {
		fprintf(stderr, "invalid build id\n");
		return -1;
	}
	newid_bytes = from_hex(newid_hex, 40);

	memset(&info, 0, sizeof(info));

	if ((elf_fd = open(argv[1], O_RDWR, 0)) < 0) {
		fprintf(stderr, "failed to open %s to read\n", argv[1]);
		perror("open");
		return 1;
	}
	rv = find_build_id(elf_fd, &info);
	if (rv < 0)
		goto out;
	if (rv == 0) {
		fprintf(stderr, "Sorry, couldn't find Build ID in that ELF file.\n");
		goto out;
	}
	printf("Found old build ID: %s\n", info.hex);
	if (write_new_buildid(elf_fd, info.data_offset, newid_bytes) < 0)
		goto out;
	printf("Wrote new build ID: %s\n", newid_hex);
out:
	free(newid_bytes);
	free(info.bytes);
	free(info.hex);
	close(elf_fd);
	return rv;
}
