// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License

//--------------------------------------------------------------------
//
// The global configuration structure and utilities header
//
//--------------------------------------------------------------------

#ifndef PROCDUMPCONFIGURATION_H
#define PROCDUMPCONFIGURATION_H

#include <stdbool.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <zconf.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <getopt.h>
#include <ctype.h>
#include <string.h>
#include <signal.h>
#include <syslog.h>
#include <limits.h>
#include <dirent.h>
#include <errno.h>
#include <libgen.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/queue.h>

#include "Handle.h"
#include "TriggerThreadProcs.h"
#include "Process.h"
#include "Logging.h"

#define MAX_TRIGGERS 10
#define NO_PID INT_MAX
#define MAX_CMDLINE_LEN 4096+1
#define EMPTY_PROC_NAME "(null)"
#define MIN_KERNEL_VERSION 3
#define MIN_KERNEL_PATCH 5

#define MIN_POLLING_INTERVAL 1000   // default trigger polling interval (ms)

#define PID_MAX_KERNEL_CONFIG "/proc/sys/kernel/pid_max"

// -------------------
// Structs
// -------------------

enum TriggerType
{
    Processor,
    Commit,
    Timer,
    Signal,
    ThreadCount,
    FileDescriptorCount
};

struct TriggerThread
{
    pthread_t thread;
    enum TriggerType trigger; 
};

struct MonitoredProcessMapEntry
{
    bool active;
    long long starttime; 
};

struct ProcDumpConfiguration {
    // Process and System info
    pid_t ProcessId;            // -p
    pid_t ProcessGroupId;       // -g
    char *ProcessName;          // -w
    struct sysinfo SystemInfo;

    // Runtime Values
    int NumberOfDumpsCollecting; // Number of dumps we're collecting
    int NumberOfDumpsCollected; // Number of dumps we have collected
    bool bTerminated; // Do we know whether the process has terminated and subsequently whether we are terminating?

    // Quit
    int nQuit; // if not 0, then quit
    struct Handle evtQuit; // for signalling threads we are quitting


    // Trigger behavior
    bool bTriggerThenSnoozeCPU;     // Detect+Trigger=>Wait N second=>[repeat]
    bool bTriggerThenSnoozeMemory;  // Detect+Trigger=>Wait N second=>[repeat]
    bool bTriggerThenSnoozeTimer;   // Detect+Trigger=>Wait N second=>[repeat]

    // Options
    int CpuUpperThreshold;          // -C
    int CpuLowerThreshold;          // -c    
    int MemoryThreshold;            // -M
    bool bMemoryTriggerBelowValue;  // -m
    int ThresholdSeconds;           // -s
    bool bTimerThreshold;           // -s
    int NumberOfDumpsToCollect;     // -n
    bool WaitingForProcessName;     // -w
    bool DiagnosticsLoggingEnabled; // -d
    int ThreadThreshold;            // -T
    int FileDescriptorThreshold;    // -F
    int SignalNumber;               // -G    
    int PollingInterval;            // -I
    char *CoreDumpPath;             // -o
    char *CoreDumpName;             // -o

    // multithreading
    // set max number of concurrent dumps on init (default to 1)
    int nThreads;
    struct TriggerThread Threads[MAX_TRIGGERS];
    struct Handle semAvailableDumpSlots; 
    pthread_mutex_t ptrace_mutex;

    // Events
    // use these to mimic WaitForSingleObject/MultibleObjects from WinApi
    struct Handle evtCtrlHandlerCleanupComplete;
    struct Handle evtBannerPrinted;
    struct Handle evtConfigurationPrinted;
    struct Handle evtDebugThreadInitialized;
    struct Handle evtStartMonitoring;

    // External
    pid_t gcorePid;
};

struct ConfigQueueEntry {
    struct ProcDumpConfiguration * config;

    TAILQ_ENTRY(ConfigQueueEntry) element;
};

int GetOptions(struct ProcDumpConfiguration *self, int argc, char *argv[]);
char * GetProcessName(pid_t pid);
pid_t GetProcessPgid(pid_t pid);
bool LookupProcessByPid(struct ProcDumpConfiguration *self);
bool LookupProcessByPgid(struct ProcDumpConfiguration *self);
void MonitorProcesses(struct ProcDumpConfiguration *self);
int CreateProcessViaDebugThreadAndWaitUntilLaunched(struct ProcDumpConfiguration *self);
int CreateTriggerThreads(struct ProcDumpConfiguration *self);
int WaitForQuit(struct ProcDumpConfiguration *self, int milliseconds);
int WaitForQuitOrEvent(struct ProcDumpConfiguration *self, struct Handle *handle, int milliseconds);
int WaitForAllMonitoringThreadsToTerminate(struct ProcDumpConfiguration *self);
int WaitForSignalThreadToTerminate(struct ProcDumpConfiguration *self);
bool IsQuit(struct ProcDumpConfiguration *self);
int SetQuit(struct ProcDumpConfiguration *self, int quit);
bool PrintConfiguration(struct ProcDumpConfiguration *self);
bool ContinueMonitoring(struct ProcDumpConfiguration *self);
bool BeginMonitoring(struct ProcDumpConfiguration *self);

void FreeProcDumpConfiguration(struct ProcDumpConfiguration *self);
struct ProcDumpConfiguration * CopyProcDumpConfiguration(struct ProcDumpConfiguration *self);
void InitProcDumpConfiguration(struct ProcDumpConfiguration *self);
void InitProcDump();
void ExitProcDump();

void PrintBanner();
int PrintUsage(struct ProcDumpConfiguration *self);
bool IsValidNumberArg(const char *arg);
bool CheckKernelVersion();
int GetMaximumPID();

#endif // PROCDUMPCONFIGURATION_H
