// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License

//--------------------------------------------------------------------
//
// ProcDumpLibTestDriver - Thin CLI used by the integration tests to
// exercise the public on-demand dump API (pdWriteDump / pdFreeError)
// declared in crates/procdump-capi/include/ProcDumpLib.h.
//
// The driver performs no validation of its own; it simply forwards the
// arguments to pdWriteDump and reports the outcome so that the bash
// scenarios (lib_api_*.sh) can decide pass/fail. This keeps the test
// logic in the existing shell-based framework.
//
// Usage:
//   ProcDumpLibTestDriver <pid> <path> [mask] [overwrite] [stack-size]
//
//   pid        Target process id.
//   path       Full dump path prefix. Two sentinels are recognised:
//                NULL  -> pass a NULL pointer (negative test)
//                EMPTY -> pass an empty string  (negative test)
//   mask       Optional. coredump_filter bitmask, or "default" for
//              PD_DUMP_MASK_DEFAULT. Defaults to "default".
//   overwrite  Optional. 1 (default) to overwrite, 0 to fail if the
//              dump file already exists.
//   stack-size Optional. When non-zero, invoke pdWriteDump on a worker
//              thread with this stack size in bytes.
//
// Exit codes:
//   0   pdWriteDump returned success
//   1   pdWriteDump returned failure
//   2   bad usage
//
// On failure the error string returned by pdWriteDump is printed to
// stderr and released with pdFreeError. pdFreeError is always invoked
// (including with a NULL argument on success) to exercise that path.
//
//--------------------------------------------------------------------

#include "ProcDumpLib.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

struct DumpArguments
{
    pid_t pid;
    const char* path;
    int mask;
    bool overwrite;
    int result;
};

static void* writeDump(void* context)
{
    DumpArguments* arguments = static_cast<DumpArguments*>(context);
    char* error = NULL;
    arguments->result = pdWriteDump(arguments->pid,
                                    arguments->path,
                                    arguments->mask,
                                    arguments->overwrite,
                                    &error);

    if(arguments->result != 0)
    {
        fprintf(stderr,
                "pdWriteDump failed (rc=%d): %s\n",
                arguments->result, error ? error : "(no detail)");
    }
    else
    {
        printf("pdWriteDump succeeded (pid=%d)\n", (int)arguments->pid);
    }

    // Always exercise pdFreeError, even on success (error is NULL there).
    pdFreeError(error);
    return NULL;
}

int main(int argc, char* argv[])
{
    if(argc < 3)
    {
        fprintf(stderr,
            "Usage: %s <pid> <path> [mask] [overwrite] [stack-size]\n",
                argv[0]);
        return 2;
    }

    // Resolve the target pid.
    pid_t pid = (pid_t)strtol(argv[1], NULL, 10);

    // Resolve the dump path, honoring the NULL / EMPTY sentinels.
    const char* path = argv[2];
    if(strcmp(path, "NULL") == 0)
    {
        path = NULL;
    }
    else if(strcmp(path, "EMPTY") == 0)
    {
        path = "";
    }

    // Resolve the dump mask.
    int mask = PD_DUMP_MASK_DEFAULT;
    if(argc >= 4 && strcmp(argv[3], "default") != 0)
    {
        mask = (int)strtol(argv[3], NULL, 0);
    }

    // Resolve the overwrite flag (default: overwrite).
    bool overwrite = true;
    if(argc >= 5)
    {
        overwrite = (strtol(argv[4], NULL, 10) != 0);
    }

    size_t stackSize = 0;
    if(argc >= 6)
    {
        stackSize = (size_t)strtoull(argv[5], NULL, 10);
    }

    DumpArguments arguments = {pid, path, mask, overwrite, -1};
    if(stackSize == 0)
    {
        writeDump(&arguments);
    }
    else
    {
        pthread_attr_t attributes;
        int rc = pthread_attr_init(&attributes);
        if(rc != 0)
        {
            fprintf(stderr, "pthread_attr_init failed: %s\n", strerror(rc));
            return 2;
        }

        rc = pthread_attr_setstacksize(&attributes, stackSize);
        if(rc != 0)
        {
            fprintf(stderr, "pthread_attr_setstacksize failed: %s\n", strerror(rc));
            pthread_attr_destroy(&attributes);
            return 2;
        }

        pthread_t thread;
        rc = pthread_create(&thread, &attributes, writeDump, &arguments);
        pthread_attr_destroy(&attributes);
        if(rc != 0)
        {
            fprintf(stderr, "pthread_create failed: %s\n", strerror(rc));
            return 2;
        }

        rc = pthread_join(thread, NULL);
        if(rc != 0)
        {
            fprintf(stderr, "pthread_join failed: %s\n", strerror(rc));
            return 2;
        }
    }

    return arguments.result != 0 ? 1 : 0;
}
