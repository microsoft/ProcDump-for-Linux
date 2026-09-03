// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License

#ifndef PROCDUMP_LIB_H
#define PROCDUMP_LIB_H

#include <stdbool.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PD_DUMP_MASK_DEFAULT (-1)

// Immediately generates a core dump of the target process. On Linux, the
// generated filename has ".<pid>" appended to dumpPath. On failure, error may
// receive an allocated UTF-8 string that must be released with pdFreeError.
int pdWriteDump(
    pid_t processId,
    const char* dumpPath,
    int dumpMask,
    bool bOverwrite,
    char** error);

void pdFreeError(char* error);

#ifdef __cplusplus
}
#endif

#endif // PROCDUMP_LIB_H
