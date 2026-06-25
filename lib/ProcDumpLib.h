// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License

//--------------------------------------------------------------------
//
// ProcDumpLib - Public API for on-demand core dump generation.
//
// Link against libprocdump.a and include this header to generate a core
// dump of a target process programmatically (without running the procdump
// command-line tool).
//
//--------------------------------------------------------------------

#ifndef PROCDUMP_LIB_H
#define PROCDUMP_LIB_H

#include <sys/types.h>  // pid_t
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//
// Pass as 'dumpMask' to leave the target process' coredump_filter unchanged.
//
#define PD_DUMP_MASK_DEFAULT (-1)

//---------------------------------------------------------------------------------------------------------
// pdWriteDump
//
// Immediately generates a core dump of the target process.
//
//  processId   - Process ID of the target process.
//  dumpPath    - Full path of the resulting dump file. On Linux the file that is actually produced
//                has ".<pid>" appended to this path (consistent with the procdump tool).
//  dumpMask    - Linux coredump_filter bitmask to apply while generating the dump, or
//                PD_DUMP_MASK_DEFAULT (-1) to leave the process' existing filter unchanged.
//                See https://man7.org/linux/man-pages/man5/core.5.html (coredump_filter) for bit meanings.
//  bOverwrite  - If true, overwrite an existing dump file of the same name; otherwise the call fails
//                if the dump file already exists.
//  error       - Optional. If non-NULL and the function fails, receives a newly allocated, NUL-terminated
//                UTF-8 error string that the caller must release with pdFreeError(). Set to NULL on success.
//
//  Returns 0 on success, non-zero on failure.
//---------------------------------------------------------------------------------------------------------
int pdWriteDump(
    pid_t       processId,
    const char* dumpPath,
    int         dumpMask,
    bool        bOverwrite,
    char**      error);

//---------------------------------------------------------------------------------------------------------
// pdFreeError
//
// Frees an error string returned by pdWriteDump.
//
//  error   - Pointer to the error string to free. May be NULL.
//---------------------------------------------------------------------------------------------------------
void pdFreeError(
    char* error);

#ifdef __cplusplus
}
#endif

#endif // PROCDUMP_LIB_H
