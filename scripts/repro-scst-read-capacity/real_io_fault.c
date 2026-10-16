/* Test-only fault injection for the single-threaded fileio_tgt backend. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "scst_user.h"

static int (*next_ioctl)(int, unsigned long, ...);
static __thread uint32_t pending_handle;
static __thread uint8_t pending_opcode;
static __thread uint8_t pending_service_action;

static void inject_capacity(struct scst_user_get_cmd *cmd)
{
	struct scst_user_reply_cmd *reply;
	uint8_t *data;
	int offset;

	if (!cmd->preply || !pending_handle ||
	    access("/var/tmp/scst-read-capacity-real-io/inject_bad_capacity", F_OK))
		return;

	reply = (void *)(uintptr_t)cmd->preply;
	if (reply->cmd_h != pending_handle ||
	    reply->subcode != SCST_USER_EXEC ||
	    reply->exec_reply.status != 0 ||
	    !reply->exec_reply.pbuf)
		return;

	if (pending_opcode == 0x25 && reply->exec_reply.resp_data_len >= 8)
		offset = 4; /* READ CAPACITY(10) block length */
	else if (pending_opcode == 0x9e && pending_service_action == 0x10 &&
		 reply->exec_reply.resp_data_len >= 12)
		offset = 8; /* READ CAPACITY(16) block length */
	else
		return;

	data = (void *)(uintptr_t)reply->exec_reply.pbuf;
	data[offset] = 0x30;
	data[offset + 1] = 0x39;
	data[offset + 2] = 0x2b;
	data[offset + 3] = 0x30; /* 809052976, matching production */
	fprintf(stderr, "CAPACITY_REAL_IO_INJECT handle=%u opcode=0x%02x block_size=809052976\n",
		pending_handle, pending_opcode);
	fflush(stderr);
}

int ioctl(int fd, unsigned long request, ...)
{
	va_list args;
	void *arg;
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
	if (request == SCST_USER_REPLY_AND_GET_CMD && arg)
		inject_capacity(arg);

	ret = next_ioctl(fd, request, arg);
	if (request == SCST_USER_REPLY_AND_GET_CMD && ret == 0 && arg) {
		struct scst_user_get_cmd *cmd = arg;
		pending_handle = 0;
		if (cmd->subcode == SCST_USER_EXEC) {
			pending_handle = cmd->cmd_h;
			pending_opcode = cmd->exec_cmd.cdb[0];
			pending_service_action = cmd->exec_cmd.cdb[1] & 0x1f;
		}
	}
	return ret;
}
