/*
 * encode_vobs.c - Simple job scheduler to encode vob dvd rips
 *
 * Copyright (C) 2012 - 2013	Andrew Clayton <andrew@digital-domain.net>
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
		struct tm *tm = localtime(&t); \
		printf("%02d:%02d:%02d: " fmt, \
			tm->tm_hour, tm->tm_min, tm->tm_sec, ##__VA_ARGS__); \
	} while (0)

#define NR_WORKERS	nr_workers
#define PROCESS_EXITED	-2

#define CREATE_THEORA(infile, outfile) \
	do { \
		execlp("ffmpeg2theora", "ffmpeg2theora", "-o", outfile, \
				"--no-skeleton", "-v", "7", "-a", "3", infile, \
				(char *)NULL); \
	} while (0)
#define CREATE_WEBM(infile, outfile) \
	do { \
		execlp("ffmpeg", "ffmpeg", "-i", infile, \
				"-filter:v", "yadif", "-b:v", "1200k", "-ab", \
				"112k", outfile, (char *)NULL); \
	} while (0)

static int files_to_process;
static int files_processed;
static int nr_workers;

struct processing {
	pid_t pid;
	char file[PATH_MAX];
};
static struct processing *processing;

static void disp_usage(void)
{
	fprintf(stderr, "Usage: encode_vobs -P <theora|webm> [-t tasks] file1 "
			" ...\n");
	exit(EXIT_FAILURE);
}

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

static void process_file(const char *file, const char *profile)
{
	pid_t pid;
	int fd;
	int i;
	int ret;
	char outfile[PATH_MAX] = "\0";
	struct stat st;

	strncpy(outfile, file, strlen(file) - 3);
	if (strcmp(profile, "theora") == 0)
		strcat(outfile, "ogv");
	else
		strcat(outfile, "webm");

	ret = stat(outfile, &st);
	if (ret == 0) {
		loginfo("File %s exists, skipping\n", outfile);
		return;
	}
	loginfo("Processing : %s -> %s\n", file, outfile);

	fd = open("/dev/null", O_RDONLY);
	pid = fork();
	if (pid == 0) { /* child */
		setpriority(PRIO_PROCESS, 0, 10);
		/* Send stderr to /dev/null */
		dup2(fd, STDERR_FILENO);
		if (strcmp(profile, "theora") == 0)
			CREATE_THEORA(file, outfile);
		else
			CREATE_WEBM(file, outfile);
	}
	close(fd);

	for (i = 0; i < NR_WORKERS; i++) {
		if (processing[i].pid == -1) {
			processing[i].pid = pid;
			strcpy(processing[i].file, outfile);
			break;
		}
	}
}

int main(int argc, char **argv)
{
	int i;
	int opt;
	int files_in_progress = 0;
	struct sigaction sa;
	const char *profile = '\0';

	while ((opt = getopt(argc, argv, "P:ht:")) != -1) {
		switch (opt) {
		case 'P':
			if (strcmp(optarg, "theora") != 0 &&
			    strcmp(optarg, "webm") != 0)
				disp_usage();
			else
				profile = optarg;
			break;
		case 'h':
			disp_usage();
			break;
		case 't':
			nr_workers = atoi(optarg);
			break;
		}
	}
	if (optind >= argc)
		disp_usage();

	loginfo("Using profile: %s\n", profile);

	sigemptyset(&sa.sa_mask);
	sa.sa_handler = reaper;
	sa.sa_flags = SA_RESTART | SA_NODEFER;
	sigaction(SIGCHLD, &sa, NULL);

	if (nr_workers == 0)
		nr_workers = get_nprocs() - 1;
	if (nr_workers == 0)
		nr_workers = 1;
	loginfo("Using %d cores\n", NR_WORKERS);
	processing = calloc(NR_WORKERS, sizeof(struct processing));
	for (i = 0; i < NR_WORKERS; i++) {
                processing[i].pid = -1;
                processing[i].file[0] = '\0';
        }

	files_to_process = argc - optind;
	i = optind;
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
		process_file(argv[i], profile);
		i++;
		files_in_progress++;
	}

	free(processing);
	exit(EXIT_SUCCESS);
}
