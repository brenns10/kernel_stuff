/*
 * Edit the "VMCOREINFO" note of an ELF file.
 *
 * This sounds like a simple task, but actually it's not. ELF files have two
 * different units of organization - sections, and segments. It seems like most
 * libraries care about managing sections, and the program header table is
 * derived after the fact. libelf has absolutely no way to edit the program
 * header table while preserving the contents of the file. It can only manage
 * sections.
 *
 * A vmcore (and probably other core dumps) is an ELF file with no sections! It
 * has segments but as I said, libelf has no real capacity to edit a section.
 *
 * So here is what we do. Use libelf's header declarations to make our life a
 * bit easier. But don't actually link to or use libelf - just "parse" the file
 * manually:
 *
 * 1. Verify we have an ELF file, 64-bit, little endian
 * 2. Find the VMCOREINFO note
 * 3. Read the new contents of the VMCOREINFO note
 * 4. Adjust the offsets of all the other program headers for the new size
 * 5. Seek to the VMCOREINFO note. Send the remaining data to a temp file.
 * 6. Truncate the file and write the new note.
 * 7. Send the data from the old file back to this one.
 * 8. Done!
 */
#include <err.h>
#include <fcntl.h>
#include <gelf.h>
#include <libelf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sysexits.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <sys/sendfile.h>

struct elf_notehdr {
	uint32_t namesz;
	uint32_t descsz;
	uint32_t type;
	char name[0];
};

struct saved_elfdata {
	Elf64_Ehdr ehdr;
	Elf64_Phdr *phdr;
	char *notes;
	struct elf_notehdr *vi_note;

	/* offset into the file for the notes section, and size */
	size_t notes_start;
	size_t notes_len;
	/* segment index for the notes section*/
	int notes_seg;

	/* offset into the notes section for the actual vmcoreinfo note */
	size_t vi_note_start;
	size_t vi_note_end;

	/* old descriptor size */
	size_t old_descsz;
};

size_t pad4(size_t val)
{
	if (val & 3)
		return (val & (~(size_t)3)) + 4;
	return val;
}

char *note_desc(struct elf_notehdr *nhdr)
{
	return &nhdr->name[pad4(nhdr->namesz)];
}

struct elf_notehdr *next_note(struct elf_notehdr *nhdr)
{
	return (struct elf_notehdr *)&nhdr->name[
		pad4(nhdr->namesz) + pad4(nhdr->descsz)];
}

bool end_notes(void *start, size_t len, void *ptr)
{
	return ((uintptr_t)ptr - (uintptr_t)start) >= len;
}

/*
 * Read a file containing new vmcoreinfo and return its length (including a
 * newly added null terminator). On error, return negative. On success, *res
 * is set to a pointer to the data.
 */
ssize_t read_newdata(char *filename, char **res)
{
	size_t len;
	char *data;
	ssize_t rv = -1;
	FILE *f = fopen(filename, "r");
	if (!f) {
		perror("open");
		return -1;
	}
	if (fseek(f, 0, SEEK_END) < 0) {
		perror("fseek");
		goto out_close;
	}
	len = ftell(f);
	if (fseek(f, 0, SEEK_SET) < 0) {
		perror("fseek");
		goto out_close;
	}
	data = malloc(len + 1);
	if (!data) {
		fprintf(stderr, "error: out of memory\n");
		goto out_close;
	}
	if (fread(data, 1, len, f) != len) {
		perror("fread");
		goto out_free;
	}
	data[len] = '\0';
	*res = data;
	return len + 1;
out_free:
	free(data);
out_close:
	fclose(f);
	return rv;
}

