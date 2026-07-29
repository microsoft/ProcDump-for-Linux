// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License

//--------------------------------------------------------------------
//
// ProcDumpLib - Implementation of the public on-demand dump API.
//
// This wraps ProcDump's existing core dump engine (CoreDumpWriter) behind
// a small, dependency-free C API so that other programs can link against
// libprocdump.a and generate a dump without spawning the procdump tool.
//
//--------------------------------------------------------------------

#include "Includes.h"
#include "ProcDumpLib.h"

#include <libgen.h>

//--------------------------------------------------------------------
//
// pdWriteDump - Immediately generate a core dump of the target process.
//
//--------------------------------------------------------------------
extern "C" int pdWriteDump(
    pid_t       processId,
    const char* dumpPath,
    int         dumpMask,
    bool        bOverwrite,
    char**      error)
{
    if(error != NULL)
    {
        *error = NULL;
    }

    if(processId <= 0 || dumpPath == NULL || dumpPath[0] == '\0')
    {
        if(error != NULL)
        {
            *error = strdup("Invalid argument: a valid processId and dumpPath are required.");
        }
        return -1;
    }

    //
    // The existing writer constructs the final dump name from a directory
    // (CoreDumpPath) plus a file name prefix (CoreDumpName). Split the caller
    // supplied full path accordingly. dirname()/basename() may modify their
    // argument, so operate on copies.
    //
    char* dirCopy = strdup(dumpPath);
    char* baseCopy = strdup(dumpPath);
    if(dirCopy == NULL || baseCopy == NULL)
    {
        free(dirCopy);
        free(baseCopy);
        if(error != NULL)
        {
            *error = strdup("Out of memory.");
        }
        return -1;
    }

    char* dir = dirname(dirCopy);
    char* base = basename(baseCopy);

    //
    // Build a minimal configuration sufficient to generate a single dump.
    // Value-initialize so every POD field defaults to 0/false (matching the
    // zero-initialized global config the procdump tool relies on); a few
    // fields read during monitoring (e.g. bTerminated) are not set by
    // InitProcDumpConfiguration.
    //
    struct ProcDumpConfiguration config = {};
    InitProcDumpConfiguration(&config);

    config.ProcessId = processId;
    config.ProcessName = GetProcessName(processId);
    if(config.ProcessName == NULL)
    {
        config.ProcessName = strdup("process");
    }
    config.CoreDumpPath = strdup(dir);
    config.CoreDumpName = strdup(base);
    config.NumberOfDumpsToCollect = 1;
    config.NumberOfDumpsCollected = 0;
    config.bOverwriteExisting = bOverwrite;
    config.CoreDumpMask = dumpMask;
    config.bUseGcore = false;
    config.nQuit = 0;
    config.bTerminated = false;

    free(dirCopy);
    free(baseCopy);

    int ret = 0;
    struct CoreDumpWriter* writer = NewCoreDumpWriter(MANUAL, &config);

    char* dumpName = WriteCoreDump(writer);
    if(dumpName == NULL)
    {
        ret = -1;
        if(error != NULL)
        {
            if(writer->ErrorMessage != NULL)
            {
                *error = strdup(writer->ErrorMessage);
            }
            else
            {
                *error = strdup("Failed to generate core dump.");
            }
        }
    }
    else
    {
        free(dumpName);
    }

    if(writer->ErrorMessage != NULL)
    {
        free(writer->ErrorMessage);
    }
    free(writer);

    // FreeProcDumpConfiguration releases ProcessName, CoreDumpPath and CoreDumpName.
    FreeProcDumpConfiguration(&config);

    return ret;
}

//--------------------------------------------------------------------
//
// pdFreeError - Free an error string returned by pdWriteDump.
//
//--------------------------------------------------------------------
extern "C" void pdFreeError(char* error)
{
    if(error != NULL)
    {
        free(error);
    }
}
