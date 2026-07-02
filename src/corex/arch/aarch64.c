/*
 * aarch64.c - AArch64 architecture-specific register handling
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <string.h>
#include <errno.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/procfs.h>
#include <elf.h>
#include <sys/uio.h>

#include "arch/arch.h"
#include "corex_internal.h"

#ifndef NT_ARM_PAC_MASK
#define NT_ARM_PAC_MASK 0x406
#endif

int arch_read_gp_regs(pid_t tid, corex_gp_regs_t *regs)
{
    struct iovec iov;
    iov.iov_base = regs;
    iov.iov_len = sizeof(*regs);

    if (ptrace(PTRACE_GETREGSET, tid, (void *)(uintptr_t)NT_PRSTATUS, &iov) < 0)
        return -1;

    return 0;
}

int arch_read_fp_regs(pid_t tid, corex_fp_regs_t *regs)
{
    struct iovec iov;
    iov.iov_base = regs;
    iov.iov_len = sizeof(*regs);

    if (ptrace(PTRACE_GETREGSET, tid, (void *)(uintptr_t)NT_PRFPREG, &iov) < 0)
        return -1;

    return 0;
}

int arch_read_pac_mask(pid_t tid, corex_pac_mask_t *out)
{
    /*
     * Capture the pointer-authentication masks. GDB needs the NT_ARM_PAC_MASK
     * note to strip PAC bits from signed return addresses; without it stack
     * unwinding stops at the first PAC-signed frame.
     */
    struct iovec iov;
    iov.iov_base = out;
    iov.iov_len = sizeof(*out);

    if (ptrace(PTRACE_GETREGSET, tid, (void *)(uintptr_t)NT_ARM_PAC_MASK, &iov) < 0)
        return -1;

    return 0;
}

void arch_fill_prstatus(struct elf_prstatus *prs, pid_t pid, pid_t tid,
                        int signo, const corex_gp_regs_t *gp_regs)
{
    memset(prs, 0, sizeof(*prs));

    prs->pr_info.si_signo = signo;
    prs->pr_cursig = signo;
    prs->pr_pid = tid;
    prs->pr_ppid = pid;
    prs->pr_pgrp = pid;
    prs->pr_sid = pid;

    /*
     * On aarch64, struct elf_prstatus.pr_reg is elf_gregset_t
     * which matches struct user_pt_regs layout.
     */
    memcpy(&prs->pr_reg, gp_regs, sizeof(prs->pr_reg));
}

uint16_t arch_get_elf_machine(void)
{
    return EM_AARCH64;
}

size_t arch_fp_regset_size(void)
{
    return sizeof(corex_fp_regs_t);
}
