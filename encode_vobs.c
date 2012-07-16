/*
 * encode_vobs.c - Simple job scheduler to encode vob dvd rips
 *
 * Copyright (C) 2012	Andrew Clayton <andrew@digital-domain.net>
 *
 * Released under the GNU General Public License version 2
 * See COPYING
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <signal.h>
#include <limits.h>

#define loginfo(fmt, ...) \
	do { \
		time_t t = time(NULL); \
		struct tm *tm; \
		char timestamp[10]; \
		tm = localtime(&t); \
		strftime(timestamp, sizeof(timestamp), "%H:%M:%S", tm); \
		printf("%s: " fmt, timestamp, ##__VA_ARGS__); \
	} while (0)

#define NR_WORKERS	nr_workers
#define PROCESS_EXITED	-2

static int files_to_process;
static int files_processed;
static int nr_workers;

struct processing {
	pid_t pid;
	char file[NAME_MAX + 1];
};
struct processing *processing;

static void reaper(int signo)
{
	int i;
	pid_t pid;

	while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
		/* Catch multiple children exiting at the same time */
		for (i = 0; i < NR_WORKERS; i++) {
			if (processing[i].pid == pid) {
				processing[i].pid = PROCESS_EXITED;
				break;
			}
		}
	}
}

static void do_processed(void)
{
	int i;

	for (i = 0; i < NR_WORKERS; i++) {
		if (processing[i].pid == PROCESS_EXITED) {
			loginfo("Finished   : %s\n", processing[i].file);
			processing[i].pid = -1;
			processing[i].file[0] = '\0';
			files_processed++;
		}
	}
}

static void process_file(const char *file)
{
	pid_t pid;
	int fd;
	int i;
	int ret;
	char ogg[NAME_MAX + 1] = "\0";
	struct stat st;

	strncpy(ogg, file, strlen(file) - 3);
	strcat(ogg, "ogv");

	ret = stat(ogg, &st);
	if (ret == 0) {
		loginfo("File %s exists, skipping\n", ogg);
		return;
	}
	loginfo("Processing : %s -> %s\n", file, ogg);

	fd = open("/dev/null", O_RDONLY);
	pid = fork();
	if (pid == 0) { /* child */
		setpriority(PRIO_PROCESS, 0, 10);
		/* Send stderr to /dev/null */
		dup2(fd, STDERR_FILENO);
		execlp("ffmpeg2theora", "ffmpeg2theora", "-o", ogg,
		       "--no-skeleton", "-v", "7", "-a", "3", file,
		       (char *)NULL);
	}
	close(fd);

	for (i = 0; i < NR_WORKERS; i++) {
		if (processing[i].pid == -1) {
			processing[i].pid = pid;
			strcpy(processing[i].file, ogg);
			break;
		}
	}
}

int main(int argc, char **argv)
{
	int i;
	int files_in_progress = 0;
	struct sigaction sa;
	
	if (argc < 2)
		exit(EXIT_FAILURE);

	sigemptyset(&sa.sa_mask);
	sa.sa_handler = reaper;
	sa.sa_flags = SA_RESTART | SA_NODEFER;
	sigaction(SIGCHLD, &sa, NULL);

	nr_workers = get_nprocs() - 1;
	if (nr_workers == 0)
		nr_workers = 1;
	loginfo("Using %d cores\n", NR_WORKERS);
	processing = calloc(NR_WORKERS, sizeof(struct processing));
	for (i = 0; i < NR_WORKERS; i++) {
                processing[i].pid = -1;
                processing[i].file[0] = '\0';
        }

	files_to_process = argc - 1;
	i = 1; /* argv[1] */
	for (;;) {
		if (i == argc) {
			while (files_processed < files_to_process) {
				pause();
				do_processed();
			}
			break;
		} else if (files_in_progress == NR_WORKERS) {
			pause();
			files_in_progress--;
			do_processed();
		}
		process_file(argv[i]);
		i++;
		files_in_progress++;
	}

	free(processing);
	exit(EXIT_SUCCESS);
}
