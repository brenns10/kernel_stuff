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
#include <signal.h>
#include <stdbool.h>

#define nelem(arr) (sizeof(arr) / sizeof(arr[0]))

bool loop;
bool exiting;

typedef int (*work_op_t)(int, const char *);

struct work {
	struct work *next;
	const char *path;
	const char *pfx;
	work_op_t op;
	unsigned long start;
	unsigned long stop;
	unsigned long cur;
	unsigned long cnt;
	pthread_t thread;
	int error;
};

static int do_stat(int dirfd, const char *filename)
{
	struct stat statbuf;
	if (fstatat(dirfd, filename, &statbuf, 0) == -1 && errno != ENOENT) {
		perror("fstatat");
		return -1;
	}
	return 0;
}

static int do_open(int dirfd, const char *filename)
{
	int fd = openat(dirfd, filename, O_RDONLY);
	if (fd < 0) {
		perror("openat");
		return -1;
	}
	if (close(fd) != 0) {
		perror("close");
		return -1;
	}
	return 0;
}

static int do_create(int dirfd, const char *filename)
{
	int fd = openat(dirfd, filename, O_WRONLY | O_CREAT, 0644);
	if (fd < 0) {
		perror("openat");
		return -1;
	}
	if (close(fd) != 0) {
		perror("close");
		return -1;
	}
	return 0;
}

static int do_unlink(int dirfd, const char *filename)
{
	int rv = unlinkat(dirfd, filename, 0);
	if (rv < 0) {
		perror("openat");
		return -1;
	}
	return 0;
}

struct { char *name; work_op_t op; } OPERATIONS[] = {
	{ "stat", do_stat },
	{ "open", do_open },
	{ "create", do_create },
	{ "unlink", do_unlink },
};

static void *stat_worker(void *varg)
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

	do {
		for (arg->cur = arg->start; arg->cur < arg->stop && !exiting; arg->cur++) {
			snprintf(filename, sizeof(filename), "%s%010lu", arg->pfx, arg->cur);
			if (arg->op(dirfd, filename) == -1) {
				close(dirfd);
				arg->error = 1;
				return NULL;
			}
		}
		if (loop)
			arg->cnt++;
	} while (loop && !exiting);
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
	printf("  -P, --pfx  <STR>   name dentries with STR + 10-digit number\n");
	printf("                     (default is \"file-\")\n");
	printf("  -o, --op <STR>     operation (choices: stat, open, create, unlink)\n");
	printf("  -l, --loop         loop continuously, re-accessing\n");
	printf("  -h, --help         show this message and exit.\n");
	exit(EXIT_SUCCESS);
}

void interrupt(int signal)
{
	exiting = true;
}

int main(int argc, char **argv)
{
	int nthread = 1, i, err;
	unsigned long count = 1000, increment, progress;
	struct work *first = NULL, *cur;
	char *path = get_current_dir_name();
	char *pfx = "file-";
	int opt;
	const char *shopt = "t:c:p:P:o:hl";
	sigset_t set;
	struct sigaction sa = {0};
	work_op_t op = &do_stat;

	/* Block SIGINT so that threads inherit this. */
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	err = pthread_sigmask(SIG_BLOCK, &set, NULL);
	if (err != 0) {
		errno = err;
		perror("pthread_sigmask");
		exit(1);
	}

	/* Setup our SIGINT handler for later. */
	sa.sa_handler = interrupt;
	err = sigaction(SIGINT, &sa, NULL);
	if (err != 0) {
		perror("sigaction");
		exit(1);
	}

	static struct option lopt[] = {
		{"--threads", required_argument, NULL, 't'},
		{"--count",   required_argument, NULL, 'c'},
		{"--path",    required_argument, NULL, 'p'},
		{"--help",    no_argument,       NULL, 'h'},
		{"--loop",    no_argument,       NULL, 'l'},
		{"--op",      required_argument, NULL, 'o'},
		{"--prefix",  required_argument, NULL, 'P'},
	};
	while ((opt = getopt_long(argc, argv, shopt, lopt, NULL)) != -1) {
		switch (opt) {
			case 'h':
				help();
				break;
			case 'l':
				loop = true;
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
			case 'o':
				for (i = 0; i < nelem(OPERATIONS); i++) {
					if (strcmp(OPERATIONS[i].name, optarg) == 0) {
						op = OPERATIONS[i].op;
						break;
					}
				}
				if (i >= nelem(OPERATIONS)) {
					fprintf(stderr, "--op %s : operation unknown\n", optarg);
					exit(EXIT_FAILURE);
				}
				break;
			case 'P':
				pfx = optarg;
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
		cur->cnt = 0;
		cur->cur = cur->start;
		cur->pfx = pfx;
		cur->op = op;
		pthread_create(&cur->thread, NULL, stat_worker, cur);
	}

	/* Unblock SIGINT now that threads are created */
	err = pthread_sigmask(SIG_UNBLOCK, &set, NULL);

	do {
		struct timespec tv;
		progress = 0;
		err = 0;
		for (cur = first; cur; cur = cur->next) {
			progress += cur->cur - cur->start;
			progress += cur->cnt * (cur->stop - cur->start);
			err += cur->error;
		}
		printf("progress: %10lu/%10lu\n", progress, count);
		tv.tv_nsec = 100 * 1000 * 1000; /* 100ms */
		tv.tv_sec = 0;
		nanosleep(&tv, NULL);
	} while ((progress < count || loop) && err == 0 && !exiting);

	if (err) {
		fprintf(stderr, "error detected! canceling threads\n");
		for (cur = first; cur; cur = cur->next)
			if (!cur->error)
				pthread_cancel(cur->thread);
		err = EXIT_FAILURE;
	} else if (exiting) {
		fprintf(stderr, "interrupted! waiting on threads\n");
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
