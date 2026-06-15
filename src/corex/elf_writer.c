/*
 * elf_writer.c - Write a complete ELF core dump file
 *
 * Layout:
 *   [ELF Header]
 *   [Program Headers: PT_NOTE + N x PT_LOAD]
 *   [PT_NOTE segment data]
 *   [padding to page boundary]
 *   [PT_LOAD segment data for mapping 0]
 *   [PT_LOAD segment data for mapping 1]
 *   ...
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <elf.h>

#include "corex_internal.h"
#include "elf_writer.h"
#include "arch/arch.h"
#include "corex/corex.h"

#define COREX_PAGE_SIZE 4096

static size_t align_up_page(size_t val)
{
    return (val + COREX_PAGE_SIZE - 1) & ~(size_t)(COREX_PAGE_SIZE - 1);
}

/*
 * Stream memory from /proc/[pid]/mem to the output file for a given
 * address range. Unreadable pages are written as zeros.
 */
static int write_memory_region(int out_fd, int mem_fd,
                               uint64_t start, uint64_t size)
{
    uint8_t *chunk = malloc(COREX_MEM_CHUNK_SIZE);
    if (!chunk) {
        corex_set_error("Failed to allocate memory chunk buffer");
        return COREX_ERR_ALLOC;
    }

    uint64_t remaining = size;
    uint64_t addr = start;

    while (remaining > 0) {
        size_t to_read = remaining;
        if (to_read > COREX_MEM_CHUNK_SIZE)
            to_read = COREX_MEM_CHUNK_SIZE;

        ssize_t n = pread(mem_fd, chunk, to_read, (off_t)addr);
        if (n <= 0) {
            /*
             * If we can't read this region (e.g., guard page, vsyscall),
             * write zeros. This is what the kernel and GDB do.
             */
            memset(chunk, 0, to_read);
            n = (ssize_t)to_read;
        }

        size_t written = 0;
        while (written < (size_t)n) {
            ssize_t w = write(out_fd, chunk + written, (size_t)n - written);
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                corex_set_error("Write failed: %s", strerror(errno));
                free(chunk);
                return COREX_ERR_WRITE;
            }
            written += (size_t)w;
        }

        addr += (uint64_t)n;
        remaining -= (uint64_t)n;
    }

    free(chunk);
    return 0;
}

/*
 * Write padding zeros to align the file to a given boundary.
 */
static int write_padding(int fd, size_t current_offset, size_t target_offset)
{
    if (target_offset <= current_offset)
        return 0;

    size_t pad_size = target_offset - current_offset;
    uint8_t zeros[4096];
    memset(zeros, 0, sizeof(zeros));

    while (pad_size > 0) {
        size_t chunk = pad_size > sizeof(zeros) ? sizeof(zeros) : pad_size;
        ssize_t w = write(fd, zeros, chunk);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            corex_set_error("Write padding failed: %s", strerror(errno));
            return COREX_ERR_WRITE;
        }
        pad_size -= (size_t)w;
    }

    return 0;
}

