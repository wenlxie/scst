// SPDX-License-Identifier: GPL-2.0
/*
 * LD_PRELOAD hook for usr/fileio/fileio_tgt: fork() once when the file named
 * by $FORK_TRIGGER exists. The child only sleeps and never touches the
 * buffers, like a crash reporter, system() or popen() in a real handler.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int (*next_ioctl)(int, unsigned long, ...);
static int forked;

static void fork_once(void)
{
	const char *trigger = getenv("FORK_TRIGGER");
	pid_t pid;

	if (forked || !trigger || access(trigger, F_OK))
		return;
	forked = 1;
	pid = fork();
	if (pid == 0) {
		for (;;)
			pause();
	}
	fprintf(stderr, "FORK_ONCE child=%d\n", (int)pid);
	fflush(stderr);
}

int ioctl(int fd, unsigned long request, ...)
{
	va_list args;
	void *arg;

	if (!next_ioctl)
		next_ioctl = dlsym(RTLD_NEXT, "ioctl");
	if (!next_ioctl) {
		errno = ENOSYS;
		return -1;
	}
	va_start(args, request);
	arg = va_arg(args, void *);
	va_end(args);

	fork_once();
	return next_ioctl(fd, request, arg);
}
