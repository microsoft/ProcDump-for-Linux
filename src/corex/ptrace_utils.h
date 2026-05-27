/*
 * ptrace_utils.h - ptrace attach/detach, register reading
 */
#ifndef PTRACE_UTILS_H
#define PTRACE_UTILS_H

#include "corex_internal.h"
#include "proc_info.h"
#include "arch/arch.h"

/* Per-thread register state */
typedef struct {
    pid_t               tid;
    int                 signo;          /* Signal that stopped this thread */
    corex_gp_regs_t     gp_regs;       /* General-purpose registers */
    corex_fp_regs_t     fp_regs;       /* Floating-point registers */
} corex_thread_state_t;

/* Attach to all threads of a process. Stops all threads.
 * Returns 0 on success. */
int ptrace_attach_all(const corex_proc_info_t *info);

/* Detach from all threads. Resumes all threads. */
void ptrace_detach_all(const corex_proc_info_t *info);

/* Read registers for all threads.
 * thread_states must have space for info->num_threads entries.
 * Returns 0 on success. */
int ptrace_read_all_regs(const corex_proc_info_t *info,
                         corex_thread_state_t *thread_states);

#endif /* PTRACE_UTILS_H */
