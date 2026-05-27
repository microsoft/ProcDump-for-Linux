/*
 * note_builder.h - Build ELF PT_NOTE segment contents
 */
#ifndef NOTE_BUILDER_H
#define NOTE_BUILDER_H

#include "corex_internal.h"
#include "proc_info.h"
#include "ptrace_utils.h"

/* Opaque note buffer */
typedef struct {
    uint8_t    *data;
    size_t      len;
    size_t      capacity;
} corex_note_buf_t;

/* Initialize a note buffer. Returns 0 on success. */
int note_buf_init(corex_note_buf_t *buf);

/* Free note buffer. */
void note_buf_free(corex_note_buf_t *buf);

/* Build all note entries for the core dump.
 * This writes NT_PRSTATUS (per thread), NT_FPREGSET (per thread),
 * NT_PRPSINFO, NT_SIGINFO, NT_AUXV, NT_FILE into the buffer.
 * Returns 0 on success. */
int note_build_all(corex_note_buf_t *buf,
                   const corex_proc_info_t *proc,
                   const corex_thread_state_t *threads,
                   int num_threads);

#endif /* NOTE_BUILDER_H */
