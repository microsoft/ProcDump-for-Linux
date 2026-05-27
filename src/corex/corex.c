/*
 * corex.c - Top-level API implementation
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/wait.h>
#include <errno.h>

#include "corex_internal.h"
#include "proc_info.h"
#include "ptrace_utils.h"
#include "note_builder.h"
#include "elf_writer.h"
#include <sys/prctl.h>
#include "corex/corex.h"

/* Thread-local error buffer */
_Thread_local char corex_errbuf[COREX_ERR_BUF_SIZE] = {0};

void corex_set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(corex_errbuf, COREX_ERR_BUF_SIZE, fmt, ap);
    va_end(ap);
}

const char *corex_strerror(void)
{
    return corex_errbuf[0] ? corex_errbuf : "No error";
}

/*
 * Core dump implementation for an external process.
 * Attaches to all threads via ptrace, captures state, writes the core,
 * then detaches (resuming the target).
 */
static int do_dump_pid(pid_t pid, const corex_options_t *opts)
{
    int rc;
    corex_proc_info_t *proc = NULL;
    corex_thread_state_t *threads = NULL;
    corex_note_buf_t notes = {0};
    int attached = 0;

    proc = calloc(1, sizeof(*proc));
    if (!proc) {
        corex_set_error("Failed to allocate proc_info");
        rc = COREX_ERR_ALLOC;
        goto cleanup;
    }

    /* Step 1: Attach to all threads (stops them) */
    /*
     * We need to read thread list before attaching. To read /proc/[pid]/task
     * we first do a partial proc_info_read just for threads. But since the
     * process may be running, we first attach to the main thread, then
     * enumerate threads, then attach to the rest.
     */

    /* First, enumerate threads while process is running.
     * Some threads may appear/disappear, but that's a race we accept. */
    {
        /* Quick thread enumeration */
        char task_path[64];
        snprintf(task_path, sizeof(task_path), "/proc/%d/task", (int)pid);
        /* We'll use the full proc_info_read after attaching for everything else,
         * but we need TIDs now for attaching. */
        corex_proc_info_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.pid = pid;

        /* Read just the thread list */
        DIR *d = opendir(task_path);
        if (!d) {
            corex_set_error("Failed to open %s: %s", task_path, strerror(errno));
            rc = COREX_ERR_PROC_READ;
            goto cleanup;
        }
        tmp.num_threads = 0;
        struct dirent *ent;
        while ((ent = readdir(d))) {
            if (ent->d_name[0] == '.') continue;
            if (tmp.num_threads >= COREX_MAX_THREADS) break;
            pid_t tid = (pid_t)atoi(ent->d_name);
            if (tid > 0)
                tmp.tids[tmp.num_threads++] = tid;
        }
        closedir(d);

        if (tmp.num_threads == 0) {
            corex_set_error("No threads found for PID %d", (int)pid);
            rc = COREX_ERR_NO_THREADS;
            goto cleanup;
        }

        /* Copy TIDs to proc */
        proc->pid = pid;
        proc->num_threads = tmp.num_threads;
        memcpy(proc->tids, tmp.tids, sizeof(pid_t) * (size_t)tmp.num_threads);
    }

    /* Attach to all threads */
    rc = ptrace_attach_all(proc);
    if (rc != 0)
        goto cleanup;
    attached = 1;

    /* Step 2: Read full process info (now that the process is stopped)
     * Save the TID list first — proc_info_read() does memset(info, 0, ...)
     * which would wipe the TIDs we already attached to. If a /proc read
     * fails partway through, we need the original TIDs for ptrace detach. */
    int saved_num_threads = proc->num_threads;
    pid_t saved_tids[COREX_MAX_THREADS];
    memcpy(saved_tids, proc->tids, sizeof(pid_t) * (size_t)saved_num_threads);

    rc = proc_info_read(pid, proc);
    if (rc != 0) {
        /* Restore TIDs so cleanup can detach properly */
        proc->num_threads = saved_num_threads;
        memcpy(proc->tids, saved_tids, sizeof(pid_t) * (size_t)saved_num_threads);
        goto cleanup;
    }

    /* Step 2b: Apply coredump_filter to decide which mappings to dump */
    if (!(opts->flags & COREX_FLAG_IGNORE_COREDUMP_FILTER)) {
        uint32_t filter = proc->coredump_filter;

        /*
         * Open /proc/[pid]/mem for reading ELF magic when checking
         * the ELF-header-pages override (bit 4 of coredump_filter).
         */
        char mem_path[64];
        int mem_fd = -1;
        if (filter & (1U << 4)) {
            snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", (int)pid);
            mem_fd = open(mem_path, O_RDONLY);
        }

        for (int i = 0; i < proc->num_mappings; i++) {
            corex_mapping_t *m = &proc->mappings[i];
            /*
             * coredump_filter bits (from kernel docs):
             *   bit 0: anonymous private
             *   bit 1: anonymous shared
             *   bit 2: file-backed private
             *   bit 3: file-backed shared
             *   bit 4: ELF header pages
             *   bit 5: DAX private
             *   bit 6: DAX shared
             */
            int bit;
            if (m->is_file_backed) {
                bit = m->is_shared ? 3 : 2;
            } else {
                bit = m->is_shared ? 1 : 0;
            }
            m->should_dump = (filter & (1U << bit)) ? 1 : 0;

            if (m->should_dump)
                continue;

            /*
             * Always dump writable segments even when filtered by
             * coredump_filter. These contain modified program state
             * (GOT/PLT, .data, .bss, dynamic linker r_debug) that
             * GDB needs to discover shared libraries and resolve
             * symbols. The kernel similarly dumps these pages.
             */
            if (m->flags & PF_W) {
                m->should_dump = 1;
                continue;
            }

            /*
             * Always dump all main executable mappings. The
             * .dynamic section (typically in a read-only page) is
             * written to at runtime by the dynamic linker (DT_DEBUG),
             * so the kernel COWs the page. COW'd pages are effectively
             * anonymous and must come from the core. Without .dynamic,
             * GDB cannot discover shared libraries.
             */
            if (m->path[0] != '\0' &&
                strcmp(m->path, proc->exe) == 0) {
                m->should_dump = 1;
                continue;
            }

            /*
             * Bit 4: ELF header pages. Dump file-backed mappings at
             * file offset 0 that actually contain an ELF header.
             * Only the first page is needed for GDB shared-library
             * discovery, but since corex works at mapping granularity,
             * we verify that the mapping genuinely starts with the
             * ELF magic bytes to avoid pulling in large non-ELF
             * file-backed mappings (e.g. locale-archive).
             */
            if ((filter & (1U << 4)) && m->is_file_backed &&
                m->offset == 0 && mem_fd >= 0) {
                unsigned char magic[4] = {0};
                if (pread(mem_fd, magic, 4, (off_t)m->start) == 4 &&
                    magic[0] == 0x7f && magic[1] == 'E' &&
                    magic[2] == 'L'  && magic[3] == 'F') {
                    m->should_dump = 1;
                }
            }
        }

        if (mem_fd >= 0)
            close(mem_fd);
    }

    /* Step 3: Read registers for all threads */
    threads = calloc((size_t)proc->num_threads, sizeof(*threads));
    if (!threads) {
        corex_set_error("Failed to allocate thread state array");
        rc = COREX_ERR_ALLOC;
        goto cleanup;
    }

    rc = ptrace_read_all_regs(proc, threads);
    if (rc != 0)
        goto cleanup;

    /* Step 4: Build note segment */
    rc = note_buf_init(&notes);
    if (rc != 0)
        goto cleanup;

    rc = note_build_all(&notes, proc, threads, proc->num_threads);
    if (rc != 0)
        goto cleanup;

    /* Step 5: Write ELF core file */
    rc = elf_write_core(opts->output_path, pid, proc, &notes);

cleanup:
    if (attached)
        ptrace_detach_all(proc);

    note_buf_free(&notes);
    free(threads);
    free(proc);

    return rc;
}