int read_elfdata(int fd, struct saved_elfdata *se)
{
	int i, rv;
	size_t n, sz;
	Elf64_Ehdr ehdr;
	Elf64_Phdr *phdr = NULL;
	struct elf_notehdr *nhdr = NULL;
	char *notes = NULL;
	bool notefound = false;

	if ((rv = read(fd, &ehdr, sizeof(ehdr))) != sizeof(ehdr)) {
		fprintf(stderr, "read ELF header failed (%d)\n", rv);
		if (rv < 0)
			perror("read");
		goto err;
	}
	se->ehdr = ehdr;

	if (!(ehdr.e_ident[0] == ELFMAG0 && ehdr.e_ident[1] == ELFMAG1 &&
	      ehdr.e_ident[2] == ELFMAG2 && ehdr.e_ident[3] == ELFMAG3)) {
		fprintf(stderr, "error: not an ELF file\n");
		printf("%d %d %d %d\n", ehdr.e_ident[0],ehdr.e_ident[1],ehdr.e_ident[2],ehdr.e_ident[3]);
		goto err;
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
		goto err;
	}
	if (ehdr.e_ident[EI_DATA] != ELFDATA2LSB) {
		fprintf(stderr, "we only support little endian\n");
		goto err;
	}

	if (ehdr.e_shentsize) {
		fprintf(stderr, "error: ELF has section header, not supported\n");
		goto err;
	}

	n = ehdr.e_phnum;
	assert(sizeof(*phdr) == ehdr.e_phentsize);
	phdr = calloc(ehdr.e_phentsize, ehdr.e_phnum);
	se->phdr = phdr;
	sz = ehdr.e_phentsize * ehdr.e_phnum;
	if (lseek(fd, ehdr.e_phoff, SEEK_SET) == (loff_t) -1) {
		fprintf(stderr, "error seeking to program header\n");
		perror("lseek");
		goto err;
	}
	if ((rv = read(fd, phdr, sz)) != sz) {
		fprintf(stderr, "error: read program header failed (%d)\n", rv);
		if (rv < 0)
			perror("read");
		goto err;
	}

	for (i = 0; i < n; i++) {
		if (phdr[i].p_type != PT_NOTE)
			continue;

		printf("FOUND NOTE SECTION at i=%d\n", i);
		se->notes_seg = i;
		se->notes_start = phdr[i].p_offset;
		se->notes_len = phdr[i].p_filesz;
		break;
	}
	if (i == n) {
		printf("Did not find notes segment, failing\n");
		goto err;
	}

	notes = malloc(se->notes_len);
	if (lseek(fd, se->notes_start, SEEK_SET) < 0) {
		fprintf(stderr, "error: seeking to notes segment\n");
		perror("lseek");
		goto err;
	}
	if ((rv = read(fd, notes, se->notes_len)) != se->notes_len) {
		fprintf(stderr, "error reading notes segment (%d)\n", rv);
		if (rv < 0)
			perror("read");
		goto err;
	}
	nhdr = (struct elf_notehdr *)notes;
	while (!end_notes(notes, se->notes_len, nhdr)) {
		if ((strcmp("VMCOREINFO", nhdr->name) == 0) &&
		    (nhdr->type == 0)) {
			size_t offset = (uintptr_t)nhdr - (uintptr_t)notes;
			printf("Found VMCOREINFO note at offset 0x%lx, with descsz=0x%x\n", offset, nhdr->descsz);
			se->vi_note_start = offset;
			se->vi_note_end = offset + sizeof(struct elf_notehdr) + pad4(nhdr->descsz) + pad4(nhdr->namesz);
			se->old_descsz = nhdr->descsz;
			se->vi_note = nhdr;
			notefound = true;
		}
		nhdr = next_note(nhdr);
	}
	if (!notefound) {
		printf("Did not find VMCOREINFO in the notes, failing\n");
		goto err;
	}
	return 0;

err:
	free(notes);
	free(phdr);
	return -1;
}

int update_offsets(int fd, struct saved_elfdata *se, size_t newdescsz)
{
	int i, rv;
	size_t n, sz;
	Elf *e;
	Elf64_Phdr *phdr;
	ssize_t sizediff = (ssize_t)pad4(newdescsz) 
	                   - (ssize_t)pad4(se->old_descsz);
	printf("OLD DESCSZ: 0x%lx | 0x%lx\n", se->old_descsz, pad4(se->old_descsz));
	printf("NEW DESCSZ: 0x%lx | 0x%lx\n", newdescsz, pad4(newdescsz));
	printf("Difference: %ld\n", sizediff);

	phdr = se->phdr;
	n = se->ehdr.e_phnum;
	i = se->notes_seg;
	printf("Notes segment, old: filesz 0x%lx / memsz 0x%lx\n",
			phdr[i].p_filesz, phdr[i].p_memsz);
	phdr[i].p_filesz += sizediff;
	if (phdr[i].p_memsz)
		phdr[i].p_memsz += sizediff;
	printf("Notes segment, new: filesz 0x%lx / memsz 0x%lx\n",
			phdr[i].p_filesz, phdr[i].p_memsz);

	for (i = i + 1; i < n; i++) {
		printf("Segment %d: p_offset 0x%lx -> 0x%lx\n",
		       i, phdr[i].p_offset, phdr[i].p_offset + sizediff);
		phdr[i].p_offset += sizediff;
	}

	// write updated phdr
	sz = se->ehdr.e_phentsize * se->ehdr.e_phnum;
	if (lseek(fd, se->ehdr.e_phoff, SEEK_SET) == (off_t) -1) {
		fprintf(stderr, "error seeking to phdr\n");
		perror("lseek");
		goto err;
	}
	if ((rv = write(fd, se->phdr, sz)) != sz) {
		fprintf(stderr, "error writing updated phdr %d\n", rv);
		if (rv < 0)
			perror("write");
		goto err;
	}
	printf("Wrote updated phdr\n");
	return 0;

err:
	return -1;
}

