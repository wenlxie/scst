// SPDX-License-Identifier: GPL-2.0-or-later
/* Exercise the real SCST completion path without registering a disk. */
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include "scst.h"

static unsigned int failures, tests, callbacks;
static bool test_tape = true;
module_param(test_tape, bool, 0444);
MODULE_PARM_DESC(test_tape, "Test tape parsing; disable for unpatched SCST");

static void set_block_shift(struct scst_cmd *cmd, int shift)
{
	callbacks++;
	if (shift >= 9 && shift < 31) {
		cmd->dev->block_shift = shift;
		cmd->dev->block_size = 1U << shift;
	}
}

static void check_shift(int size, int expected)
{
	int actual = scst_calc_block_shift(size);

	tests++;
	if (actual != expected)
		failures++;
	pr_info("CAPACITY_PROBE shift input=%d expected=%d actual=%d %s\n",
		size, expected, actual, actual == expected ? "PASS" : "FAIL");
}

static int check_capacity(bool rc16, bool split, u32 block_size, int expected_shift)
{
	struct scst_cmd *cmd;
	struct scst_device *dev;
	struct scatterlist sg[2];
	struct page *pages[2] = { NULL, NULL };
	u8 original[32] = { 0 }, actual[32] = { 0 };
	u8 sense[SCST_SENSE_BUFFERSIZE];
	unsigned int length = rc16 ? 32 : 8;
	unsigned int offset = rc16 ? 8 : 4;
	unsigned int first = split ? offset : length;
	bool valid = expected_shift >= 0;
	u32 expected_geometry = valid ? 1U << expected_shift : 4096;
	bool unchanged, status_ok, passed;

	cmd = kzalloc(sizeof(*cmd), GFP_KERNEL);
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	pages[0] = alloc_page(GFP_KERNEL);
	if (split)
		pages[1] = alloc_page(GFP_KERNEL);
	if (!cmd || !dev || !pages[0] || (split && !pages[1]))
		goto nomem;

	original[offset - 1] = 0xff; /* Last LBA. */
	put_unaligned_be32(block_size, &original[offset]);
	memcpy(page_address(pages[0]), original, first);
	sg_init_table(sg, split ? 2 : 1);
	sg_set_page(&sg[0], pages[0], first, 0);
	if (split) {
		memcpy(page_address(pages[1]), original + first, length - first);
		sg_set_page(&sg[1], pages[1], length - first, 0);
	}
	dev->virt_name = "capacity_probe";
	dev->block_shift = 12;
	dev->block_size = 4096;
	cmd->dev = dev;
	cmd->cdb = cmd->cdb_buf;
	cmd->cdb[0] = rc16 ? SERVICE_ACTION_IN_16 : READ_CAPACITY;
	cmd->cdb[1] = rc16 ? SAI_READ_CAPACITY_16 : 0;
	cmd->op_name = rc16 ? "READ CAPACITY(16)" : "READ CAPACITY";
	cmd->sense = sense;
	cmd->sense_buflen = sizeof(sense);
	cmd->completed = 1;
	cmd->status = SAM_STAT_GOOD;
	cmd->data_direction = SCST_DATA_READ;
	cmd->bufflen = length;
	cmd->resp_data_len = length;
	cmd->sg = sg;
	cmd->sg_cnt = split ? 2 : 1;
	callbacks = 0;
	scst_block_generic_dev_done(cmd, set_block_shift);
	memcpy(actual, page_address(pages[0]), first);
	if (split)
		memcpy(actual + first, page_address(pages[1]), length - first);
	unchanged = !memcmp(original, actual, length);
	if (valid) {
		status_ok = cmd->status == SAM_STAT_GOOD &&
			cmd->resp_data_len == length &&
			cmd->data_direction == SCST_DATA_READ;
	} else {
		status_ok = cmd->status == SAM_STAT_CHECK_CONDITION &&
			cmd->resp_data_len == 0 &&
			cmd->data_direction == SCST_DATA_NONE &&
			cmd->is_send_status &&
			scst_analyze_sense(cmd->sense, cmd->sense_valid_len,
					   SCST_SENSE_ALL_VALID,
					   SCST_LOAD_SENSE(scst_sense_internal_failure));
	}
	passed = unchanged && status_ok &&
		dev->block_size == expected_geometry &&
		dev->block_shift == (valid ? expected_shift : 12) &&
		callbacks == (valid ? 1 : 0);
	tests++;
	if (!passed)
		failures++;
	pr_info("CAPACITY_PROBE rc=%d sg_cnt=%d supplied=%u observed=%u geometry=%u callbacks=%u unchanged=%d status=0x%x resp_len=%d %s\n",
		rc16 ? 16 : 10, cmd->sg_cnt, block_size,
		get_unaligned_be32(actual + offset), dev->block_size,
		callbacks, unchanged, cmd->status, cmd->resp_data_len,
		passed ? "PASS" : "FAIL");
	if (pages[1])
		__free_page(pages[1]);
	__free_page(pages[0]);
	kfree(dev);
	kfree(cmd);
	return 0;

nomem:
	if (pages[1])
		__free_page(pages[1]);
	if (pages[0])
		__free_page(pages[0]);
	kfree(dev);
	kfree(cmd);
	return -ENOMEM;
}

