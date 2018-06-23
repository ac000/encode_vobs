/*
 * encode_vobs.c - Simple job scheduler to encode vob dvd rips
 *
 * Copyright (C) 	2012 - 2015, 2017 - 2018
 * 			Andrew Clayton <andrew@digital-domain.net>
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
#include <termios.h>
#include <limits.h>

#define loginfo(fmt, ...) \
	do { \
		time_t t = time(NULL); \
		struct tm *tm = localtime(&t); \
		printf("%02d:%02d:%02d: " fmt, \
			tm->tm_hour, tm->tm_min, tm->tm_sec, ##__VA_ARGS__); \
	} while (0)

#define PROCESS_EXITED	-2

#define NR_WORKERS	nr_workers
#define ENC_NICE	enc_nice
#define POST_CMD	post_cmd

#define TXT_FMT_BOLD	"\033[1m"
#define TXT_FMT_END	"\033[0m"

#define LOG_MSG_FIN	TXT_FMT_BOLD"Finished   : "TXT_FMT_END"%s\n"

enum { FMT_NONE = 0, FMT_WEBM, FMT_MKV, FMT_CUSTOM };

static const char *profiles[] = { "None", "WebM", "MKV", "Custom" };

static volatile sig_atomic_t files_in_progress;
static volatile sig_atomic_t files_processed;
static volatile sig_atomic_t file_processed;

static int enc_nice = 10;
static int nr_workers;
static char *custom_encoder_cmd;
static char *post_cmd;
static char *audio_track_id = "1";

struct processing {
	pid_t pid;
	char file[PATH_MAX];
};
static struct processing *processing;

static void create_custom(char *cmd, const char *infile, const char *outfile)
{
	char *args[128];
	char *tok;
	char **next = args;

	tok = strtok(cmd, " ");
	while (tok) {
		if (strcmp(tok, "%i") == 0)
			*next++ = (char *)infile;
		else if (strncmp(tok, "%o", 2) == 0)
			*next++ = (char *)outfile;
		else
			*next++ = tok;
		tok = strtok(NULL, " ");
	}
	*next = (char *)NULL;

	execvp(args[0], args);
}

static void create_webm(const char *infile, const char *outfile)
{
	execlp("ffmpeg", "ffmpeg",
			 "-i", infile,
			 "-speed", "3",
			 "-filter:v", "yadif",
			 "-crf", "10",
			 "-c:v", "libvpx",
			 "-b:v", "1200k",
			 "-b:a", "160k",
			 outfile, (char *)NULL);
}

static void create_mkv(const char *infile, const char *outfile)
{
	pid_t pid;
	char webm[PATH_MAX];

	snprintf(webm, strlen(outfile) - 3, "%s", outfile);
	if (strlen(webm) > PATH_MAX - 6 /* .webm + \0 */) {
		loginfo("ERROR: Filename '%s' + '.webm' too long.\n", webm);
		_exit(EXIT_FAILURE);
	}
	strcat(webm, ".webm");

	/*
	 * We need to run the WebM encode in a child process so we can then
	 * carry on and do the mkvmerge afterwards.
	 */
	pid = fork();
	if (pid == 0)
		create_webm(infile, webm);
	pause();

	loginfo("Doing mkvmerge (%s)\n", outfile);
	execlp("mkvmerge", "mkvmerge", "-q", "-o", outfile, "-A", webm, "-D",
			"-a", audio_track_id, infile, (char *)NULL);
}

static void disp_usage(void)
{
	fprintf(stderr, "Usage: encode_vobs -P <webm|mkv|custom> "
			"[-c custom encoding command]\n"
			"                   [-a audio track ID] [-t tasks] "
		        "[-n nice]\n                   "
			"[-e post_process_exec_command] <file1 ...>\n\n");
	fprintf(stderr, "tasks is how many files to process at a time. It "
			"defaults to nr cores - 1.\n\n"
			"nice is the priority to run the encode processes "
			"at. It should be a value\nbetween 0 and 19. It "
			"defaults to 10.\n\n"
			"post_process_exec_command is the full path to an "
			"executable that will be\ncalled after each processed "
			"file, it will be passed the full path of the\n"
			"newly processed file as argv[1].\n\n"
			"audio track ID is the audio track to use from the "
			"VOB (only for MKV\nprofile). Defaults to 1.\n\n"
			"The custom profile expects a -c argument specifying "
			"the encode command to\nuse. It takes two place "
			"holders. %%i for the input file and %%o for the "
			"output\nfile. e.g -c \"ffmpeg -i %%i %%o.webm\"\n");
	exit(EXIT_FAILURE);
}

