// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License

//--------------------------------------------------------------------
//
// Monitor functions
//
//--------------------------------------------------------------------

#ifndef MONITOR_H
#define MONITOR_H

#include <signal.h>
#include <sys/ptrace.h>
#include <stdlib.h>

#include "ProcDumpConfiguration.h"

#define MAX_PROFILER_CONNECTIONS    50

// Monitor functions
void MonitorProcesses(struct ProcDumpConfiguration*self);
int CreateMonitorThread(struct ProcDumpConfiguration *self, enum TriggerType triggerType, void *(*monitorThread) (void *), void *arg);
int CreateMonitorThreads(struct ProcDumpConfiguration *self);
int StartMonitor(struct ProcDumpConfiguration* monitorConfig);
int WaitForQuit(struct ProcDumpConfiguration *self, int milliseconds);
int WaitForQuitOrEvent(struct ProcDumpConfiguration *self, struct Handle *handle, int milliseconds);
int WaitForAllMonitorsToTerminate(struct ProcDumpConfiguration *self);
int WaitForSignalThreadToTerminate(struct ProcDumpConfiguration *self);
int CancelRestrackThread(struct ProcDumpConfiguration *self);
bool IsQuit(struct ProcDumpConfiguration *self);
int SetQuit(struct ProcDumpConfiguration *self, int quit);
bool ContinueMonitoring(struct ProcDumpConfiguration *self);
bool BeginMonitoring(struct ProcDumpConfiguration *self);
bool MonitorDotNet(struct ProcDumpConfiguration *self);
char* GetThresholds(struct ProcDumpConfiguration *self);
char* GetClientData(struct ProcDumpConfiguration *self, char* fullDumpPath);
char* GetClientDataHelper(enum TriggerType triggerType, char* path, const char* format, ...);
bool ExitProcessMonitor(struct ProcDumpConfiguration* config, pthread_t processMonitor);

// Monitor worker threads
void *CommitMonitoringThread(void *thread_args /* struct ProcDumpConfiguration* */);
void *CpuMonitoringThread(void *thread_args /* struct ProcDumpConfiguration* */);
void *ThreadCountMonitoringThread(void *thread_args /* struct ProcDumpConfiguration* */);
void *FileDescriptorCountMonitoringThread(void *thread_args /* struct ProcDumpConfiguration* */);
void *SignalMonitoringThread(void *thread_args /* struct ProcDumpConfiguration* */);
void *TimerThread(void *thread_args /* struct ProcDumpConfiguration* */);
void *DotNetMonitoringThread(void *thread_args /* struct ProcDumpConfiguration* */);
void *RestrackManualTriggerThread(void *thread_args /* struct ProcDumpConfiguration* */);
void *RestrackThread(void *thread_args /* struct ProcDumpConfiguration* */);
void *ProcessMonitor(void *thread_args /* struct ProcDumpConfiguration* */);
void *WaitForProfilerCompletion(void *thread_args /* struct ProcDumpConfiguration* */);

#endif // MONITOR_H