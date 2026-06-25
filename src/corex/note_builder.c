/*
 * note_builder.c - Build ELF PT_NOTE segment for core dumps
 *
 * Each note entry has the format:
 *   Elf64_Nhdr { namesz, descsz, type }
 *   name (padded to 4-byte alignment)
 *   desc (padded to 4-byte alignment)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <elf.h>
#include <sys/procfs.h>

#include "corex_internal.h"
#include "note_builder.h"
#include "arch/arch.h"
#include "corex/corex.h"

#ifndef NT_ARM_PAC_MASK
#define NT_ARM_PAC_MASK 0x406
#endif

#define NOTE_INITIAL_CAPACITY (256 * 1024)
#define NOTE_ALIGN 4

static size_t align_up(size_t val, size_t align)
{
    return (val + align - 1) & ~(align - 1);
}

int note_buf_init(corex_note_buf_t *buf)
{
    buf->data = malloc(NOTE_INITIAL_CAPACITY);
    if (!buf->data) {
        corex_set_error("Failed to allocate note buffer");
        return COREX_ERR_ALLOC;
    }
    buf->len = 0;
    buf->capacity = NOTE_INITIAL_CAPACITY;
    return 0;
}

void note_buf_free(corex_note_buf_t *buf)
{
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->capacity = 0;
}

static int note_buf_grow(corex_note_buf_t *buf, size_t needed)
{
    if (buf->len + needed <= buf->capacity)
        return 0;

    size_t new_cap = buf->capacity * 2;
    while (new_cap < buf->len + needed)
        new_cap *= 2;

    uint8_t *new_data = realloc(buf->data, new_cap);
    if (!new_data) {
        corex_set_error("Failed to grow note buffer");
        return COREX_ERR_ALLOC;
    }
    buf->data = new_data;
    buf->capacity = new_cap;
    return 0;
}

/*
 * Append a single note entry to the buffer.
 *   name: note name string (e.g. "CORE" or "LINUX")
 *   type: note type (e.g. NT_PRSTATUS)
 *   desc: pointer to note data
 *   descsz: size of note data
 */
static int note_append(corex_note_buf_t *buf, const char *name,
                       uint32_t type, const void *desc, size_t descsz)
{
    uint32_t namesz = (uint32_t)strlen(name) + 1;  /* includes NUL */
    size_t name_padded = align_up(namesz, NOTE_ALIGN);
    size_t desc_padded = align_up(descsz, NOTE_ALIGN);
    size_t total = sizeof(Elf64_Nhdr) + name_padded + desc_padded;

    int rc = note_buf_grow(buf, total);
    if (rc != 0)
        return rc;

    uint8_t *p = buf->data + buf->len;
    memset(p, 0, total);

    /* Write the header */
    Elf64_Nhdr *nhdr = (Elf64_Nhdr *)p;
    nhdr->n_namesz = namesz;
    nhdr->n_descsz = (uint32_t)descsz;
    nhdr->n_type = type;

    /* Write the name */
    memcpy(p + sizeof(Elf64_Nhdr), name, namesz);

    /* Write the descriptor */
    memcpy(p + sizeof(Elf64_Nhdr) + name_padded, desc, descsz);

    buf->len += total;
    return 0;
}

static int build_prstatus_notes(corex_note_buf_t *buf,
                                const corex_proc_info_t *proc,
                                const corex_thread_state_t *threads,
                                int num_threads)
{
    for (int i = 0; i < num_threads; i++) {
        struct elf_prstatus prs;
        arch_fill_prstatus(&prs, proc->pid, threads[i].tid,
                           threads[i].signo, &threads[i].gp_regs);

        int rc = note_append(buf, "CORE", NT_PRSTATUS, &prs, sizeof(prs));
        if (rc != 0)
            return rc;

        /* NT_FPREGSET immediately after the corresponding NT_PRSTATUS */
        rc = note_append(buf, "CORE", NT_FPREGSET,
                         &threads[i].fp_regs, arch_fp_regset_size());
        if (rc != 0)
            return rc;

        /*
         * NT_ARM_PAC_MASK (AArch64 pointer-authentication masks). GDB uses it
         * to strip PAC bits from signed return addresses; without it stack
         * unwinding halts at the first PAC-signed frame. Emitted per-thread,
         * matching the kernel's core-dump layout. Uses the "LINUX" note owner.
         */
        if (threads[i].has_pac_mask) {
            rc = note_append(buf, "LINUX", NT_ARM_PAC_MASK,
                             &threads[i].pac_mask, sizeof(threads[i].pac_mask));
            if (rc != 0)
                return rc;
        }
    }
    return 0;
}

