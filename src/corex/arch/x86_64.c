/*
 * x86_64.c - x86_64 architecture-specific register handling
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
    /* x86_64 has no pointer authentication. */
    (void)tid;
    (void)out;
    return -1;
}

void arch_fill_prstatus(struct elf_prstatus *prs, pid_t pid, pid_t tid,
                        int signo, const corex_gp_regs_t *gp_regs)
{
    memset(prs, 0, sizeof(*prs));

    prs->pr_info.si_signo = signo;
    prs->pr_cursig = signo;
    prs->pr_pid = tid;
    prs->pr_ppid = pid;  /* parent = process leader for threads */
    prs->pr_pgrp = pid;
    prs->pr_sid = pid;

    /*
     * Copy general-purpose registers into pr_reg.
     * On x86_64, struct elf_prstatus.pr_reg is elf_gregset_t which
     * is the same layout as struct user_regs_struct.
     */
    memcpy(&prs->pr_reg, gp_regs, sizeof(prs->pr_reg));
}

uint16_t arch_get_elf_machine(void)
{
    return EM_X86_64;
}

size_t arch_fp_regset_size(void)
{
    return sizeof(corex_fp_regs_t);
}
