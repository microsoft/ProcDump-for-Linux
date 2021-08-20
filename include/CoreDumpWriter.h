// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

//--------------------------------------------------------------------
//
// Core dump orchestrator
//
//--------------------------------------------------------------------

#ifndef CORE_DUMP_WRITER_H
#define CORE_DUMP_WRITER_H

#include <ctype.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdint.h>

#include "Handle.h"
#include "ProcDumpConfiguration.h"

#define DATE_LENGTH 26
#define MAX_LINES 15
#define BUFFER_LENGTH 1024

#define CORECLR_DUMPTYPE_FULL 4
#define CORECLR_DUMPLOGGING_OFF 0
#define CORECLR_DIAG_IPCHEADER_SIZE 24

// Magic version for the IpcHeader struct
struct MagicVersion
{
    uint8_t Magic[14];
};

// The header to be associated with every command and response
// to/from the diagnostics server
struct IpcHeader
{
    union
    {
        struct MagicVersion _magic;
        uint8_t  Magic[14];  // Magic Version number
    };

    uint16_t Size;       // The size of the incoming packet, size = header + payload size
    uint8_t  CommandSet; // The scope of the Command.
    uint8_t  CommandId;  // The command being sent
    uint16_t Reserved;   // reserved for future use
};


enum ECoreDumpType {
    COMMIT,                 // trigger on memory threshold
    CPU,                    // trigger on CPU threshold
    THREAD,                 // trigger on thread count
    FILEDESC,               // trigger on file descriptor count
    SIGNAL,                 // trigger on signal
    TIME,                   // trigger on time interval
    MANUAL                  // manual trigger
};

struct CoreDumpWriter {
    struct ProcDumpConfiguration *Config;
    enum ECoreDumpType Type;
};

struct CoreDumpWriter *NewCoreDumpWriter(enum ECoreDumpType type, struct ProcDumpConfiguration *config);

int WriteCoreDump(struct CoreDumpWriter *self);

#endif // CORE_DUMP_WRITER_H