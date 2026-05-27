/*
 * corex_internal.h - Shared internal declarations
 */
#ifndef COREX_INTERNAL_H
#define COREX_INTERNAL_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <elf.h>

/* Thread-local error message buffer */
#define COREX_ERR_BUF_SIZE 256
extern _Thread_local char corex_errbuf[COREX_ERR_BUF_SIZE];

void corex_set_error(const char *fmt, ...);

/* Maximum number of threads we track */
#define COREX_MAX_THREADS 1024

/* Maximum number of memory mappings */
#define COREX_MAX_MAPPINGS 4096

/* Chunk size for streaming memory to file (1 MB) */
#define COREX_MEM_CHUNK_SIZE (1 << 20)

#endif /* COREX_INTERNAL_H */
