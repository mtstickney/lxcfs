/* SPDX-License-Identifier: LGPL-2.1+ */

/* Parts of this taken from systemd's implementation. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "../macro.h"
#include "../memory_utils.h"
#include "cgroup2_devices.h"

#ifdef HAVE_STRUCT_BPF_CGROUP_DEV_CTX
#include <linux/bpf.h>
#include <linux/filter.h>

static int bpf_program_add_instructions(struct bpf_program *prog,
					const struct bpf_insn *instructions,
					size_t count)
{

	struct bpf_insn *new_insn;

	if (prog->kernel_fd >= 0)
		return log_error_errno(-1, EBUSY, "Refusing to update bpf cgroup program that's already loaded");

	new_insn = realloc(prog->instructions, sizeof(struct bpf_insn) * (count + prog->n_instructions));
	if (!new_insn)
		return log_error_errno(-1, ENOMEM, "Failed to reallocate bpf cgroup program");

	prog->instructions = new_insn;
	memcpy(prog->instructions + prog->n_instructions, instructions,
	       sizeof(struct bpf_insn) * count);
	prog->n_instructions += count;

	return 0;
}

void bpf_program_free(struct bpf_program *prog)
{
	if (!prog)
		return;

	(void)bpf_program_cgroup_detach(prog);

	if (prog->kernel_fd >= 0)
		close(prog->kernel_fd);
	free(prog->instructions);
	free(prog->attached_path);
	free(prog);
}

/* Memory load, dst_reg = *(uint *) (src_reg + off16) */
#define BPF_LDX_MEM(SIZE, DST, SRC, OFF)                               \
	((struct bpf_insn){.code = BPF_LDX | BPF_SIZE(SIZE) | BPF_MEM, \
			   .dst_reg = DST,                             \
			   .src_reg = SRC,                             \
			   .off = OFF,                                 \
			   .imm = 0})

/* ALU ops on immediates, bpf_add|sub|...: dst_reg += imm32 */
#define BPF_ALU32_IMM(OP, DST, IMM)                              \
	((struct bpf_insn){.code = BPF_ALU | BPF_OP(OP) | BPF_K, \
			   .dst_reg = DST,                       \
			   .src_reg = 0,                         \
			   .off = 0,                             \
			   .imm = IMM})

/* Short form of mov, dst_reg = src_reg */
#define BPF_MOV64_IMM(DST, IMM)                                 \
	((struct bpf_insn){.code = BPF_ALU64 | BPF_MOV | BPF_K, \
			   .dst_reg = DST,                      \
			   .src_reg = 0,                        \
			   .off = 0,                            \
			   .imm = IMM})

#define BPF_MOV32_REG(DST, SRC)                               \
	((struct bpf_insn){.code = BPF_ALU | BPF_MOV | BPF_X, \
			   .dst_reg = DST,                    \
			   .src_reg = SRC,                    \
			   .off = 0,                          \
			   .imm = 0})

/* Conditional jumps against registers, if (dst_reg 'op' src_reg) goto pc + off16 */
#define BPF_JMP_REG(OP, DST, SRC, OFF)                           \
	((struct bpf_insn){.code = BPF_JMP | BPF_OP(OP) | BPF_X, \
			   .dst_reg = DST,                       \
			   .src_reg = SRC,                       \
			   .off = OFF,                           \
			   .imm = 0})

/* Conditional jumps against immediates, if (dst_reg 'op' imm32) goto pc + off16 */
#define BPF_JMP_IMM(OP, DST, IMM, OFF)                           \
	((struct bpf_insn){.code = BPF_JMP | BPF_OP(OP) | BPF_K, \
			   .dst_reg = DST,                       \
			   .src_reg = 0,                         \
			   .off = OFF,                           \
			   .imm = IMM})

/* Program exit */
#define BPF_EXIT_INSN()                                \
	((struct bpf_insn){.code = BPF_JMP | BPF_EXIT, \
			   .dst_reg = 0,               \
			   .src_reg = 0,               \
			   .off = 0,                   \
			   .imm = 0})

static int bpf_access_mask(const char *acc)
{
	int mask = 0;

	if (!acc)
		return mask;

	for (; *acc; acc++)
		switch (*acc) {
		case 'r':
			mask |= BPF_DEVCG_ACC_READ;
			break;
		case 'w':
			mask |= BPF_DEVCG_ACC_WRITE;
			break;
		case 'm':
			mask |= BPF_DEVCG_ACC_MKNOD;
			break;
		default:
			return -EINVAL;
		}

	return mask;
}

static int bpf_device_type(char type)
{
	switch (type) {
	case 'a':
		return 0;
	case 'b':
		return BPF_DEVCG_DEV_BLOCK;
	case 'c':
		return BPF_DEVCG_DEV_CHAR;
	}

	return -1;
}

static inline bool bpf_device_all_access(int access_mask)
{
	return (access_mask == (BPF_DEVCG_ACC_READ | BPF_DEVCG_ACC_WRITE |
				BPF_DEVCG_ACC_MKNOD));
}

struct bpf_program *bpf_program_new(uint32_t prog_type)
{
	__do_free struct bpf_program *prog = NULL;

