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
#ifdef __linux__
#include <sys/sysinfo.h>
#endif
#include <zconf.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <getopt.h>
#include <ctype.h>
#include <string.h>
#include <stdint.h>
#include <strings.h>
#include <syslog.h>
#include <limits.h>
#include <dirent.h>
#include <errno.h>
#include <libgen.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#ifdef __linux__
#include "Restrack.h"
#include "procdump_ebpf_common.h"
#endif

#include <unordered_map>

#define MAX_TRIGGERS 10
#define NO_PID INT_MAX
#define EMPTY_PROC_NAME "(null)"

#define MIN_POLLING_INTERVAL 1000   // default trigger polling interval (ms)
#define MAX_DUMP_COUNT 100          // maximum number of dumps that can be requested to be collected

// -------------------
// Structs
// -------------------

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

enum DiagnosticsLogTarget
{
    none,
    diag_syslog,
    diag_stdout
};

struct ProcDumpConfiguration
{
    // Process and System info
    pid_t ProcessId;
    pid_t ProcessGroup;         // -pgid
    bool bProcessGroup;         // -pgid

    char *ProcessName;
#ifdef __linux__    
    struct sysinfo SystemInfo;
#endif

    // Runtime Values
    int NumberOfDumpsCollecting; // Number of dumps we're collecting
    int NumberOfDumpsCollected; // Number of dumps we have collected
    int NumberOfLeakReportsCollected; // Number of leak reports we have collected
    bool bTerminated; // Do we know whether the process has terminated and subsequently whether we are terminating?
    char* socketPath;
    bool bExitProcessMonitor;

    // Quit
    int nQuit; // if not 0, then quit
    struct Handle evtQuit; // for signalling threads we are quitting
    int statusSocket;   // Socket used to wait for target process reporting status to procdump


    // Trigger behavior
    bool bTriggerThenSnoozeCPU;     // Detect+Trigger=>Wait N second=>[repeat]
    bool bTriggerThenSnoozeMemory;  // Detect+Trigger=>Wait N second=>[repeat]
    bool bTriggerThenSnoozeTimer;   // Detect+Trigger=>Wait N second=>[repeat]

    // Options
    int CpuThreshold;               // -c
    bool bCpuTriggerBelowValue;     // -cl
    int* MemoryThreshold;           // -m
    int MemoryThresholdCount;
    int MemoryCurrentThreshold;
    bool bMemoryTriggerBelowValue;  // -m or -ml
    bool bMonitoringGCMemory;       // -gcm
    int DumpGCGeneration;           // -gcgen
    int ThresholdSeconds;           // -s
    bool bTimerThreshold;           // -s
    int NumberOfDumpsToCollect;     // -n
    bool WaitingForProcessName;     // -w
    DiagnosticsLogTarget DiagnosticsLoggingEnabled; // -log
    int ThreadThreshold;            // -tc
    int FileDescriptorThreshold;    // -fc
    int* SignalNumber;              // -sig
    int SignalCount;
    int PollingInterval;            // -pf
    char *CoreDumpPath;             //
    char *CoreDumpName;             //
    bool bOverwriteExisting;        // -o
    bool bDumpOnException;          // -e
    char *ExceptionFilter;          // -f (unfortunately we named this ExceptionFilter event hough it can be used for other include filters as well)
    char *ExcludeFilter;            // -fx (exclude filter)
    bool bRestrackEnabled;          // -restrack
    bool bRestrackGenerateDump;     // -restrack generate dump flag
    bool bRestrackDecimalFormat;    // -restrack use decimal format instead of hex
    bool bLeakReportInProgress;
    int SampleRate;                 // Record every X resource allocation in restrack
    int CoreDumpMask;               // -mc (core dump mask)

    //
    // Keeps track of the memory allocations when -restrack is specified.
    // Access must be protected by memAllocMapMutex.
    //
#ifdef __linux__
    std::unordered_map<uintptr_t, ResourceInformation*> memAllocMap;
    pthread_mutex_t memAllocMapMutex;
#endif

    // multithreading
    // set max number of concurrent dumps on init (default to 1)
    int nThreads;
    struct TriggerThread Threads[MAX_TRIGGERS];
    struct Handle semAvailableDumpSlots;
    pthread_mutex_t ptrace_mutex;
    pthread_cond_t dotnetCond;
    pthread_mutex_t dotnetMutex;
    bool bSocketInitialized;

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

int GetOptions(struct ProcDumpConfiguration *self, int argc, char *argv[]);
bool PrintConfiguration(struct ProcDumpConfiguration *self);
void ApplyDefaults(struct ProcDumpConfiguration *self);  // Call this after GetOptions has been called to set default values
void FreeProcDumpConfiguration(struct ProcDumpConfiguration *self);
struct ProcDumpConfiguration * CopyProcDumpConfiguration(struct ProcDumpConfiguration *self);
void InitProcDumpConfiguration(struct ProcDumpConfiguration *self);
void InitProcDump();
void ExitProcDump();
void PrintBanner();
int PrintUsage();


#endif // PROCDUMPCONFIGURATION_H
