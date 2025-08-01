/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _LINUX_PRCTL_H
#define _LINUX_PRCTL_H

#include <linux/types.h>

/* Values to pass as first argument to prctl() */

#define PR_SET_PDEATHSIG  1  /* Second arg is a signal */
#define PR_GET_PDEATHSIG  2  /* Second arg is a ptr to return the signal */

/* Get/set current->mm->dumpable */
#define PR_GET_DUMPABLE   3
#define PR_SET_DUMPABLE   4

/* Get/set unaligned access control bits (if meaningful) */
#define PR_GET_UNALIGN	  5
#define PR_SET_UNALIGN	  6
# define PR_UNALIGN_NOPRINT	1	/* silently fix up unaligned user accesses */
# define PR_UNALIGN_SIGBUS	2	/* generate SIGBUS on unaligned user access */

/* Get/set whether or not to drop capabilities on setuid() away from
 * uid 0 (as per security/commoncap.c) */
#define PR_GET_KEEPCAPS   7
#define PR_SET_KEEPCAPS   8

/* Get/set floating-point emulation control bits (if meaningful) */
#define PR_GET_FPEMU  9
#define PR_SET_FPEMU 10
# define PR_FPEMU_NOPRINT	1	/* silently emulate fp operations accesses */
# define PR_FPEMU_SIGFPE	2	/* don't emulate fp operations, send SIGFPE instead */

/* Get/set floating-point exception mode (if meaningful) */
#define PR_GET_FPEXC	11
#define PR_SET_FPEXC	12
# define PR_FP_EXC_SW_ENABLE	0x80	/* Use FPEXC for FP exception enables */
# define PR_FP_EXC_DIV		0x010000	/* floating point divide by zero */
# define PR_FP_EXC_OVF		0x020000	/* floating point overflow */
# define PR_FP_EXC_UND		0x040000	/* floating point underflow */
# define PR_FP_EXC_RES		0x080000	/* floating point inexact result */
# define PR_FP_EXC_INV		0x100000	/* floating point invalid operation */
# define PR_FP_EXC_DISABLED	0	/* FP exceptions disabled */
# define PR_FP_EXC_NONRECOV	1	/* async non-recoverable exc. mode */
# define PR_FP_EXC_ASYNC	2	/* async recoverable exception mode */
# define PR_FP_EXC_PRECISE	3	/* precise exception mode */

/* Get/set whether we use statistical process timing or accurate timestamp
 * based process timing */
#define PR_GET_TIMING   13
#define PR_SET_TIMING   14
# define PR_TIMING_STATISTICAL  0       /* Normal, traditional,
                                                   statistical process timing */
# define PR_TIMING_TIMESTAMP    1       /* Accurate timestamp based
                                                   process timing */

#define PR_SET_NAME    15		/* Set process name */
#define PR_GET_NAME    16		/* Get process name */

/* Get/set process endian */
#define PR_GET_ENDIAN	19
#define PR_SET_ENDIAN	20
# define PR_ENDIAN_BIG		0
# define PR_ENDIAN_LITTLE	1	/* True little endian mode */
# define PR_ENDIAN_PPC_LITTLE	2	/* "PowerPC" pseudo little endian */

/* Get/set process seccomp mode */
#define PR_GET_SECCOMP	21
#define PR_SET_SECCOMP	22

/* Get/set the capability bounding set (as per security/commoncap.c) */
#define PR_CAPBSET_READ 23
#define PR_CAPBSET_DROP 24

/* Get/set the process' ability to use the timestamp counter instruction */
#define PR_GET_TSC 25
#define PR_SET_TSC 26
# define PR_TSC_ENABLE		1	/* allow the use of the timestamp counter */
# define PR_TSC_SIGSEGV		2	/* throw a SIGSEGV instead of reading the TSC */

/* Get/set securebits (as per security/commoncap.c) */
#define PR_GET_SECUREBITS 27
#define PR_SET_SECUREBITS 28

/*
 * Get/set the timerslack as used by poll/select/nanosleep
 * A value of 0 means "use default"
 */
#define PR_SET_TIMERSLACK 29
#define PR_GET_TIMERSLACK 30

#define PR_TASK_PERF_EVENTS_DISABLE		31
#define PR_TASK_PERF_EVENTS_ENABLE		32

/*
 * Set early/late kill mode for hwpoison memory corruption.
 * This influences when the process gets killed on a memory corruption.
 */
#define PR_MCE_KILL	33
# define PR_MCE_KILL_CLEAR   0
# define PR_MCE_KILL_SET     1