int elf_write_core(const char *path,
                   pid_t pid,
                   const corex_proc_info_t *proc,
                   const corex_note_buf_t *notes)
{
    if (proc->num_mappings < 0 || proc->num_mappings > COREX_MAX_MAPPINGS) {
        corex_set_error("Invalid mapping count: %d", proc->num_mappings);
        return COREX_ERR_INVALID_ARG;
    }

    size_t num_mappings = (size_t)proc->num_mappings;
    
    /* Count only mappings that will be dumped */
    int num_loads = 0;
    for (int i = 0; i < proc->num_mappings; i++) {
        if (proc->mappings[i].should_dump)
            num_loads++;
    }
    int num_phdrs = 1 + num_loads;  /* PT_NOTE + PT_LOADs */

    /*
     * Compute layout offsets:
     *   ehdr_offset = 0
     *   phdr_offset = sizeof(Elf64_Ehdr)
     *   note_offset = phdr_offset + num_phdrs * sizeof(Elf64_Phdr)
     *   first PT_LOAD offset = align_up_page(note_offset + notes->len)
     */
    size_t ehdr_size = sizeof(Elf64_Ehdr);
    size_t phdrs_size = (size_t)num_phdrs * sizeof(Elf64_Phdr);
    size_t note_offset = ehdr_size + phdrs_size;
    size_t first_load_offset = align_up_page(note_offset + notes->len);

    /* Calculate all PT_LOAD file offsets (only for dumped segments) */
    size_t *load_offsets = calloc((size_t)proc->num_mappings, sizeof(size_t));
    if (!load_offsets) {
        corex_set_error("Failed to allocate offset table");
        return COREX_ERR_ALLOC;
    }

    size_t current_offset = first_load_offset;
    for (int i = 0; i < proc->num_mappings; i++) {
        if (proc->mappings[i].should_dump) {
            load_offsets[i] = current_offset;
            size_t region_size = (size_t)(proc->mappings[i].end - proc->mappings[i].start);
            current_offset += region_size;
        } else {
            load_offsets[i] = 0;
        }
    }

    /* Open output file */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        corex_set_error("Failed to create %s: %s", path, strerror(errno));
        free(load_offsets);
        return COREX_ERR_OPEN_FAILED;
    }

    int rc = 0;

    /* ---- Write ELF Header ---- */
    Elf64_Ehdr ehdr;
    memset(&ehdr, 0, sizeof(ehdr));
    ehdr.e_ident[EI_MAG0] = ELFMAG0;
    ehdr.e_ident[EI_MAG1] = ELFMAG1;
    ehdr.e_ident[EI_MAG2] = ELFMAG2;
    ehdr.e_ident[EI_MAG3] = ELFMAG3;
    ehdr.e_ident[EI_CLASS] = ELFCLASS64;
    ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr.e_ident[EI_VERSION] = EV_CURRENT;
    ehdr.e_ident[EI_OSABI] = ELFOSABI_NONE;
    ehdr.e_type = ET_CORE;
    ehdr.e_machine = arch_get_elf_machine();
    ehdr.e_version = EV_CURRENT;
    ehdr.e_phoff = ehdr_size;
    ehdr.e_ehsize = sizeof(Elf64_Ehdr);
    ehdr.e_phentsize = sizeof(Elf64_Phdr);
    ehdr.e_phnum = (num_phdrs <= PN_XNUM) ? (uint16_t)num_phdrs : PN_XNUM;

    if (write(fd, &ehdr, sizeof(ehdr)) != sizeof(ehdr)) {
        corex_set_error("Failed to write ELF header: %s", strerror(errno));
        rc = COREX_ERR_WRITE;
        goto out;
    }

    /* ---- Write Program Headers ---- */

    /* PT_NOTE program header */
    Elf64_Phdr note_phdr;
    memset(&note_phdr, 0, sizeof(note_phdr));
    note_phdr.p_type = PT_NOTE;
    note_phdr.p_offset = note_offset;
    note_phdr.p_filesz = notes->len;
    note_phdr.p_align = 4;

    if (write(fd, &note_phdr, sizeof(note_phdr)) != sizeof(note_phdr)) {
        corex_set_error("Failed to write PT_NOTE phdr: %s", strerror(errno));
        rc = COREX_ERR_WRITE;
        goto out;
    }

    /* PT_LOAD program headers (only for dumped segments) */
    for (int i = 0; i < proc->num_mappings; i++) {
        const corex_mapping_t *m = &proc->mappings[i];
        if (!m->should_dump)
            continue;

        uint64_t region_size = m->end - m->start;

        Elf64_Phdr load_phdr;
        memset(&load_phdr, 0, sizeof(load_phdr));
        load_phdr.p_type = PT_LOAD;
        load_phdr.p_vaddr = m->start;
        load_phdr.p_paddr = 0;
        load_phdr.p_memsz = region_size;
        load_phdr.p_flags = m->flags;
        load_phdr.p_align = COREX_PAGE_SIZE;
        load_phdr.p_offset = load_offsets[i];
        load_phdr.p_filesz = region_size;

        if (write(fd, &load_phdr, sizeof(load_phdr)) != sizeof(load_phdr)) {
            corex_set_error("Failed to write PT_LOAD phdr: %s", strerror(errno));
            rc = COREX_ERR_WRITE;
            goto out;
        }
    }

    /* ---- Write PT_NOTE segment data ---- */
    {
        size_t written = 0;
        while (written < notes->len) {
            ssize_t w = write(fd, notes->data + written, notes->len - written);
            if (w < 0) {
                if (errno == EINTR) continue;
                corex_set_error("Failed to write notes: %s", strerror(errno));
                rc = COREX_ERR_WRITE;
                goto out;
            }
            written += (size_t)w;
        }
    }

    /* Pad to page boundary before PT_LOAD data */
    {
        size_t cur = note_offset + notes->len;
        rc = write_padding(fd, cur, first_load_offset);
        if (rc != 0) goto out;
    }

    /* ---- Write PT_LOAD segment data ---- */
    {
        char mem_path[64];
        snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", (int)pid);
        int mem_fd = open(mem_path, O_RDONLY);
        if (mem_fd < 0) {
            corex_set_error("Failed to open %s: %s", mem_path, strerror(errno));
            rc = COREX_ERR_PROC_READ;
            goto out;
        }

        for (int i = 0; i < proc->num_mappings; i++) {
            const corex_mapping_t *m = &proc->mappings[i];
            if (!m->should_dump)
                continue;

            uint64_t region_size = m->end - m->start;

            rc = write_memory_region(fd, mem_fd, m->start, region_size);
            if (rc != 0) {
                close(mem_fd);
                goto out;
            }
        }

        close(mem_fd);
    }

    rc = 0;

out:
    close(fd);
    free(load_offsets);

    if (rc != 0) {
        /* Clean up partial file on error */
        unlink(path);
    }

    return rc;
}
