/*
 * proc_info.h - Read process information from /proc
 */
#ifndef PROC_INFO_H
#define PROC_INFO_H

#include "corex_internal.h"

/* A single memory mapping from /proc/[pid]/maps */
typedef struct {
    uint64_t    start;
    uint64_t    end;
    uint32_t    flags;      /* PF_R, PF_W, PF_X */
    uint64_t    offset;
    uint8_t     is_shared;      /* 's' in perms (vs 'p' for private) */
    uint8_t     is_file_backed; /* has a real file path (inode > 0) */
    uint8_t     should_dump;    /* set after applying coredump_filter */
    char        path[512];
} corex_mapping_t;

/* Process-level information */
typedef struct {
    pid_t       pid;
    pid_t       ppid;
    pid_t       pgrp;           /* Process group ID */
    pid_t       sid;            /* Session ID */
    uid_t       uid;            /* Real UID */
    gid_t       gid;            /* Real GID */
    int         num_threads;
    char        comm[16];       /* From /proc/[pid]/comm */
    char        exe[512];       /* From /proc/[pid]/exe symlink */
    char        cmdline[4096];  /* From /proc/[pid]/cmdline */
    int         cmdline_len;

    /* Memory mappings */
    int             num_mappings;
    corex_mapping_t mappings[COREX_MAX_MAPPINGS];

    /* Auxiliary vector */
    uint8_t     auxv[4096];
    size_t      auxv_len;

    /* Coredump filter from /proc/[pid]/coredump_filter */
    uint32_t    coredump_filter;

    /* Thread IDs */
    pid_t       tids[COREX_MAX_THREADS];
} corex_proc_info_t;

/* Read all process information for the given PID.
 * Returns 0 on success, negative COREX_ERR_* on failure. */
int proc_info_read(pid_t pid, corex_proc_info_t *info);

#endif /* PROC_INFO_H */