# define PR_MCE_KILL_LATE    0
# define PR_MCE_KILL_EARLY   1
# define PR_MCE_KILL_DEFAULT 2

#define PR_MCE_KILL_GET 34

/*
 * Tune up process memory map specifics.
 */
#define PR_SET_MM		35
# define PR_SET_MM_START_CODE		1
# define PR_SET_MM_END_CODE		2
# define PR_SET_MM_START_DATA		3
# define PR_SET_MM_END_DATA		4
# define PR_SET_MM_START_STACK		5
# define PR_SET_MM_START_BRK		6
# define PR_SET_MM_BRK			7
# define PR_SET_MM_ARG_START		8
# define PR_SET_MM_ARG_END		9
# define PR_SET_MM_ENV_START		10
# define PR_SET_MM_ENV_END		11
# define PR_SET_MM_AUXV			12
# define PR_SET_MM_EXE_FILE		13
# define PR_SET_MM_MAP			14
# define PR_SET_MM_MAP_SIZE		15

/*
 * This structure provides new memory descriptor
 * map which mostly modifies /proc/pid/stat[m]
 * output for a task. This mostly done in a
 * sake of checkpoint/restore functionality.
 */
struct prctl_mm_map {
	__u64	start_code;		/* code section bounds */
	__u64	end_code;
	__u64	start_data;		/* data section bounds */
	__u64	end_data;
	__u64	start_brk;		/* heap for brk() syscall */
	__u64	brk;
	__u64	start_stack;		/* stack starts at */
	__u64	arg_start;		/* command line arguments bounds */
	__u64	arg_end;
	__u64	env_start;		/* environment variables bounds */
	__u64	env_end;
	__u64	*auxv;			/* auxiliary vector */
	__u32	auxv_size;		/* vector size */
	__u32	exe_fd;			/* /proc/$pid/exe link file */
};

/*
 * Set specific pid that is allowed to ptrace the current task.
 * A value of 0 mean "no process".
 */
#define PR_SET_PTRACER 0x59616d61
# define PR_SET_PTRACER_ANY ((unsigned long)-1)

#define PR_SET_CHILD_SUBREAPER	36
#define PR_GET_CHILD_SUBREAPER	37

/*
 * If no_new_privs is set, then operations that grant new privileges (i.e.
 * execve) will either fail or not grant them.  This affects suid/sgid,
 * file capabilities, and LSMs.
 *
 * Operations that merely manipulate or drop existing privileges (setresuid,
 * capset, etc.) will still work.  Drop those privileges if you want them gone.
 *
 * Changing LSM security domain is considered a new privilege.  So, for example,
 * asking selinux for a specific new context (e.g. with runcon) will result
 * in execve returning -EPERM.
 *
 * See Documentation/userspace-api/no_new_privs.rst for more details.
 */
#define PR_SET_NO_NEW_PRIVS	38
#define PR_GET_NO_NEW_PRIVS	39

#define PR_GET_TID_ADDRESS	40

#define PR_SET_THP_DISABLE	41
#define PR_GET_THP_DISABLE	42

/*
 * No longer implemented, but left here to ensure the numbers stay reserved:
 */
#define PR_MPX_ENABLE_MANAGEMENT  43
#define PR_MPX_DISABLE_MANAGEMENT 44

#define PR_SET_FP_MODE		45
#define PR_GET_FP_MODE		46
# define PR_FP_MODE_FR		(1 << 0)	/* 64b FP registers */
# define PR_FP_MODE_FRE		(1 << 1)	/* 32b compatibility */

/* Control the ambient capability set */
#define PR_CAP_AMBIENT			47
# define PR_CAP_AMBIENT_IS_SET		1
# define PR_CAP_AMBIENT_RAISE		2
# define PR_CAP_AMBIENT_LOWER		3
# define PR_CAP_AMBIENT_CLEAR_ALL	4

/* arm64 Scalable Vector Extension controls */
/* Flag values must be kept in sync with ptrace NT_ARM_SVE interface */
#define PR_SVE_SET_VL			50	/* set task vector length */
# define PR_SVE_SET_VL_ONEXEC		(1 << 18) /* defer effect until exec */
#define PR_SVE_GET_VL			51	/* get task vector length */
/* Bits common to PR_SVE_SET_VL and PR_SVE_GET_VL */
# define PR_SVE_VL_LEN_MASK		0xffff
# define PR_SVE_VL_INHERIT		(1 << 17) /* inherit across exec */