	prog = calloc(1, sizeof(struct bpf_program));
	if (!prog)
		return NULL;

	prog->prog_type = prog_type;
	prog->kernel_fd = -EBADF;
	/*
	 * By default a allowlist is used unless the user tells us otherwise.
	 */
	prog->device_list_type = LXC_BPF_DEVICE_CGROUP_ALLOWLIST;

	return move_ptr(prog);
}

int bpf_program_init(struct bpf_program *prog)
{
	if (!prog)
		return ret_set_errno(-1, EINVAL);

	const struct bpf_insn pre_insn[] = {
	    /* load device type to r2 */
	    BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_1, offsetof(struct bpf_cgroup_dev_ctx, access_type)),
	    BPF_ALU32_IMM(BPF_AND, BPF_REG_2, 0xFFFF),

	    /* load access type to r3 */
	    BPF_LDX_MEM(BPF_W, BPF_REG_3, BPF_REG_1, offsetof(struct bpf_cgroup_dev_ctx, access_type)),
	    BPF_ALU32_IMM(BPF_RSH, BPF_REG_3, 16),

	    /* load major number to r4 */
	    BPF_LDX_MEM(BPF_W, BPF_REG_4, BPF_REG_1, offsetof(struct bpf_cgroup_dev_ctx, major)),

	    /* load minor number to r5 */
	    BPF_LDX_MEM(BPF_W, BPF_REG_5, BPF_REG_1, offsetof(struct bpf_cgroup_dev_ctx, minor)),
	};

	return bpf_program_add_instructions(prog, pre_insn, ARRAY_SIZE(pre_insn));
}

int bpf_program_append_device(struct bpf_program *prog, struct device_item *device)
{
	int ret;
	int jump_nr = 1;
	struct bpf_insn bpf_access_decision[] = {
	    BPF_MOV64_IMM(BPF_REG_0, device->allow),
	    BPF_EXIT_INSN(),
	};
	int access_mask;
	int device_type;

	if (!prog || !device)
		return ret_set_errno(-1, EINVAL);

	/* This is a global rule so no need to append anything. */
	if (device->global_rule > LXC_BPF_DEVICE_CGROUP_LOCAL_RULE) {
		prog->device_list_type = device->global_rule;
		return 0;
	}

	device_type = bpf_device_type(device->type);
	if (device_type < 0)
		return log_error_errno(-1, EINVAL, "Invalid bpf cgroup device type %c", device->type);

	if (device_type > 0)
		jump_nr++;

	access_mask = bpf_access_mask(device->access);
	if (!bpf_device_all_access(access_mask))
		jump_nr += 3;

	if (device->major != -1)
		jump_nr++;

	if (device->minor != -1)
		jump_nr++;

	if (device_type > 0) {
		struct bpf_insn ins[] = {
		    BPF_JMP_IMM(BPF_JNE, BPF_REG_2, device_type, jump_nr--),
		};

		ret = bpf_program_add_instructions(prog, ins, ARRAY_SIZE(ins));
		if (ret)
			return log_error_errno(-1, errno, "Failed to add instructions to bpf cgroup program");
	}

	if (!bpf_device_all_access(access_mask)) {
		struct bpf_insn ins[] = {
		    BPF_MOV32_REG(BPF_REG_1, BPF_REG_3),
		    BPF_ALU32_IMM(BPF_AND, BPF_REG_1, access_mask),
		    BPF_JMP_REG(BPF_JNE, BPF_REG_1, BPF_REG_3, jump_nr),
		};

		jump_nr -= 3;
		ret = bpf_program_add_instructions(prog, ins, ARRAY_SIZE(ins));
		if (ret)
			return log_error_errno(-1, errno, "Failed to add instructions to bpf cgroup program");
	}

	if (device->major >= 0) {
		struct bpf_insn ins[] = {
		    BPF_JMP_IMM(BPF_JNE, BPF_REG_4, device->major, jump_nr--),
		};

		ret = bpf_program_add_instructions(prog, ins, ARRAY_SIZE(ins));
		if (ret)
			return log_error_errno(-1, errno, "Failed to add instructions to bpf cgroup program");
	}

	if (device->minor >= 0) {
		struct bpf_insn ins[] = {
		    BPF_JMP_IMM(BPF_JNE, BPF_REG_5, device->minor, jump_nr--),
		};

		ret = bpf_program_add_instructions(prog, ins, ARRAY_SIZE(ins));
		if (ret)
			return log_error_errno(-1, errno, "Failed to add instructions to bpf cgroup program");
	}

	ret = bpf_program_add_instructions(prog, bpf_access_decision,
					    ARRAY_SIZE(bpf_access_decision));
	if (ret)
		return log_error_errno(-1, errno, "Failed to add instructions to bpf cgroup program");

	return 0;
}

