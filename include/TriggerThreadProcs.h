// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License

//--------------------------------------------------------------------
//
// header - thread processes
//
//--------------------------------------------------------------------

#ifndef TRIGGER_THREAD_PROCS_H
#define TRIGGER_THREAD_PROCS_H

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <memory.h>
#include <zconf.h>
#include <sys/sysinfo.h>
#include <sys/ptrace.h>
#include <unistd.h>

#include "CoreDumpWriter.h"
#include "Events.h"
#include "ProcDumpConfiguration.h"
#include "Process.h"
#include "Logging.h"

// worker thread process for monitoring memory commit
void *CommitMonitoringThread(void *thread_args /* struct ProcDumpConfiguration* */);
void *CpuMonitoringThread(void *thread_args /* struct ProcDumpConfiguration* */);
void *ThreadCountMonitoringThread(void *thread_args /* struct ProcDumpConfiguration* */);
void *FileDescriptorCountMonitoringThread(void *thread_args /* struct ProcDumpConfiguration* */);
void *SignalMonitoringThread(void *thread_args /* struct ProcDumpConfiguration* */);
void *TimerThread(void *thread_args /* struct ProcDumpConfiguration* */);

#endif // TRIGGER_THREAD_PROCS_H