/* Per task speculation control */
#define PR_GET_SPECULATION_CTRL		52
#define PR_SET_SPECULATION_CTRL		53
/* Speculation control variants */
# define PR_SPEC_STORE_BYPASS		0
# define PR_SPEC_INDIRECT_BRANCH	1
# define PR_SPEC_L1D_FLUSH		2
/* Return and control values for PR_SET/GET_SPECULATION_CTRL */
# define PR_SPEC_NOT_AFFECTED		0
# define PR_SPEC_PRCTL			(1UL << 0)
# define PR_SPEC_ENABLE			(1UL << 1)
# define PR_SPEC_DISABLE		(1UL << 2)
# define PR_SPEC_FORCE_DISABLE		(1UL << 3)
# define PR_SPEC_DISABLE_NOEXEC		(1UL << 4)

/* Reset arm64 pointer authentication keys */
#define PR_PAC_RESET_KEYS		54
# define PR_PAC_APIAKEY			(1UL << 0)
# define PR_PAC_APIBKEY			(1UL << 1)
# define PR_PAC_APDAKEY			(1UL << 2)
# define PR_PAC_APDBKEY			(1UL << 3)
# define PR_PAC_APGAKEY			(1UL << 4)

/* Tagged user address controls for arm64 and RISC-V */
#define PR_SET_TAGGED_ADDR_CTRL		55
#define PR_GET_TAGGED_ADDR_CTRL		56
# define PR_TAGGED_ADDR_ENABLE		(1UL << 0)
/* MTE tag check fault modes */
# define PR_MTE_TCF_NONE		0UL
# define PR_MTE_TCF_SYNC		(1UL << 1)
# define PR_MTE_TCF_ASYNC		(1UL << 2)
# define PR_MTE_TCF_MASK		(PR_MTE_TCF_SYNC | PR_MTE_TCF_ASYNC)
/* MTE tag inclusion mask */
# define PR_MTE_TAG_SHIFT		3
# define PR_MTE_TAG_MASK		(0xffffUL << PR_MTE_TAG_SHIFT)
/* Unused; kept only for source compatibility */
# define PR_MTE_TCF_SHIFT		1
/* MTE tag check store only */
# define PR_MTE_STORE_ONLY		(1UL << 19)
/* RISC-V pointer masking tag length */
# define PR_PMLEN_SHIFT			24
# define PR_PMLEN_MASK			(0x7fUL << PR_PMLEN_SHIFT)

/* Control reclaim behavior when allocating memory */
#define PR_SET_IO_FLUSHER		57
#define PR_GET_IO_FLUSHER		58

/* Dispatch syscalls to a userspace handler */
#define PR_SET_SYSCALL_USER_DISPATCH	59
# define PR_SYS_DISPATCH_OFF		0
/* Enable dispatch except for the specified range */
# define PR_SYS_DISPATCH_EXCLUSIVE_ON	1
/* Enable dispatch for the specified range */
# define PR_SYS_DISPATCH_INCLUSIVE_ON	2
/* Legacy name for backwards compatibility */
# define PR_SYS_DISPATCH_ON		PR_SYS_DISPATCH_EXCLUSIVE_ON
/* The control values for the user space selector when dispatch is enabled */
# define SYSCALL_DISPATCH_FILTER_ALLOW	0
# define SYSCALL_DISPATCH_FILTER_BLOCK	1

/* Set/get enabled arm64 pointer authentication keys */
#define PR_PAC_SET_ENABLED_KEYS		60
#define PR_PAC_GET_ENABLED_KEYS		61

/* Request the scheduler to share a core */
#define PR_SCHED_CORE			62
# define PR_SCHED_CORE_GET		0
# define PR_SCHED_CORE_CREATE		1 /* create unique core_sched cookie */
# define PR_SCHED_CORE_SHARE_TO		2 /* push core_sched cookie to pid */
# define PR_SCHED_CORE_SHARE_FROM	3 /* pull core_sched cookie to pid */
# define PR_SCHED_CORE_MAX		4
# define PR_SCHED_CORE_SCOPE_THREAD		0
# define PR_SCHED_CORE_SCOPE_THREAD_GROUP	1
# define PR_SCHED_CORE_SCOPE_PROCESS_GROUP	2

/* arm64 Scalable Matrix Extension controls */
/* Flag values must be in sync with SVE versions */
#define PR_SME_SET_VL			63	/* set task vector length */
# define PR_SME_SET_VL_ONEXEC		(1 << 18) /* defer effect until exec */
#define PR_SME_GET_VL			64	/* get task vector length */
/* Bits common to PR_SME_SET_VL and PR_SME_GET_VL */
# define PR_SME_VL_LEN_MASK		0xffff
# define PR_SME_VL_INHERIT		(1 << 17) /* inherit across exec */

