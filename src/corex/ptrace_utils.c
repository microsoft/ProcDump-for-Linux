/*
 * ptrace_utils.c - ptrace attach/detach and register reading
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/ptrace.h>
#include <sys/wait.h>

#include "corex_internal.h"
#include "ptrace_utils.h"
#include "corex/corex.h"

int ptrace_attach_all(const corex_proc_info_t *info)
{
    for (int i = 0; i < info->num_threads; i++) {
        pid_t tid = info->tids[i];

        if (ptrace(PTRACE_ATTACH, tid, NULL, NULL) < 0) {
            corex_set_error("PTRACE_ATTACH to tid %d failed: %s",
                            (int)tid, strerror(errno));
            /* Detach from threads we already attached to */
            for (int j = 0; j < i; j++) {
                ptrace(PTRACE_DETACH, info->tids[j], NULL, NULL);
            }
            return COREX_ERR_PTRACE;
        }

        /* Wait for the thread to stop */
        int status;
        if (waitpid(tid, &status, __WALL) < 0) {
            corex_set_error("waitpid for tid %d failed: %s",
                            (int)tid, strerror(errno));
            /* Detach from all attached threads */
            for (int j = 0; j <= i; j++) {
                ptrace(PTRACE_DETACH, info->tids[j], NULL, NULL);
            }
            return COREX_ERR_PTRACE;
        }
    }

    return 0;
}

void ptrace_detach_all(const corex_proc_info_t *info)
{
    for (int i = 0; i < info->num_threads; i++) {
        ptrace(PTRACE_DETACH, info->tids[i], NULL, NULL);
    }
}

int ptrace_read_all_regs(const corex_proc_info_t *info,
                         corex_thread_state_t *thread_states)
{
    for (int i = 0; i < info->num_threads; i++) {
        pid_t tid = info->tids[i];
        corex_thread_state_t *ts = &thread_states[i];

        memset(ts, 0, sizeof(*ts));
        ts->tid = tid;
        ts->signo = 0;  /* No signal for a live dump */

        if (arch_read_gp_regs(tid, &ts->gp_regs) != 0) {
            corex_set_error("Failed to read GP regs for tid %d: %s",
                            (int)tid, strerror(errno));
            return COREX_ERR_PTRACE;
        }

        if (arch_read_fp_regs(tid, &ts->fp_regs) != 0) {
            corex_set_error("Failed to read FP regs for tid %d: %s",
                            (int)tid, strerror(errno));
            return COREX_ERR_PTRACE;
        }

        /* Pointer-auth masks are optional (only present on AArch64 with PAC);
         * their absence must not fail the dump. */
        ts->has_pac_mask = (arch_read_pac_mask(tid, &ts->pac_mask) == 0) ? 1 : 0;
    }

    return 0;
}
