// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal chrt replacement for the fpga-pci-bus tests.
 *
 *   rtprio get <pid>                     print "<policy> <priority>"
 *   rtprio set <pid> fifo|rr|other <prio>
 *   rtprio hog <prio> <seconds>          busy loop as SCHED_FIFO <prio>
 */
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *policy_name(int policy)
{
	switch (policy) {
	case SCHED_FIFO:
		return "SCHED_FIFO";
	case SCHED_RR:
		return "SCHED_RR";
	case SCHED_OTHER:
		return "SCHED_OTHER";
	default:
		return "other";
	}
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	struct sched_param sp = { 0 };
	int policy;
	pid_t pid;

	if (argc == 3 && !strcmp(argv[1], "get")) {
		pid = atoi(argv[2]);
		policy = sched_getscheduler(pid);
		if (policy < 0 || sched_getparam(pid, &sp)) {
			perror("rtprio get");
			return 1;
		}
		printf("%s %d\n", policy_name(policy), sp.sched_priority);
		return 0;
	}

	if (argc == 5 && !strcmp(argv[1], "set")) {
		pid = atoi(argv[2]);
		if (!strcmp(argv[3], "fifo"))
			policy = SCHED_FIFO;
		else if (!strcmp(argv[3], "rr"))
			policy = SCHED_RR;
		else
			policy = SCHED_OTHER;
		sp.sched_priority = atoi(argv[4]);
		if (sched_setscheduler(pid, policy, &sp)) {
			perror("rtprio set");
			return 1;
		}
		return 0;
	}

	if (argc == 4 && !strcmp(argv[1], "hog")) {
		double end = now() + atof(argv[3]);

		sp.sched_priority = atoi(argv[2]);
		if (sched_setscheduler(0, SCHED_FIFO, &sp)) {
			perror("rtprio hog");
			return 1;
		}
		while (now() < end)
			;
		return 0;
	}

	fprintf(stderr, "usage: rtprio get <pid> | set <pid> fifo|rr|other <prio> | hog <prio> <seconds>\n");
	return 2;
}