/* Memory deny write / execute */
#define PR_SET_MDWE			65
# define PR_MDWE_REFUSE_EXEC_GAIN	(1UL << 0)
# define PR_MDWE_NO_INHERIT		(1UL << 1)

#define PR_GET_MDWE			66

#define PR_SET_VMA		0x53564d41
# define PR_SET_VMA_ANON_NAME		0

#define PR_GET_AUXV			0x41555856

#define PR_SET_MEMORY_MERGE		67
#define PR_GET_MEMORY_MERGE		68

#define PR_RISCV_V_SET_CONTROL		69
#define PR_RISCV_V_GET_CONTROL		70
# define PR_RISCV_V_VSTATE_CTRL_DEFAULT		0
# define PR_RISCV_V_VSTATE_CTRL_OFF		1
# define PR_RISCV_V_VSTATE_CTRL_ON		2
# define PR_RISCV_V_VSTATE_CTRL_INHERIT		(1 << 4)
# define PR_RISCV_V_VSTATE_CTRL_CUR_MASK	0x3
# define PR_RISCV_V_VSTATE_CTRL_NEXT_MASK	0xc
# define PR_RISCV_V_VSTATE_CTRL_MASK		0x1f

#define PR_RISCV_SET_ICACHE_FLUSH_CTX	71
# define PR_RISCV_CTX_SW_FENCEI_ON	0
# define PR_RISCV_CTX_SW_FENCEI_OFF	1
# define PR_RISCV_SCOPE_PER_PROCESS	0
# define PR_RISCV_SCOPE_PER_THREAD	1

/* PowerPC Dynamic Execution Control Register (DEXCR) controls */
#define PR_PPC_GET_DEXCR		72
#define PR_PPC_SET_DEXCR		73
/* DEXCR aspect to act on */
# define PR_PPC_DEXCR_SBHE		0 /* Speculative branch hint enable */
# define PR_PPC_DEXCR_IBRTPD		1 /* Indirect branch recurrent target prediction disable */
# define PR_PPC_DEXCR_SRAPD		2 /* Subroutine return address prediction disable */
# define PR_PPC_DEXCR_NPHIE		3 /* Non-privileged hash instruction enable */
/* Action to apply / return */
# define PR_PPC_DEXCR_CTRL_EDITABLE	 0x1 /* Aspect can be modified with PR_PPC_SET_DEXCR */
# define PR_PPC_DEXCR_CTRL_SET		 0x2 /* Set the aspect for this process */
# define PR_PPC_DEXCR_CTRL_CLEAR	 0x4 /* Clear the aspect for this process */
# define PR_PPC_DEXCR_CTRL_SET_ONEXEC	 0x8 /* Set the aspect on exec */
# define PR_PPC_DEXCR_CTRL_CLEAR_ONEXEC	0x10 /* Clear the aspect on exec */
# define PR_PPC_DEXCR_CTRL_MASK		0x1f

/*
 * Get the current shadow stack configuration for the current thread,
 * this will be the value configured via PR_SET_SHADOW_STACK_STATUS.
 */
#define PR_GET_SHADOW_STACK_STATUS      74

/*
 * Set the current shadow stack configuration.  Enabling the shadow
 * stack will cause a shadow stack to be allocated for the thread.
 */
#define PR_SET_SHADOW_STACK_STATUS      75
# define PR_SHADOW_STACK_ENABLE         (1UL << 0)
# define PR_SHADOW_STACK_WRITE		(1UL << 1)
# define PR_SHADOW_STACK_PUSH		(1UL << 2)

/*
 * Prevent further changes to the specified shadow stack
 * configuration.  All bits may be locked via this call, including
 * undefined bits.
 */
#define PR_LOCK_SHADOW_STACK_STATUS      76

/*
 * Controls the mode of timer_create() for CRIU restore operations.
 * Enabling this allows CRIU to restore timers with explicit IDs.
 *
 * Don't use for normal operations as the result might be undefined.
 */
#define PR_TIMER_CREATE_RESTORE_IDS		77
# define PR_TIMER_CREATE_RESTORE_IDS_OFF	0
# define PR_TIMER_CREATE_RESTORE_IDS_ON		1
# define PR_TIMER_CREATE_RESTORE_IDS_GET	2

/* FUTEX hash management */
#define PR_FUTEX_HASH			78
# define PR_FUTEX_HASH_SET_SLOTS	1
# define PR_FUTEX_HASH_GET_SLOTS	2

#endif /* _LINUX_PRCTL_H */