int bpf_program_finalize(struct bpf_program *prog)
{
	struct bpf_insn ins[] = {
	    BPF_MOV64_IMM(BPF_REG_0, prog->device_list_type),
	    BPF_EXIT_INSN(),
	};

	if (!prog)
		return ret_set_errno(-1, EINVAL);

	TRACE("Implementing %s bpf device cgroup program",
	      prog->device_list_type == LXC_BPF_DEVICE_CGROUP_DENYLIST
		  ? "denylist"
		  : "allowlist");
	return bpf_program_add_instructions(prog, ins, ARRAY_SIZE(ins));
}

static int bpf_program_load_kernel(struct bpf_program *prog, char *log_buf,
				   size_t log_size)
{
	union bpf_attr attr;

	if (prog->kernel_fd >= 0) {
		memset(log_buf, 0, log_size);
		return 0;
	}

	attr = (union bpf_attr){
	    .prog_type	= prog->prog_type,
	    .insns	= PTR_TO_UINT64(prog->instructions),
	    .insn_cnt	= prog->n_instructions,
	    .license	= PTR_TO_UINT64("GPL"),
	    .log_buf	= PTR_TO_UINT64(log_buf),
	    .log_level	= !!log_buf,
	    .log_size	= log_size,
	};

	prog->kernel_fd = bpf(BPF_PROG_LOAD, &attr, sizeof(attr));
	if (prog->kernel_fd < 0)
		return log_error_errno(-1, errno, "Failed to load bpf program");

	return 0;
}

int bpf_program_cgroup_attach(struct bpf_program *prog, int type,
			      const char *path, uint32_t flags)
{
	__do_free char *copy = NULL;
	__do_close int fd = -EBADF;
	union bpf_attr attr;
	int ret;

	if (!prog)
		return ret_set_errno(-1, EINVAL);

	if (flags & ~(BPF_F_ALLOW_OVERRIDE, BPF_F_ALLOW_MULTI))
		return log_error_errno(-1, EINVAL, "Invalid flags for bpf program");

	if (prog->attached_path) {
		if (prog->attached_type != type)
			return log_error_errno(-1, EBUSY, "Wrong type for bpf program");

		if (prog->attached_flags != flags)
			return log_error_errno(-1, EBUSY, "Wrong flags for bpf program");

		if (flags != BPF_F_ALLOW_OVERRIDE)
			return true;
	}

	ret = bpf_program_load_kernel(prog, NULL, 0);
	if (ret < 0)
		return log_error_errno(-1, ret, "Failed to load bpf program");

	copy = strdup(path);
	if (!copy)
		return log_error_errno(-1, ENOMEM, "Failed to duplicate cgroup path %s", path);

	fd = open(path, O_DIRECTORY | O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return log_error_errno(-1, errno, "Failed to open cgroup path %s", path);

	attr = (union bpf_attr){
	    .attach_type	= type,
	    .target_fd		= fd,
	    .attach_bpf_fd	= prog->kernel_fd,
	    .attach_flags	= flags,
	};

	ret = bpf(BPF_PROG_ATTACH, &attr, sizeof(attr));
	if (ret < 0)
		return log_error_errno(-1, errno, "Failed to attach bpf program");

	free_replace_move_ptr(prog->attached_path, copy);
	prog->attached_type = type;
	prog->attached_flags = flags;

	TRACE("Loaded and attached bpf program to cgroup %s", prog->attached_path);
	return 0;
}

int bpf_program_cgroup_detach(struct bpf_program *prog)
{
	int ret;
	__do_close int fd = -EBADF;

	if (!prog)
		return 0;

	if (!prog->attached_path)
		return 0;

	fd = open(prog->attached_path, O_DIRECTORY | O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		if (errno != ENOENT)
			return log_error_errno(-1, errno, "Failed to open attach cgroup %s",
					       prog->attached_path);
	} else {
		union bpf_attr attr;

		attr = (union bpf_attr){
		    .attach_type	= prog->attached_type,
		    .target_fd		= fd,
		    .attach_bpf_fd	= prog->kernel_fd,
		};

		ret = bpf(BPF_PROG_DETACH, &attr, sizeof(attr));
		if (ret < 0)
			return log_error_errno(-1, errno, "Failed to detach bpf program from cgroup %s",
					       prog->attached_path);
	}

	free(prog->attached_path);
	prog->attached_path = NULL;

	return 0;
}

bool bpf_devices_cgroup_supported(void)
{
	const struct bpf_insn dummy[] = {
	    BPF_MOV64_IMM(BPF_REG_0, 1),
	    BPF_EXIT_INSN(),
	};

	__do_bpf_program_free struct bpf_program *prog = NULL;
	int ret;

	if (geteuid() != 0)
		return log_trace(false,
				 "The bpf device cgroup requires real root");

	prog = bpf_program_new(BPF_PROG_TYPE_CGROUP_DEVICE);
	if (prog < 0)
		return log_trace(false, "Failed to allocate new bpf device cgroup program");

	ret = bpf_program_add_instructions(prog, dummy, ARRAY_SIZE(dummy));
	if (ret < 0)
		return log_trace(false, "Failed to add new instructions to bpf device cgroup program");

	ret = bpf_program_load_kernel(prog, NULL, 0);
	if (ret < 0)
		return log_trace(false, "Failed to load new bpf device cgroup program");

	return log_trace(true, "The bpf device cgroup is supported");
}
#endif