static int build_prpsinfo_note(corex_note_buf_t *buf,
                               const corex_proc_info_t *proc)
{
    struct elf_prpsinfo psinfo;
    memset(&psinfo, 0, sizeof(psinfo));

    psinfo.pr_state = 0;    /* Running */
    psinfo.pr_sname = 'R';
    psinfo.pr_zomb = 0;
    psinfo.pr_nice = 0;
    psinfo.pr_pid = proc->pid;
    psinfo.pr_ppid = proc->ppid;
    psinfo.pr_pgrp = proc->pgrp;
    psinfo.pr_sid = proc->sid;
    psinfo.pr_uid = proc->uid;
    psinfo.pr_gid = proc->gid;

    strncpy(psinfo.pr_fname, proc->comm, sizeof(psinfo.pr_fname) - 1);

    /* Build a space-separated args string from the NUL-separated cmdline */
    if (proc->cmdline_len > 0) {
        size_t out = 0;
        for (int i = 0; i < proc->cmdline_len && out < sizeof(psinfo.pr_psargs) - 1; i++) {
            char c = proc->cmdline[i];
            psinfo.pr_psargs[out++] = (c == '\0') ? ' ' : c;
        }
        /* Trim trailing space */
        if (out > 0 && psinfo.pr_psargs[out - 1] == ' ')
            psinfo.pr_psargs[out - 1] = '\0';
    }

    return note_append(buf, "CORE", NT_PRPSINFO, &psinfo, sizeof(psinfo));
}

static int build_siginfo_note(corex_note_buf_t *buf)
{
    /* For a live dump, there's no crashing signal. Write a zeroed siginfo. */
    siginfo_t si;
    memset(&si, 0, sizeof(si));
    return note_append(buf, "CORE", NT_SIGINFO, &si, sizeof(si));
}

static int build_auxv_note(corex_note_buf_t *buf,
                           const corex_proc_info_t *proc)
{
    if (proc->auxv_len == 0)
        return 0;
    return note_append(buf, "CORE", NT_AUXV, proc->auxv, proc->auxv_len);
}

/*
 * Build NT_FILE note.
 * Format:
 *   uint64_t count         - number of file mappings
 *   uint64_t page_size     - page size
 *   For each mapping:
 *     uint64_t start
 *     uint64_t end
 *     uint64_t file_offset (in pages)
 *   Followed by NUL-terminated filename strings (one per mapping)
 */
static int build_file_note(corex_note_buf_t *buf,
                           const corex_proc_info_t *proc)
{
    /* Count file-backed mappings */
    int count = 0;
    for (int i = 0; i < proc->num_mappings; i++) {
        if (proc->mappings[i].path[0] == '/' )
            count++;
    }

    if (count == 0)
        return 0;

    uint64_t page_size = 4096;

    /* Calculate total size */
    size_t header_size = sizeof(uint64_t) * 2;  /* count + page_size */
    size_t entries_size = (size_t)count * sizeof(uint64_t) * 3;
    size_t strings_size = 0;
    for (int i = 0; i < proc->num_mappings; i++) {
        if (proc->mappings[i].path[0] == '/')
            strings_size += strlen(proc->mappings[i].path) + 1;
    }

    size_t total = header_size + entries_size + strings_size;
    uint8_t *data = calloc(1, total);
    if (!data) {
        corex_set_error("Failed to allocate NT_FILE data");
        return COREX_ERR_ALLOC;
    }

    uint8_t *p = data;

    /* Write header */
    uint64_t u64_count = (uint64_t)count;
    memcpy(p, &u64_count, sizeof(uint64_t)); p += sizeof(uint64_t);
    memcpy(p, &page_size, sizeof(uint64_t)); p += sizeof(uint64_t);

    /* Write entries */
    for (int i = 0; i < proc->num_mappings; i++) {
        if (proc->mappings[i].path[0] != '/')
            continue;
        const corex_mapping_t *m = &proc->mappings[i];
        uint64_t start = m->start;
        uint64_t end = m->end;
        uint64_t offset_pages = m->offset / page_size;
        memcpy(p, &start, sizeof(uint64_t)); p += sizeof(uint64_t);
        memcpy(p, &end, sizeof(uint64_t)); p += sizeof(uint64_t);
        memcpy(p, &offset_pages, sizeof(uint64_t)); p += sizeof(uint64_t);
    }

    /* Write filenames */
    for (int i = 0; i < proc->num_mappings; i++) {
        if (proc->mappings[i].path[0] != '/')
            continue;
        size_t len = strlen(proc->mappings[i].path) + 1;
        memcpy(p, proc->mappings[i].path, len);
        p += len;
    }

    int rc = note_append(buf, "CORE", NT_FILE, data, total);
    free(data);
    return rc;
}

int note_build_all(corex_note_buf_t *buf,
                   const corex_proc_info_t *proc,
                   const corex_thread_state_t *threads,
                   int num_threads)
{
    int rc;

    /*
     * Order matters for GDB compatibility:
     * 1. NT_PRSTATUS + NT_FPREGSET for each thread (first thread = crashing thread)
     * 2. NT_PRPSINFO (process info)
     * 3. NT_SIGINFO
     * 4. NT_AUXV
     * 5. NT_FILE
     */

    if ((rc = build_prstatus_notes(buf, proc, threads, num_threads)) != 0)
        return rc;
    if ((rc = build_prpsinfo_note(buf, proc)) != 0)
        return rc;
    if ((rc = build_siginfo_note(buf)) != 0)
        return rc;
    if ((rc = build_auxv_note(buf, proc)) != 0)
        return rc;
    if ((rc = build_file_note(buf, proc)) != 0)
        return rc;

    return 0;
}
