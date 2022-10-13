/*
 * Tool for creating negative dentries.
 *
 * Time recordings on 48-core machine, by thread count, to do 1,000,000 per
 * thread. (UEK6-U2, OL7)
 *
 * #T    Wall       Sys  dps
 *  1   2.594     1.799  385k
 *  2   3.280     4.895  609k
 *  3   3.390     7.688  884k
 *  4   4.369    14.128  915k
 *  5   7.274    32.038  687k
 *  6   9.976    54.592  601k
 *  8  12.450  1:32.333  642k
 * 16  27.679  7:06.373  578k
 *
 * For 180 million: (UEK6-U2, OL7)
 *   - 4 threads: 3:49.353 wall = 784,816 dps
 *   - 3 threads: 4:02.281 wall = 742,939 dps
 */
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <getopt.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

struct work {
	struct work *next;
	const char *path;
	unsigned long start;
	unsigned long stop;
	unsigned long cur;
	pthread_t thread;
	int error;
};

void *stat_worker(void *varg)
{
	struct work *arg = (struct work *)varg;
	struct stat statbuf;
	char filename[64];
	int dirfd;


	dirfd = open(arg->path, O_DIRECTORY | O_PATH);
	if (dirfd == -1) {
		perror("open");
		arg->error = 1;
		return NULL;
	}

	for (arg->cur = arg->start; arg->cur < arg->stop; arg->cur++) {
		snprintf(filename, sizeof(filename), "file-%010lu", arg->cur);
		if (fstatat(dirfd, filename, &statbuf, 0) == -1 && errno != ENOENT) {
			perror("fstatat");
			close(dirfd);
			arg->error = 1;
			return NULL;
		}
	}
	close(dirfd);
	return NULL;
}

void help(void)
{
	printf("negdentcreate is a tool for creating negative dentries\n\n");
	printf("Currently, it can only create them by calling stat() on files which do\n");
	printf("not exist. However, it could be extended to allow creating dentries by\n");
	printf("creating files and then deleting them. This tool tries to be performant\n");
	printf("by allowing you to tweak the number of threads used. However, know that\n");
	printf("more threads is not necessarily better, as the cost of lock contention\n");
	printf("may outweigh the gains of parallelism.\n\n");
	printf("Options:\n");
	printf("  -t, --threads <N>  use N threads (default 1)\n");
	printf("  -c, --count <N>    create N negative dentries (default 1000)\n");
	printf("  -p, --path <PATH>  create negative dentries in PATH\n");
	printf("  -h, --help         show this message and exit.\n");
	exit(EXIT_SUCCESS);
}

int main(int argc, char **argv)
{
	int nthread, i, err;
	unsigned long count, increment, progress;
	struct work *first = NULL, *cur;
	char *path = get_current_dir_name();
	int opt;
	const char *shopt = "t:c:p:h";
	static struct option lopt[] = {
		{"--threads", required_argument, NULL, 't'},
		{"--count",   required_argument, NULL, 'c'},
		{"--path",    required_argument, NULL, 'p'},
		{"--help",    no_argument,       NULL, 'h'},
	};
	while ((opt = getopt_long(argc, argv, shopt, lopt, NULL)) != -1) {
		switch (opt) {
			case 'h':
				help();
				break;
			case 't':
				nthread = atoi(optarg);
				break;
			case 'c':
				count = atol(optarg);
				break;
			case 'p':
				free(path);
				path = strdup(optarg);
				break;
			default:
				fprintf(stderr, "Invalid parameters, try again with -h for help.\n");
				exit(EXIT_FAILURE);
		}
	}

	increment = count / nthread;
	count = increment * nthread; /* we only deal with round numbers... */
	for (i = 0; i < nthread; i++) {
		if (first) {
			cur->next = calloc(1, sizeof(struct work));
			cur = cur->next;
		} else {
			first = calloc(1, sizeof(struct work));
			cur = first;
		}
		cur->path = path;
		cur->start = i * increment;
		cur->stop = cur->start + increment;
		cur->cur = cur->start;
		pthread_create(&cur->thread, NULL, stat_worker, cur);
	}

	do {
		struct timespec tv;
		progress = 0;
		err = 0;
		for (cur = first; cur; cur = cur->next) {
			progress += cur->cur - cur->start;
			err += cur->error;
		}
		printf("progress: %10lu/%10lu\n", progress, count);
		tv.tv_nsec = 10 * 1000 * 1000; /* 10ms */
		tv.tv_sec = 0;
		nanosleep(&tv, NULL);
	} while (progress < count && err == 0);

	if (err) {
		fprintf(stderr, "error detected! canceling threads\n");
		for (cur = first; cur; cur = cur->next)
			if (!cur->error)
				pthread_cancel(cur->thread);
		err = EXIT_FAILURE;
	} else {
		fprintf(stderr, "done! waiting on threads\n");
		err = EXIT_SUCCESS;
	}

	cur = first;
	while (cur) {
		struct work *tmp = cur->next;
		pthread_join(cur->thread, NULL);
		free(cur);
		cur = tmp;
	}
	free(path);
	return err;
}
