/*
 * corex.h - CoreX: ELF core dump generation library
 *
 * Generate full ELF core dumps compatible with GDB and other debuggers.
 * Supports dumping the current process or an external process by PID.
 */
#ifndef COREX_H
#define COREX_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Flags for corex_options_t.flags */
#define COREX_FLAG_NONE                0
#define COREX_FLAG_IGNORE_COREDUMP_FILTER (1 << 1)  /* Dump all mappings, ignoring coredump_filter */

/* Return codes */
#define COREX_OK                  0
#define COREX_ERR_INVALID_ARG    -1
#define COREX_ERR_OPEN_FAILED    -2
#define COREX_ERR_PTRACE         -3
#define COREX_ERR_PROC_READ      -4
#define COREX_ERR_WRITE          -5
#define COREX_ERR_FORK           -6
#define COREX_ERR_PERMISSIONS    -7
#define COREX_ERR_NO_THREADS     -8
#define COREX_ERR_ALLOC          -9

/* Options for controlling core dump generation */
typedef struct {
    const char *output_path;    /* Path to write the core file (required) */
    int         flags;          /* Bitwise OR of COREX_FLAG_* constants   */
} corex_options_t;

/*
 * Generate a core dump of the calling process.
 *
 * Internally forks so the child can ptrace-attach to the parent
 * and capture a consistent snapshot. The calling process is briefly
 * stopped during the dump.
 *
 * Returns COREX_OK on success, or a negative COREX_ERR_* code.
 */
int corex_dump_self(const corex_options_t *opts);

/*
 * Generate a core dump of another process identified by PID.
 *
 * Requires appropriate permissions (same UID, or CAP_SYS_PTRACE,
 * and Yama ptrace_scope must allow it).
 *
 * The target process is stopped for the duration of the dump
 * and resumed upon completion.
 *
 * Returns COREX_OK on success, or a negative COREX_ERR_* code.
 */
int corex_dump_pid(pid_t pid, const corex_options_t *opts);

/*
 * Return a human-readable error description for the most recent
 * failure on the calling thread.
 */
const char *corex_strerror(void);

#ifdef __cplusplus
}
#endif

#endif /* COREX_H */
