#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc != 2) {
		printf("usage: openclose FILE\n");
		return 1;
	}
	while (1) {
		int fd = open(argv[1], O_RDONLY);
		if (fd < 0)
			return 1;
		close(fd);
	}
	return 0;
}
