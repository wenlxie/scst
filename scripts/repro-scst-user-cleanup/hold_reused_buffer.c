/*
 * Test-only ioctl interposer for usr/fileio/fileio_tgt.
 * Build with: cc -shared -fPIC -O2 -Wall -Wextra -Iscst/include -o hold_reused_buffer.so hold_reused_buffer.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <signal.h>

#include "scst_user.h"

static int (*next_ioctl)(int, unsigned long, ...);
static int fd_a = -1;
static int fd_b = -1;
static uint64_t a_buffer;

static void mark(const char *event, uint64_t buffer)
{
	char line[160];
	int n = snprintf(line, sizeof(line), "SCST_REPRO_%s buffer=0x%llx\n",
			event, (unsigned long long)buffer);
	if (n > 0) {
		ssize_t written = write(STDERR_FILENO, line,
			n < (int)sizeof(line) ? n : (int)sizeof(line));
		(void)written;
	}
}

int ioctl(int fd, unsigned long request, ...)
{
	void *arg;
	va_list args;
	int ret;

	if (!next_ioctl)
		next_ioctl = dlsym(RTLD_NEXT, "ioctl");
	if (!next_ioctl) {
		errno = ENOSYS;
		return -1;
	}

	if (_IOC_DIR(request) == _IOC_NONE)
		return next_ioctl(fd, request);

	va_start(args, request);
	arg = va_arg(args, void *);
	va_end(args);
	ret = next_ioctl(fd, request, arg);
	if (ret != 0 || !arg)
		return ret;

	if (request == SCST_USER_REGISTER_DEVICE) {
		const struct scst_user_dev_desc *desc = arg;
		if (strcmp(desc->name, "repro_a") == 0)
			fd_a = fd;
		else if (strcmp(desc->name, "repro_b") == 0)
			fd_b = fd;
		return ret;
	}

	if (request == SCST_USER_REPLY_AND_GET_CMD) {
		const struct scst_user_get_cmd *cmd = arg;
		unsigned int lba;
		if (cmd->subcode != SCST_USER_EXEC ||
		    cmd->exec_cmd.cdb[0] != 0x28 ||
		    cmd->exec_cmd.bufflen != 4096)
			return ret;
		lba = ((unsigned int)cmd->exec_cmd.cdb[2] << 24) |
			((unsigned int)cmd->exec_cmd.cdb[3] << 16) |
			((unsigned int)cmd->exec_cmd.cdb[4] << 8) |
			cmd->exec_cmd.cdb[5];

		if (fd == fd_a && lba == 123 && cmd->exec_cmd.pbuf != 0) {
			a_buffer = cmd->exec_cmd.pbuf;
			mark("A_BUFFER", a_buffer);
		} else if (fd == fd_b && lba == 124 && a_buffer != 0 &&
			   cmd->exec_cmd.pbuf == a_buffer) {
			mark("B_HOLDS_A_BUFFER", a_buffer);
			if (close(fd_a) != 0) {
				perror("close A handle");
				_exit(2);
			}
			fd_a = -1;
			mark("A_HANDLE_CLOSED", a_buffer);
			raise(SIGSTOP);
		}
	}
	return ret;
}
