// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License

//--------------------------------------------------------------------
//
// Sample program demonstrating the ProcDump on-demand dump API.
//
// Build (from the build directory after `make`):
//   clang++ -I../lib ../lib/sample.cpp libprocdump.a libcorex.a \
//       libbpf/src/libbpf/src/libbpf.a -lelf -lz -lpthread -o pd_sample
//
// Usage:
//   ./pd_sample <pid> <dump-path-prefix>
//
//--------------------------------------------------------------------

#include "ProcDumpLib.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char* argv[])
{
    if(argc < 3)
    {
        fprintf(stderr, "Usage: %s <pid> <dump-path-prefix>\n", argv[0]);
        return 2;
    }

    pid_t pid = (pid_t)atoi(argv[1]);
    const char* path = argv[2];

    char* error = NULL;
    int rc = pdWriteDump(pid, path, PD_DUMP_MASK_DEFAULT, /*bOverwrite*/ true, &error);
    if(rc != 0)
    {
        fprintf(stderr, "pdWriteDump failed: %s\n", error ? error : "(no detail)");
        pdFreeError(error);
        return 1;
    }

    printf("Dump generated successfully (prefix: %s)\n", path);
    return 0;
}