int corex_dump_pid(pid_t pid, const corex_options_t *opts)
{
    if (!opts || !opts->output_path) {
        corex_set_error("Invalid arguments: opts and output_path are required");
        return COREX_ERR_INVALID_ARG;
    }

    if (pid <= 0) {
        corex_set_error("Invalid PID: %d", (int)pid);
        return COREX_ERR_INVALID_ARG;
    }

    return do_dump_pid(pid, opts);
}

int corex_dump_self(const corex_options_t *opts)
{
    if (!opts || !opts->output_path) {
        corex_set_error("Invalid arguments: opts and output_path are required");
        return COREX_ERR_INVALID_ARG;
    }

    pid_t parent = getpid();

    /*
     * Fork a child process that will ptrace-attach to the parent
     * and generate the core dump. The parent waits for the child
     * to finish.
     *
     * We use a pipe to communicate the result code back.
     */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        corex_set_error("pipe() failed: %s", strerror(errno));
        return COREX_ERR_FORK;
    }

    pid_t child = fork();
    if (child < 0) {
        corex_set_error("fork() failed: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return COREX_ERR_FORK;
    }

    if (child == 0) {
        /* Child: dump the parent */
        close(pipefd[0]);

        int rc = do_dump_pid(parent, opts);

        /* Write the result code back to the parent */
        ssize_t w;
        do {
            w = write(pipefd[1], &rc, sizeof(rc));
        } while (w < 0 && errno == EINTR);

        /* Also send the error string so the parent can report it */
        const char *errstr = corex_strerror();
        int32_t errlen = (int32_t)strlen(errstr);
        do {
            w = write(pipefd[1], &errlen, sizeof(errlen));
        } while (w < 0 && errno == EINTR);
        if (errlen > 0) {
            do {
                w = write(pipefd[1], errstr, (size_t)errlen);
            } while (w < 0 && errno == EINTR);
        }

        close(pipefd[1]);
        _exit(rc == 0 ? 0 : 1);
    }

    /* Parent: allow the child to ptrace us (required for Yama ptrace_scope >= 1) */
    prctl(PR_SET_PTRACER, (unsigned long)child, 0, 0, 0);

    /* Parent: wait for child to complete */
    close(pipefd[1]);

    int result = COREX_ERR_FORK;
    ssize_t r;
    do {
        r = read(pipefd[0], &result, sizeof(result));
    } while (r < 0 && errno == EINTR);

    /* Read the error string from the child */
    int32_t errlen = 0;
    do {
        r = read(pipefd[0], &errlen, sizeof(errlen));
    } while (r < 0 && errno == EINTR);
    if (r == (ssize_t)sizeof(errlen) && errlen > 0 && errlen < COREX_ERR_BUF_SIZE) {
        char errbuf[COREX_ERR_BUF_SIZE];
        ssize_t total = 0;
        while (total < errlen) {
            r = read(pipefd[0], errbuf + total, (size_t)(errlen - total));
            if (r <= 0) break;
            total += r;
        }
        if (total == errlen) {
            errbuf[errlen] = '\0';
            corex_set_error("%s", errbuf);
        }
    }
    close(pipefd[0]);

    /* Revoke ptrace permission */
    prctl(PR_SET_PTRACER, 0, 0, 0, 0);

    /* Reap the child */
    int status;
    waitpid(child, &status, 0);

    return result;
}