static void reaper(__attribute__ ((unused)) int signo)
{
	int i;
	pid_t pid;

	while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
		/* Catch multiple children exiting at the same time */
		for (i = 0; i < NR_WORKERS; i++) {
			if (processing[i].pid == pid) {
				processing[i].pid = PROCESS_EXITED;
				files_in_progress--;
				files_processed++;
				file_processed = 1;
				break;
			}
		}
	}
}

static void do_post_cmd(const char *file)
{
	pid_t pid;

	pid = fork();
	if (pid == 0)
		execl(POST_CMD, POST_CMD, file, (char *)NULL);
}

static void do_processed(void)
{
	int i;

	for (i = 0; i < NR_WORKERS; i++) {
		if (processing[i].pid == PROCESS_EXITED) {
			loginfo(LOG_MSG_FIN, processing[i].file);
			if (POST_CMD)
				do_post_cmd(processing[i].file);

			processing[i].pid = -1;
			processing[i].file[0] = '\0';
		}
	}
	file_processed = 0;
}

static void process_file(const char *file, int profile)
{
	pid_t pid;
	int fd;
	int i;
	int ret;
	char outfile[PATH_MAX] = "\0";
	struct stat st;

	snprintf(outfile, sizeof(outfile), "%s", file);
	outfile[strlen(outfile) - 3] = '\0';
	if (profile == FMT_WEBM) {
		strcat(outfile, "webm");
	} else if (profile == FMT_MKV) {
		strcat(outfile, "mkv");
	} else {
		char *ptr = strstr(custom_encoder_cmd, "%o");
		size_t len = strlen(outfile);

		ptr += 3;
		while (*ptr != ' ' && *ptr != '\0') {
			if (len + 1 == PATH_MAX) {
				loginfo("ERROR: Filename '%s' too long.\n",
						outfile);
				loginfo("Skipping   : %s\n", file);
				return;
			}

			outfile[len++] = *(ptr++);
		}
	}

	ret = stat(outfile, &st);
	if (ret == 0) {
		loginfo("File %s exists, skipping\n", outfile);
		files_processed++;
		files_in_progress--;
		return;
	}
	loginfo("Processing : %s -> %s\n", file, outfile);

	fd = open("/dev/null", O_RDONLY);
	pid = fork();
	if (pid == 0) { /* child */
		setpriority(PRIO_PROCESS, 0, ENC_NICE);
		/* Send stderr to /dev/null */
		dup2(fd, STDERR_FILENO);
		if (profile == FMT_WEBM) {
			create_webm(file, outfile);
		} else if (profile == FMT_MKV){
			create_mkv(file, outfile);
		} else {
			char *cmd = strdup(custom_encoder_cmd);

			create_custom(cmd, file, outfile);
		}
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
	int files_to_process;
	struct sigaction sa;
	struct termios tp;
	int profile = FMT_NONE;

	while ((opt = getopt(argc, argv, "a:e:P:c:hn:t:")) != -1) {
		switch (opt) {
		case 'a':
			audio_track_id = optarg;
			break;
		case 'e': {
			struct stat st;
			int err;

			err = stat(optarg, &st);
			if (err) {
				fprintf(stderr, "Cannot stat %s\n", optarg);
				exit(EXIT_FAILURE);
			}
			post_cmd = optarg;
			break;
		}
		case 'P':
			if (strcmp(optarg, "webm") == 0)
				profile = FMT_WEBM;
			else if (strcmp(optarg, "mkv") == 0)
				profile = FMT_MKV;
			else if (strcmp(optarg, "custom") == 0)
				profile = FMT_CUSTOM;
			else
				disp_usage();
			break;
		case 'c':
			if (profile == FMT_CUSTOM)
				custom_encoder_cmd = optarg;
			break;
		case 'h':
			disp_usage();
			break;
		case 'n':
			enc_nice = atoi(optarg);
			if (enc_nice < 0 || enc_nice > 19)
				disp_usage();
			break;
		case 't':
			nr_workers = atoi(optarg);
			break;
		default:
			disp_usage();
		}
	}
	if (optind >= argc || profile == FMT_NONE ||
	    (profile == FMT_CUSTOM && !custom_encoder_cmd))
		disp_usage();

	tcgetattr(STDIN_FILENO, &tp);

	loginfo("Using profile: %s\n", profiles[profile]);
	if (profile == FMT_CUSTOM)
		loginfo("Using custom encode cmd: %s\n", custom_encoder_cmd);

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
	while (files_processed < files_to_process) {
		if (files_in_progress < NR_WORKERS && i < argc) {
			process_file(argv[i], profile);
			files_in_progress++;
			i++;
		} else {
			pause();
		}
		if (file_processed)
			do_processed();
	}

	free(processing);
	tp.c_lflag |= ECHO;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &tp);

	exit(EXIT_SUCCESS);
}
