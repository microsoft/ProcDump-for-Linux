/*
 * proc_info.c - Read process information from /proc filesystem
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <elf.h>

#include "corex_internal.h"
#include "proc_info.h"
#include "corex/corex.h"

static int read_maps(pid_t pid, corex_proc_info_t *info)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", (int)pid);

    FILE *f = fopen(path, "r");
    if (!f) {
        corex_set_error("Failed to open %s: %s", path, strerror(errno));
        return COREX_ERR_PROC_READ;
    }

    info->num_mappings = 0;
    char line[1024];

    while (fgets(line, sizeof(line), f)) {
        if (info->num_mappings >= COREX_MAX_MAPPINGS)
            break;

        corex_mapping_t *m = &info->mappings[info->num_mappings];
        memset(m, 0, sizeof(*m));

        uint64_t start, end, offset;
        unsigned int dev_major, dev_minor;
        uint64_t inode;
        char perms[5] = {0};

        int n = sscanf(line, "%lx-%lx %4s %lx %x:%x %lu",
                       &start, &end, perms, &offset,
                       &dev_major, &dev_minor, &inode);
        if (n < 7)
            continue;

        m->start = start;
        m->end = end;
        m->offset = offset;
        m->flags = 0;

        if (perms[0] == 'r') m->flags |= PF_R;
        if (perms[1] == 'w') m->flags |= PF_W;
        if (perms[2] == 'x') m->flags |= PF_X;

        m->is_shared = (perms[3] == 's') ? 1 : 0;
        m->is_file_backed = (inode > 0) ? 1 : 0;
        m->should_dump = 1;  /* default: dump everything */

        /* Extract the file path (skip whitespace after inode) */
        char *p = strchr(line, '/');
        if (!p)
            p = strchr(line, '[');
        if (p) {
            size_t len = strlen(p);
            if (len > 0 && p[len - 1] == '\n')
                p[len - 1] = '\0';
            snprintf(m->path, sizeof(m->path), "%s", p);
        }

        info->num_mappings++;
    }

    fclose(f);
    return 0;
}

static int read_auxv(pid_t pid, corex_proc_info_t *info)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/auxv", (int)pid);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        corex_set_error("Failed to open %s: %s", path, strerror(errno));
        return COREX_ERR_PROC_READ;
    }

    ssize_t n = read(fd, info->auxv, sizeof(info->auxv));
    close(fd);

    if (n < 0) {
        corex_set_error("Failed to read %s: %s", path, strerror(errno));
        return COREX_ERR_PROC_READ;
    }

    info->auxv_len = (size_t)n;
    return 0;
}

static int read_status(pid_t pid, corex_proc_info_t *info)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);

    FILE *f = fopen(path, "r");
    if (!f) {
        corex_set_error("Failed to open %s: %s", path, strerror(errno));
        return COREX_ERR_PROC_READ;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "PPid:", 5) == 0) {
            info->ppid = (pid_t)atoi(line + 5);
        } else if (strncmp(line, "Uid:", 4) == 0) {
            info->uid = (uid_t)strtoul(line + 4, NULL, 10);
        } else if (strncmp(line, "Gid:", 4) == 0) {
            info->gid = (gid_t)strtoul(line + 4, NULL, 10);
        } else if (strncmp(line, "NSpgid:", 7) == 0) {
            info->pgrp = (pid_t)atoi(line + 7);
        } else if (strncmp(line, "NSsid:", 6) == 0) {
            info->sid = (pid_t)atoi(line + 6);
        }
    }

    fclose(f);

    /* Fallback: if NSpgid/NSsid were not present (kernel < 4.1),
     * read pgrp (field 5) and session (field 6) from /proc/[pid]/stat. */
    if (info->pgrp == 0 && info->sid == 0) {
        char stat_path[64];
        snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", (int)pid);
        FILE *sf = fopen(stat_path, "r");
        if (sf) {
            char statbuf[1024];
            if (fgets(statbuf, sizeof(statbuf), sf)) {
                /* Fields after the comm "(name)" field */
                char *p = strrchr(statbuf, ')');
                if (p) {
                    int pgrp_val = 0, sid_val = 0;
                    /* After ')': state, ppid, pgrp, session */
                    if (sscanf(p + 2, "%*c %*d %d %d", &pgrp_val, &sid_val) == 2) {
                        info->pgrp = (pid_t)pgrp_val;
                        info->sid = (pid_t)sid_val;
                    }
                }
            }
            fclose(sf);
        }
    }

    return 0;
}

static int read_comm(pid_t pid, corex_proc_info_t *info)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);

    FILE *f = fopen(path, "r");
    if (!f) {
        corex_set_error("Failed to open %s: %s", path, strerror(errno));
        return COREX_ERR_PROC_READ;
    }

    if (fgets(info->comm, sizeof(info->comm), f)) {
        size_t len = strlen(info->comm);
        if (len > 0 && info->comm[len - 1] == '\n')
            info->comm[len - 1] = '\0';
    }

    fclose(f);
    return 0;
}

static int read_exe(pid_t pid, corex_proc_info_t *info)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/exe", (int)pid);

    ssize_t n = readlink(path, info->exe, sizeof(info->exe) - 1);
    if (n < 0) {
        /* Not fatal - some processes may not have an exe link */
        info->exe[0] = '\0';
        return 0;
    }
    info->exe[n] = '\0';
    return 0;
}

static int read_cmdline(pid_t pid, corex_proc_info_t *info)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        info->cmdline_len = 0;
        return 0;
    }

    ssize_t n = read(fd, info->cmdline, sizeof(info->cmdline) - 1);
    close(fd);

    if (n < 0) {
        info->cmdline_len = 0;
        return 0;
    }

    info->cmdline_len = (int)n;
    return 0;
}

static int read_coredump_filter(pid_t pid, corex_proc_info_t *info)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/coredump_filter", (int)pid);

    FILE *f = fopen(path, "r");
    if (!f) {
        /* Default filter if unreadable: 0x33 (kernel default) */
        info->coredump_filter = 0x33;
        return 0;
    }

    if (fscanf(f, "%x", &info->coredump_filter) != 1)
        info->coredump_filter = 0x33;

    fclose(f);
    return 0;
}

static int read_threads(pid_t pid, corex_proc_info_t *info)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/task", (int)pid);

    DIR *d = opendir(path);
    if (!d) {
        corex_set_error("Failed to open %s: %s", path, strerror(errno));
        return COREX_ERR_PROC_READ;
    }

    info->num_threads = 0;
    struct dirent *ent;

    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.')
            continue;
        if (info->num_threads >= COREX_MAX_THREADS)
            break;

        pid_t tid = (pid_t)atoi(ent->d_name);
        if (tid > 0) {
            info->tids[info->num_threads++] = tid;
        }
    }

    closedir(d);

    if (info->num_threads == 0) {
        corex_set_error("No threads found for PID %d", (int)pid);
        return COREX_ERR_NO_THREADS;
    }

    return 0;
}

int proc_info_read(pid_t pid, corex_proc_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->pid = pid;

    int rc;

    if ((rc = read_maps(pid, info)) != 0)   return rc;
    if ((rc = read_auxv(pid, info)) != 0)    return rc;
    if ((rc = read_status(pid, info)) != 0)  return rc;
    if ((rc = read_comm(pid, info)) != 0)    return rc;
    if ((rc = read_exe(pid, info)) != 0)     return rc;
    if ((rc = read_cmdline(pid, info)) != 0) return rc;
    if ((rc = read_coredump_filter(pid, info)) != 0) return rc;
    if ((rc = read_threads(pid, info)) != 0) return rc;

    return 0;
}
