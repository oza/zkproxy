/*
 * zkproxy
 *
 * Copyright (C) 2011 OZAWA Tsuyoshi
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <limits.h>

#include "event.h"
#include "net.h"
#include "work.h"
#include "logger.h"
#include "util.h"

#include "zkproxy.h"

#define MAX_EVENT_SIZE 4096
#define ZKPROXY_DEFAULT_PORT 9091
#define LOG_DEFAULT_DIR "/tmp/zkproxy.log"

static char program_name[] = "zkproxy";
/* FIXME: default value for testing */
int keepidle = 1;
int keepintvl = 1;
int keepcnt = 2;

static struct option const long_options[] = {
	/* common options */
	{"port", required_argument, NULL, 'p'},
	{"foreground", no_argument, NULL, 'f'},
	{"debug", no_argument, NULL, 'd'},
	{"help", no_argument, NULL, 'h'},

	/* keepalive-related options */
	{"keeptimeout", required_argument, NULL, 't'},
	{"keepintvl", required_argument, NULL, 'i'},
	{"keepcnt", required_argument, NULL, 'c'},

	{NULL, 0, NULL, 0},
};

static const char *short_options = "p:fl:dhmt:i:c:";

static void usage(int status)
{
	if (status)
		fprintf(stderr, "Try `%s --help' for more information.\n",
			program_name);
	else {
		printf("Usage: %s [OPTION] [PATH]\n", program_name);
		printf("\
Accord Daemon\n\
  -p, --port              specify the listen port number\n\
  -f, --foreground        make the program run in the foreground\n\
  -d, --debug             print debug messages\n\
  -t, --keeptimeout       specify idle time of sending keepalive packet to\
  detect client failure\n\
  -i, --keeptintvl        specify the period of sending keepalive packet to\
  detect client failure\n\
  -c, --keepcnt           specify the count of retrying to send packet.\
  timeout value is : keeptimeout + keepintvl * keepcnt.\n\
  -h, --help              display this help and exit\n\
");
	}
	exit(status);
}

int main(int argc, char *argv[])
{
	int ch, longindex;
	unsigned short port = ZKPROXY_DEFAULT_PORT;
	char *dir = LOG_DEFAULT_DIR;
	int is_daemon = 1, is_debug = 0;
	char logfile[PATH_MAX];

	signal(SIGPIPE, SIG_IGN);

	while ((ch = getopt_long(argc, argv, short_options, long_options,
				 &longindex)) >= 0) {
		switch (ch) {
		case 'p':
			port = atoi(optarg);
			break;
		case 'f':
			is_daemon = 0;
			break;
		case 'd':
			is_debug = 1;
			break;
		case 't':
			keepidle = atoi(optarg);
			break;
		case 'i':
			keepintvl = atoi(optarg);
			break;
		case 'c':
			keepcnt = atoi(optarg);
			break;
		case 'h':
			usage(0);
			break;
		default:
			usage(1);
			break;
		}
	}

	is_debug = 1;
	/* FIXME: pass warning */
	is_daemon = is_daemon;

	/*
	if (optind != argc)
		dir = argv[optind];
	*/

	/*
	if (is_daemon && daemon(0, 0))
		exit(1);
		*/

	strncpy(logfile, dir, sizeof(logfile));
	strncat(logfile, "/proxy.log", sizeof(logfile) - strlen(logfile) - 1);
	if (log_init(program_name, is_debug, logfile))
		exit(1);

	dprintf("start to init accord.\n");
	if (init_event(MAX_EVENT_SIZE) < 0) {
		eprintf("failed to epoll.\n");
		exit(1);
	}

	dprintf("create listen port.\n");
	if (create_listen_port(port, NULL)) {
		eprintf("failed to listen.\n");
		exit(1);
	}

	event_loop(-1);

	dprintf("exit.\n");
	return 0;
}