int sendfile_loop(int dst_fd, int src_fd, size_t amt)
{
	size_t cur = 0;
	ssize_t rv;

	do {
		rv = sendfile(dst_fd, src_fd, NULL, amt - cur);
		if (rv < 0) {
			perror("sendfile");
			return -1;
		}
		cur += rv;
		printf("sendfile: completed %ld/%ld\n", cur, amt);
	} while (cur < amt);
	return 0;
}

int shift_data(int fd, struct saved_elfdata *se, size_t newdescsz, char *newdesc)
{
	char tmpname[] = "/tmp/vmcore.XXXXXX";
	int tmpfd;
	off_t length;
	struct elf_notehdr *newnote;
	size_t sz, savesz;
	off_t offset;

	sz = sizeof(*newnote) + pad4(se->vi_note->namesz) + pad4(newdescsz);
	newnote = malloc(sz);
	// init 0-pad
	memset(newnote, 0, sz);
	// copy old data
	memcpy(newnote, se->vi_note, sizeof(struct elf_notehdr) + pad4(se->vi_note->namesz));
	// set new desc
	newnote->descsz = newdescsz;
	memcpy(note_desc(newnote), newdesc, newdescsz);

	if ((length = lseek(fd, 0, SEEK_END)) == (off_t) -1) {
		fprintf(stderr, "error seeking to end\n");
		perror("lseek");
		return -1;
	}

	if (lseek(fd, se->notes_start + se->vi_note_end, SEEK_SET) < 0) {
		fprintf(stderr, "error seeking to vmcoreinfo note\n");
		perror("lseek");
		return -1;
	}
	tmpfd = mkstemp(tmpname);
	if (tmpfd < 0) {
		perror("mkstemp");
		return -1;
	}
	if (unlink(tmpname) < 0) {
		perror("unlink");
		goto err;
	}

	// save everything after the end of the old note
	offset = se->notes_start + se->vi_note_end;
	savesz = length - offset;
	printf("Sending %ld bytes at end of data to tempfile...\n", savesz);
	ssize_t rv;
	if (sendfile_loop(tmpfd, fd, savesz) < 0)
		goto err;

	// truncate file at current position (beginning of note)
	if (ftruncate(fd, se->notes_start + se->vi_note_start) < 0) {
		perror("ftruncate");
		goto err;
	}

	if (lseek(fd, 0, SEEK_END) == (off_t) -1) {
		perror("lseek");
		goto err;
	}
	if (lseek(tmpfd, 0, SEEK_SET) == (off_t) -1) {
		perror("lseek tmp");
		goto err;
	}
	if ((rv = write(fd, newnote, sz)) != sz) {
		fprintf(stderr, "error writing new note! %ld\n", rv);
		if (rv < 0)
			perror("write");
		goto err;
	}
	if (sendfile_loop(fd, tmpfd, savesz) < 0)
		goto err;
	close(tmpfd);
	return 0;

err:
	close(tmpfd);
	return -1;
}

int main(int argc, char **argv)
{
	struct saved_elfdata se;
	char *newdata;
	ssize_t newsize;
	int elf_fd;

	if (argc != 3) {
		fprintf(stderr, " usage : %s VMCORE VMCOREINFO", argv[0]);
		return -1;
	}

	memset(&se, 0, sizeof(se));

	if ((newsize = read_newdata(argv[2], &newdata)) < 0)
		return 1;
	if ((elf_fd = open(argv[1], O_RDWR, 0)) < 0) {
		fprintf(stderr, "failed to open %s to read\n", argv[1]);
		perror("open");
		return 1;
	}
	if (read_elfdata(elf_fd, &se) < 0)
		goto err;
	if (update_offsets(elf_fd, &se, newsize) < 0)
		goto err;
	if (shift_data(elf_fd, &se, newsize, newdata) < 0)
		goto err;
	close(elf_fd);
	printf("Success!\n");
	return 0;
err:
	close(elf_fd);
	return 1;
}