static int check_tape(const char *name, u32 block_size, bool fixed,
		      int bufflen, s64 data_len, int out_bufflen, int expected)
{
	struct scst_cmd *cmd;
	struct scst_device *dev;
	u8 sense[SCST_SENSE_BUFFERSIZE];
	u32 factor = fixed ? block_size : 1;
	bool passed;
	int actual;

	cmd = kzalloc(sizeof(*cmd), GFP_KERNEL);
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!cmd || !dev) {
		kfree(dev);
		kfree(cmd);
		return -ENOMEM;
	}
	dev->virt_name = "capacity_probe_tape";
	dev->block_size = block_size;
	dev->block_shift = -1; /* Tape parsing must not use a shift. */
	cmd->dev = dev;
	cmd->cdb = cmd->cdb_buf;
	cmd->cdb[0] = READ_6;
	cmd->cdb[1] = fixed ? 1 : 0;
	cmd->cdb_len = 6;
	cmd->op_name = "TAPE READ(6)";
	cmd->op_flags = SCST_TRANSFER_LEN_TYPE_FIXED;
	cmd->sense = sense;
	cmd->sense_buflen = sizeof(sense);
	cmd->data_direction = SCST_DATA_READ;
	cmd->bufflen = bufflen;
	cmd->data_len = data_len;
	cmd->out_bufflen = out_bufflen;
	actual = scst_tape_generic_parse(cmd);
	passed = actual == expected;
	if (!expected) {
		passed = passed && cmd->status == SAM_STAT_GOOD &&
			cmd->bufflen == (u64)bufflen * factor &&
			cmd->data_len == (u64)data_len * factor &&
			cmd->out_bufflen == (u64)out_bufflen * factor;
	} else {
		passed = passed && cmd->status == SAM_STAT_CHECK_CONDITION &&
			cmd->bufflen == bufflen && cmd->data_len == data_len &&
			cmd->out_bufflen == out_bufflen &&
			scst_analyze_sense(cmd->sense, cmd->sense_valid_len,
					   SCST_SENSE_ALL_VALID,
					   SCST_LOAD_SENSE(scst_sense_block_out_range_error));
	}
	tests++;
	if (!passed)
		failures++;
	pr_info("CAPACITY_PROBE tape=%s block_size=%u fixed=%d rc=%d bufflen=%d data_len=%lld out_bufflen=%d %s\n",
		name, block_size, fixed, actual, cmd->bufflen,
		(long long)cmd->data_len, cmd->out_bufflen,
		passed ? "PASS" : "FAIL");
	kfree(dev);
	kfree(cmd);
	return 0;
}

static int __init capacity_probe_init(void)
{
	static const struct {
		const char *name;
		u32 block_size;
		bool fixed;
		int bufflen;
		s64 data_len;
		int out_bufflen;
		int expected;
	} tape_cases[] = {
		{ "power_of_two", 512, true, 2, 3, 1, 0 },
		{ "non_power_of_two", 1536, true, 2, 3, 1, 0 },
		{ "small_block", 80, true, 2, 3, 1, 0 },
		{ "variable", 0, false, 1000, 1000, 0, 0 },
		{ "zero_fixed", 0, true, 2, 3, 1, -EINVAL },
		{ "bufflen_overflow", 1536, true, INT_MAX / 1536 + 1, 1, 0, -EINVAL },
		{ "bufflen_boundary", 1536, true, INT_MAX / 1536, 1, 0, 0 },
		{ "out_bufflen_overflow", 512, true, 1, 1, INT_MAX / 512 + 1, -EINVAL },
		{ "data_len_overflow", 512, true, 1, LLONG_MAX / 512 + 1, 0, -EINVAL },
		{ "data_len_boundary", 512, true, 1, LLONG_MAX / 512, 0, 0 },
	};
	unsigned int rc16, split;
	unsigned int i;
	int ret;

	pr_info("CAPACITY_PROBE BEGIN\n");
	check_shift(0, 9);
	check_shift(512, 9);
	check_shift(4096, 12);
	check_shift(65536, 16);
	check_shift(4, -1);
	check_shift(809052976, -1);
	check_shift(768, -1);
	for (rc16 = 0; rc16 < 2; rc16++) {
		for (split = 0; split < 2; split++) {
			ret = check_capacity(rc16, split, 4096, 12);
			if (ret)
				return ret;
			ret = check_capacity(rc16, split, 809052976, -1);
			if (ret)
				return ret;
			ret = check_capacity(rc16, split, 0, -1);
			if (ret)
				return ret;
		}
		ret = check_capacity(rc16, false, 512, 9);
		if (ret)
			return ret;
	}
	if (test_tape) {
		for (i = 0; i < ARRAY_SIZE(tape_cases); i++) {
			ret = check_tape(tape_cases[i].name, tape_cases[i].block_size,
					 tape_cases[i].fixed, tape_cases[i].bufflen,
					 tape_cases[i].data_len, tape_cases[i].out_bufflen,
					 tape_cases[i].expected);
			if (ret)
				return ret;
		}
	}
	pr_info("CAPACITY_PROBE SUMMARY tests=%u failed=%u\n", tests, failures);
	return 0;
}

static void __exit capacity_probe_exit(void)
{
}

module_init(capacity_probe_init);
module_exit(capacity_probe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SCST READ CAPACITY response corruption regression probe");
MODULE_IMPORT_NS(SCST);
