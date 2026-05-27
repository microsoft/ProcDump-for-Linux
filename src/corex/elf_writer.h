/*
 * elf_writer.h - Write a complete ELF core dump file
 */
#ifndef ELF_WRITER_H
#define ELF_WRITER_H

#include "corex_internal.h"
#include "proc_info.h"
#include "note_builder.h"

/*
 * Write a complete ELF core dump file.
 *
 * Steps:
 *   1. Write ELF header
 *   2. Write program headers (PT_NOTE + PT_LOAD per mapping)
 *   3. Write the PT_NOTE segment data
 *   4. Stream PT_LOAD data from /proc/[pid]/mem
 *
 * Returns 0 on success.
 */
int elf_write_core(const char *path,
                   pid_t pid,
                   const corex_proc_info_t *proc,
                   const corex_note_buf_t *notes);

#endif /* ELF_WRITER_H */
