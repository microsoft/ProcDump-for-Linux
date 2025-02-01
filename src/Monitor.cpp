// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License

//--------------------------------------------------------------------
//
// Monitor functions
//
//--------------------------------------------------------------------
#define _Bool bool
#ifdef __linux__
#include "procdump_ebpf.skel.h"
#endif

#include "Includes.h"

#include <vector>
#include <string>
#include <memory>

#ifdef __APPLE__
#include <libproc.h>
#endif

static pthread_t sig_thread_id;

extern struct ProcDumpConfiguration g_config;
extern struct ProcDumpConfiguration * target_config;
extern sigset_t sig_set;

//
// Set when a SIGINT is received.
//
bool g_sigint = false;

//
// List of all active monitor configurations.
// All access to this map must be protected by activeConfigurationMutex
//
std::unordered_map<int, ProcDumpConfiguration*> activeConfigurations;
pthread_mutex_t activeConfigurationsMutex;

//
// Map of which processes are being monitored
//
std::unordered_map<int, MonitoredProcessMapEntry> monitoredProcessMap;

//------------------------------------------------------------------------------------------------------
//
// SignalThread - Thread for handling graceful Async signals (e.g., SIGINT, SIGTERM)
//
// Turn off address sanitation for this function as a result of a likely bug with pthread_cancel. The
// incorrect error that the address sanitizer gives is:
//==314250==AddressSanitizer CHECK failed: ../../../../src/libsanitizer/asan/asan_thread.cpp:367 "((ptr[0] == kCurrentStackFrameMagic)) != (0)" (0x0, 0x0)
//    #0 0x7f6cd0a30988 in AsanCheckFailed ../../../../src/libsanitizer/asan/asan_rtl.cpp:74
//    #1 0x7f6cd0a5130e in __sanitizer::CheckFailed(char const*, int, char const*, unsigned long long, unsigned long long) ../../../../src/libsanitizer/sanitizer_common/sanitizer_termination.cpp:78
//    #2 0x7f6cd0a3610c in __asan::AsanThread::GetStackFrameAccessByAddr(unsigned long, __asan::AsanThread::StackFrameAccess*) ../../../../src/libsanitizer/asan/asan_thread.cpp:367
//    #3 0x7f6cd09a0e9b in __asan::GetStackAddressInformation(unsigned long, unsigned long, __asan::StackAddressDescription*) ../../../../src/libsanitizer/asan/asan_descriptions.cpp:203
//    #4 0x7f6cd09a22d8 in __asan::AddressDescription::AddressDescription(unsigned long, unsigned long, bool) ../../../../src/libsanitizer/asan/asan_descriptions.cpp:455
//    #5 0x7f6cd09a22d8 in __asan::AddressDescription::AddressDescription(unsigned long, unsigned long, bool) ../../../../src/libsanitizer/asan/asan_descriptions.cpp:439
//    #6 0x7f6cd09a4a84 in __asan::ErrorGeneric::ErrorGeneric(unsigned int, unsigned long, unsigned long, unsigned long, unsigned long, bool, unsigned long) ../../../../src/libsanitizer/asan/asan_errors.cpp:389
//    #7 0x7f6cd0a2ffa5 in __asan::ReportGenericError(unsigned long, unsigned long, unsigned long, unsigned long, bool, unsigned long, unsigned int, bool) ../../../../src/libsanitizer/asan/asan_report.cpp:476
//    #8 0x7f6cd09c6fe8 in __interceptor_sigaltstack ../../../../src/libsanitizer/sanitizer_common/sanitizer_common_interceptors.inc:9986
//    #9 0x7f6cd0a45867 in __sanitizer::UnsetAlternateSignalStack() ../../../../src/libsanitizer/sanitizer_common/sanitizer_posix_libcdep.cpp:195
//    #10 0x7f6cd0a3560c in __asan::AsanThread::Destroy() ../../../../src/libsanitizer/asan/asan_thread.cpp:104
//    #11 0x7f6cd07dc710 in __GI___nptl_deallocate_tsd nptl/nptl_deallocate_tsd.c:73
//    #12 0x7f6cd07dc710 in __GI___nptl_deallocate_tsd nptl/nptl_deallocate_tsd.c:22
//    #13 0x7f6cd07df9c9 in start_thread nptl/pthread_create.c:453
//    #14 0x7f6cd08719ff  (/lib/x86_64-linux-gnu/libc.so.6+0x1269ff)
//------------------------------------------------------------------------------------------------------
__attribute__((no_sanitize("address")))
void* SignalThread(void *input)
{
    Trace("SignalThread: Enter [id=%d]", gettid());
    int sig_caught, rc;

    if ((rc = sigwait(&sig_set, &sig_caught)) != 0) {
        Log(error, "Failed to wait on signal");
        exit(-1);
    }

    switch (sig_caught)
    {
        case SIGINT:
            Trace("SignalThread: Got a SIGINT");
            g_sigint = true;

            // In case of CTRL-C we need to iterate over all the outstanding monitors and handle them appropriately
            pthread_mutex_lock(&activeConfigurationsMutex);
            for (auto it = activeConfigurations.begin(); it != activeConfigurations.end(); it++)
            {
                if(!IsQuit(it->second)) SetQuit(it->second, 1);

                if(it->second->gcorePid != NO_PID) {
                    Log(info, "Shutting down gcore");
                    if((rc = kill(-it->second->gcorePid, SIGKILL)) != 0) {            // pass negative PID to kill entire PGRP with value of gcore PID
                        Log(error, "Failed to shutdown gcore.");
                    }
                }

                // Need to make sure we detach from ptrace (if not attached it will silently fail)
                // To avoid situations where we have intercepted a signal and CTRL-C is hit, we synchronize
                // access to the signal path (in SignalMonitoringThread). Note, there is still a race but
                // acceptable since it is very unlikely to occur. We also cancel the SignalMonitorThread to
                // break it out of waitpid call.
                if(it->second->SignalCount > 0)
                {
                    for(int i=0; i<it->second->nThreads; i++)
                    {
                        if(it->second->Threads[i].trigger == Signal)
                        {
                            pthread_mutex_lock(&it->second->ptrace_mutex);
#ifdef __linux__      
                            ptrace(PTRACE_DETACH, it->second->ProcessId, 0, 0);
#endif                            
                            pthread_mutex_unlock(&it->second->ptrace_mutex);

                            if ((rc = pthread_cancel(it->second->Threads[i].thread)) != 0) {
                                Log(error, "An error occurred while cancelling SignalMonitorThread.\n");
                                exit(-1);
                            }
                        }
                    }
                }
            }
            pthread_mutex_unlock(&activeConfigurationsMutex);

            Log(info, "Quit");
            SetQuit(&g_config, 1);                  // Make sure to signal the global config
            break;

        default:
            Trace("Unexpected signal %d", sig_caught);
            break;
    }

    Trace("SignalThread: Exit [id=%d]", gettid());
    return NULL;
}

//--------------------------------------------------------------------
//
// GetNewMonitorConfiguration
// Gets a new configuration based off of the passed in config.
// Also adds the configuration to both activeConfigurations and
// monitoredProcessMap meaning it is now considered an active and monitored
// process.
//
//--------------------------------------------------------------------
ProcDumpConfiguration* GetNewMonitorConfiguration(ProcDumpConfiguration* sourceConfig, char* processName, int procPid, unsigned long long starttime)
{
    ProcDumpConfiguration* config = CopyProcDumpConfiguration(sourceConfig);
    if(config == NULL)
    {
        Log(error, INTERNAL_ERROR);
        Trace("MonitorProcesses: failed to alloc struct for process.");
        return NULL;
    }

    // populate fields for this target
    if(procPid != -1)
    {
        config->ProcessId = procPid;
    }

    if(processName != NULL)
    {
        config->ProcessName = processName;
    }

    // insert config into queue
    pthread_mutex_lock(&activeConfigurationsMutex);
    activeConfigurations[config->ProcessId] = config;
    monitoredProcessMap[config->ProcessId].active = true;
    monitoredProcessMap[config->ProcessId].starttime = starttime;
    pthread_mutex_unlock(&activeConfigurationsMutex);

    return config;
}

//--------------------------------------------------------------------
//
// CheckAccess
//
// Checks to make sure we have access to the target process.
//
//--------------------------------------------------------------------
bool CheckAccess(struct ProcDumpConfiguration *self)
{
    struct ProcessStat proc;
    if(GetProcessStat(self->ProcessId, &proc) == false)
    {
        return false;
    }

    uid_t euid = geteuid();
    if(euid == 0 || euid == proc.effective_uid)
    {
        return true;
    }

    return false;
}

//--------------------------------------------------------------------
//
// MonitorProcesses
// MonitorProcess is the starting point of where the monitors get
// created. It uses a list to store all the monitors that are active.
// All monitors must go on this list as there are other places (for
// example, SignalThread) that relies on all active monitors to be part
// of the list.
//
//--------------------------------------------------------------------
void MonitorProcesses(struct ProcDumpConfiguration *self)
{
    if (self->WaitingForProcessName)    Log(info, "Waiting for processes '%s' to launch\n", self->ProcessName);
    if (self->bProcessGroup == true)    Log(info, "Monitoring processes of PGID '%d'\n", self->ProcessGroup);

    // allocate list of configs for process monitoring
    int numMonitoredProcesses = 0;

    monitoredProcessMap.reserve(5000);      // assume 5000 processes

    // Create a signal handler thread where we handle shutdown as a result of SIGINT.
    // Note: We only create ONE per instance of procdump rather than per monitor.
    if((pthread_create(&sig_thread_id, NULL, SignalThread, (void *)self))!= 0)
    {
        Log(error, INTERNAL_ERROR);
        Trace("CreateMonitorThreads: failed to create SignalThread.");
        return;
    }

    Log(info, "Press Ctrl-C to end monitoring without terminating the process(es).");

    if(!self->WaitingForProcessName && !self->bProcessGroup)
    {
        //
        // Monitoring single process (-p)
        //

        //
        // Make sure target process exists
        //

        // If we have a process name find it to make sure it exists
        if(self->ProcessName)
        {
            if(!LookupProcessByName(self->ProcessName))
            {
                Log(error, "No process matching the specified name (%s) can be found.", self->ProcessName);
                return;
            }

            // Set the process ID so the monitor can target.
            self->ProcessId = LookupProcessPidByName(g_config.ProcessName);
        }
        else
        {
            if (self->ProcessId != NO_PID && LookupProcessByPid(self->ProcessId))
            {
                self->ProcessName = GetProcessName(self->ProcessId);
            }
            else
            {
                Log(error, "No process matching the specified PID (%d) can be found.", self->ProcessId);
                return;
            }
        }

        ProcDumpConfiguration* config = GetNewMonitorConfiguration(self, NULL, -1, 0);
        if(config == NULL)
        {
            Log(error, INTERNAL_ERROR);
            Trace("MonitorProcesses: failed to get new monitor configuration.");
            return;
        }

        // print config here
        PrintConfiguration(config);

        if(StartMonitor(config)!=0)
        {
            Trace("MonitorProcesses: Failed to start the monitor.");
            Log(error, "MonitorProcesses: Failed to start the monitor.");
            return;
        }

        WaitForAllMonitorsToTerminate(config);
        Log(info, "Stopping monitor for process %s (%d)", config->ProcessName, config->ProcessId);
        WaitForSignalThreadToTerminate(config);

        pthread_mutex_lock(&activeConfigurationsMutex);
        activeConfigurations.erase(config->ProcessId);
        monitoredProcessMap[config->ProcessId].active = false;
        pthread_mutex_unlock(&activeConfigurationsMutex);
        FreeProcDumpConfiguration(config);
        free(config);
    }
    else
    {
        // print config here
        PrintConfiguration(self);

        do
        {
            // Multi process monitoring

            // If we are monitoring for PGID, validate the root process exists
            if(self->bProcessGroup && !LookupProcessByPgid(self->ProcessGroup)) {
                Log(error, "No process matching the specified PGID can be found.");
                PrintUsage();
                return;
            }

            // Iterate over all running processes
#ifdef __linux__            
            struct dirent ** nameList;
            int numEntries = scandir("/proc/", &nameList, FilterForPid, alphasort);
#else
            pid_t *nameList;
            int numEntries = GetRunningPids(&nameList);
#endif            
            for (int i = 0; i < numEntries; i++)
            {
                pid_t procPid;
#ifdef __linux__
                if(!ConvertToInt(nameList[i]->d_name, &procPid))
                {
                    continue;
                }
#else
                procPid = nameList[i];
#endif

                if(self->bProcessGroup)
                {
                    // We are monitoring a process group (-g)
                    pid_t pgid = GetProcessPgid(procPid);
                    if(pgid != NO_PID && pgid == self->ProcessGroup)
                    {
                        struct ProcessStat procStat;
                        bool ret = GetProcessStat(procPid, &procStat);

                        // Note: To solve the PID reuse case, we uniquely identify an entry via {PID}{starttime}
                        if(ret && (monitoredProcessMap[procPid].active == false || monitoredProcessMap[procPid].starttime != procStat.starttime))
                        {
                            ProcDumpConfiguration* config = GetNewMonitorConfiguration(self, GetProcessName(procPid), procPid, procStat.starttime);
                            if(config == NULL)
                            {
                                Log(error, INTERNAL_ERROR);
                                Trace("MonitorProcesses: failed to get new monitor configuration.");
                                return;
                            }

                            if(StartMonitor(config)!=0)
                            {
                                Log(error, INTERNAL_ERROR);
                                Trace("MonitorProcesses: Failed to start the monitor.");
                                return;
                            }

                            numMonitoredProcesses++;
                        }
                    }
                }
                else if(self->WaitingForProcessName)
                {
                    // We are monitoring for a process name (-w)
                    char *nameForPid = GetProcessName(procPid);

                    // check to see if process name matches target
                    if (nameForPid && strcmp(nameForPid, self->ProcessName) == 0)
                    {
                        struct ProcessStat procStat;
                        bool ret = GetProcessStat(procPid, &procStat);

                        // Note: To solve the PID reuse case, we uniquely identify an entry via {PID}{starttime}
                        if(ret && (monitoredProcessMap[procPid].active == false || monitoredProcessMap[procPid].starttime != procStat.starttime))
                        {
                            ProcDumpConfiguration* config = GetNewMonitorConfiguration(self, strdup(nameForPid), procPid, procStat.starttime);
                            if(config == NULL)
                            {
                                Log(error, INTERNAL_ERROR);
                                Trace("MonitorProcesses: failed to get new monitor configuration.");
                                return;
                            }

                            if(StartMonitor(config)!=0)
                            {
                                Log(error, INTERNAL_ERROR);
                                Trace("MonitorProcesses: Failed to start the monitor.");
                                return;
                            }

                            numMonitoredProcesses++;
                        }
                    }
                }
            }

            // clean up namelist
#ifdef __linux__            
            for (int i = 0; i < numEntries; i++)
            {
                free(nameList[i]);
            }
#endif            
            if(numEntries!=-1)
            {
                free(nameList);
            }

            // cleanup process configs for child processes that have exited or for monitors that have captured N dumps
            pthread_mutex_lock(&activeConfigurationsMutex);
            for (auto it = activeConfigurations.begin(); it != activeConfigurations.end(); )
            {
                if (it->second->bTerminated ||
                    it->second->nQuit ||
                    it->second->NumberOfDumpsCollected == it->second->NumberOfDumpsToCollect ||
                    it->second->NumberOfLeakReportsCollected == it->second->NumberOfDumpsToCollect)
                {
                    Log(info, "Stopping monitors for process: %s (%d)", it->second->ProcessName, it->second->ProcessId);
                    WaitForAllMonitorsToTerminate(it->second);
                    FreeProcDumpConfiguration(it->second);
                    delete it->second;

                    it = activeConfigurations.erase(it);
                    numMonitoredProcesses--;
                }
                else
                {
                    ++it;
                }
            }
            pthread_mutex_unlock(&activeConfigurationsMutex);

            // Exit if we are monitoring PGID and there are no more processes to monitor.
            // If we are monitoring for processes based on a process name we keep monitoring
            if(numMonitoredProcesses == 0 && self->WaitingForProcessName == false)
            {
                break;
            }

            // Wait for the polling interval the user specified before we check again
            sleep(g_config.PollingInterval / 1000);

        // We keep iterating while we have processes to monitor (in case of -g <pgid>) or if process name has
        // been specified (-w) in which case we keep monitoring until CTRL-C or finally if we have a quit signal.
        } while ((numMonitoredProcesses >= 0 || self->WaitingForProcessName == true) && !IsQuit(&g_config));

        // cleanup monitoring queue
        pthread_mutex_lock(&activeConfigurationsMutex);

        for (auto it = activeConfigurations.begin(); it != activeConfigurations.end(); )
        {
            if (it->second->bTerminated ||
                it->second->nQuit ||
                it->second->NumberOfDumpsCollected == it->second->NumberOfDumpsToCollect ||
                it->second->NumberOfLeakReportsCollected == it->second->NumberOfDumpsToCollect)
            {
                SetQuit(it->second, 1);
                WaitForAllMonitorsToTerminate(it->second);

                FreeProcDumpConfiguration(it->second);
                delete it->second;

                it = activeConfigurations.erase(it);
            }
            else
            {
                ++it;
            }
        }
        pthread_mutex_unlock(&activeConfigurationsMutex);

        free(target_config);
    }
}

//--------------------------------------------------------------------
//
// MonitorDotNet - Returns true if we are monitoring a dotnet process
//
//--------------------------------------------------------------------
bool MonitorDotNet(struct ProcDumpConfiguration *self)
{
    if(self->bDumpOnException || self->bMonitoringGCMemory || self->DumpGCGeneration != -1)
    {
        return true;
    }

    return false;
}

//--------------------------------------------------------------------
//
// CreateMonitorThread - Create a specific monitor thread
//
//--------------------------------------------------------------------
int CreateMonitorThread(struct ProcDumpConfiguration *self, enum TriggerType triggerType, void *(*monitorThread) (void *), void *arg)
{
    int rc = -1;

    if (self->nThreads < MAX_TRIGGERS)
    {
        if ((rc = pthread_create(&self->Threads[self->nThreads].thread, NULL, monitorThread, arg)) != 0)
        {
            return rc;
        }

        self->Threads[self->nThreads].trigger = triggerType;
        self->nThreads++;

    }
    else
    {
        Trace("CreateMonitorThread: max number of triggers reached.");
    }

    return rc;
}

//--------------------------------------------------------------------
//
// CreateMonitorThreads - Create each of the threads that will be running as a trigger
//
//--------------------------------------------------------------------
int CreateMonitorThreads(struct ProcDumpConfiguration *self)
{
    int rc = 0;
    self->nThreads = 0;
    bool tooManyTriggers = false;

    // create threads
    if (MonitorDotNet(self) == true)
    {
        if ((rc = CreateMonitorThread(self, Exception, DotNetMonitoringThread, (void *)self)) != 0 )
        {
            Trace("CreateMonitorThreads: failed to create DotNetMonitoringThread.");
            return rc;
        }
    }

    if (self->CpuThreshold != -1)
    {
        if ((rc = CreateMonitorThread(self, Processor, CpuMonitoringThread, (void *)self)) != 0 )
        {
            Trace("CreateMonitorThreads: failed to create CpuThread.");
            return rc;
        }
    }

    if (self->MemoryThreshold != NULL && !tooManyTriggers && self->bMonitoringGCMemory == false)
    {
        if ((rc = CreateMonitorThread(self, Commit, CommitMonitoringThread, (void *)self)) != 0 )
        {
            Trace("CreateMonitorThreads: failed to create CommitThread.");
            return rc;
        }
    }

    if (self->ThreadThreshold != -1 && !tooManyTriggers)
    {
        if ((rc = CreateMonitorThread(self, ThreadCount, ThreadCountMonitoringThread, (void *)self)) != 0 )
        {
            Trace("CreateMonitorThreads: failed to create ThreadThread.");
            return rc;
        }
    }

    if (self->FileDescriptorThreshold != -1 && !tooManyTriggers)
    {
        if ((rc = CreateMonitorThread(self, FileDescriptorCount, FileDescriptorCountMonitoringThread, (void *)self)) != 0 )
        {
            Trace("CreateMonitorThreads: failed to create FileDescriptorThread.");
            return rc;
        }
    }

    if (self->SignalCount > 0 && !tooManyTriggers)
    {
        if ((rc = CreateMonitorThread(self, Signal, SignalMonitoringThread, (void *)self)) != 0 )
        {
            Trace("CreateMonitorThreads: failed to create SignalMonitoringThread.");
            return rc;
        }
    }

    if (self->bTimerThreshold)
    {
        if ((rc = CreateMonitorThread(self, Timer, TimerThread, (void *)self)) != 0 )
        {
            Trace("CreateMonitorThreads: failed to create TimerThread.");
            return rc;
        }
    }

    if (self->bRestrackEnabled)
    {
        if ((rc = CreateMonitorThread(self, Restrack, RestrackThread, (void *)self)) != 0 )
        {
            Trace("CreateMonitorThreads: failed to create RestrackThread.");
            return rc;
        }
    }

    return 0;
}

//--------------------------------------------------------------------
//
// StartMonitor
// Creates the monitoring threads and begins the monitor based on
// the configuration passed in. In the case of exception monitoring
// we inject the monitor into the target process.
//
//--------------------------------------------------------------------
int StartMonitor(struct ProcDumpConfiguration* monitorConfig)
{
    int ret = 0;

    if(CheckAccess(monitorConfig) == false)
    {
        Log(error, "Procdump is not running with elevated credentials or the effective uid does not match the effective uid of the target process (pid %d).", monitorConfig->ProcessId);
        return -1;
    }

    if(CreateMonitorThreads(monitorConfig) != 0)
    {
        Log(error, INTERNAL_ERROR);
        Trace("StartMonitor: failed to create trigger threads.");
        return -1;
    }

    if(BeginMonitoring(monitorConfig) == false)
    {
        Log(error, INTERNAL_ERROR);
        Trace("StartMonitor: failed to start monitoring.");
        return -1;
    }

    Log(info, "Starting monitor for process %s (%d)", monitorConfig->ProcessName, monitorConfig->ProcessId);

    return ret;
}

//--------------------------------------------------------------------
//
// WaitForQuit - Wait for Quit Event or just timeout
//
//      Timed wait with awareness of quit event
//
// Returns: WAIT_OBJECT_0   - Quit triggered
//          WAIT_TIMEOUT    - Timeout
//          WAIT_ABANDONED  - At dump limit or terminated
//
//--------------------------------------------------------------------
int WaitForQuit(struct ProcDumpConfiguration *self, int milliseconds)
{
    if (!ContinueMonitoring(self)) {
        return WAIT_ABANDONED;
    }

    int wait = WaitForSingleObject(&self->evtQuit, milliseconds);

    if ((wait == WAIT_TIMEOUT) && !ContinueMonitoring(self)) {
        return WAIT_ABANDONED;
    }

    return wait;
}

//--------------------------------------------------------------------
//
// WaitForQuitOrEvent - Wait for Quit Event, an Event, or just timeout
//
//      Use to wait for dumps to complete, yet be aware of quit or finished events
//
// Returns: WAIT_OBJECT_0   - Quit triggered
//          WAIT_OBJECT_0+1 - Event triggered
//          WAIT_TIMEOUT    - Timeout
//          WAIT_ABANDONED  - (Abandonded) At dump limit or terminated
//
//--------------------------------------------------------------------
int WaitForQuitOrEvent(struct ProcDumpConfiguration *self, struct Handle *handle, int milliseconds)
{
    struct Handle *waits[2];
    waits[0] = &self->evtQuit;
    waits[1] = handle;

    if (!ContinueMonitoring(self)) {
        return WAIT_ABANDONED;
    }

    int wait = WaitForMultipleObjects(2, waits, false, milliseconds);
    if ((wait == WAIT_TIMEOUT) && !ContinueMonitoring(self)) {
        return WAIT_ABANDONED;
    }

    if ((wait == WAIT_OBJECT_0) && !ContinueMonitoring(self)) {
        return WAIT_ABANDONED;
    }

    return wait;
}


pthread_t GetRestrackThread(struct ProcDumpConfiguration *self)
{
    pthread_t restrackThread = 0;

    for(int i=0; i<self->nThreads; i++)
    {
        if(self->Threads[i].trigger == Restrack)
        {
            restrackThread = self->Threads[i].thread;
            break;
        }
    }

    return restrackThread;
}

//--------------------------------------------------------------------
//
// CancelRestrackThread - Cancel the restrack thread
//
//--------------------------------------------------------------------
int CancelRestrackThread(struct ProcDumpConfiguration *self)
{
    Trace("CancelRestrackThread: Enter [id=%d]", gettid());

    int rc = 0;
    pthread_t restrackThread = 0;

    restrackThread = GetRestrackThread(self);

    if(restrackThread != 0)
    {
        Trace("CancelRestrackThread: cancel restrack thread");
        SetQuit(self, 1);
    }

    Trace("CancelRestrackThread: Exit [id=%d]", gettid());
    return rc;
}

//--------------------------------------------------------------------
//
// WaitForAllMonitorsToTerminate - Wait for all monitors to terminate
//
//--------------------------------------------------------------------
int WaitForAllMonitorsToTerminate(struct ProcDumpConfiguration *self)
{
    int rc = 0;
    pthread_t restrackThread = 0;

    // Wait for the other monitoring threads. We exclude restrack
    // since we want that thread to exit last
    for (int i = 0; i < self->nThreads; i++)
    {
        if(self->Threads[i].trigger != Restrack)
        {
            if ((rc = pthread_join(self->Threads[i].thread, NULL)) != 0)
            {
                Log(error, "An error occurred while joining threads\n");
                exit(-1);
            }
        }
        else
        {
            restrackThread = self->Threads[i].thread;
        }
    }

    //
    // If we have a restrack thread, cancel it and wait for it to exit
    //
#ifdef __linux__    
    if(CancelRestrackThread(self) != 0)
    {
        if ((rc = pthread_join(restrackThread, NULL)) != 0)
        {
            Log(error, "An error occurred while joining restrack thread\n");
            exit(-1);
        }
    }
#endif

    return rc;
}

//--------------------------------------------------------------------
//
// WaitForSignalThreadToTerminate - Wait for signal handler thread to terminate
//
//--------------------------------------------------------------------
int WaitForSignalThreadToTerminate(struct ProcDumpConfiguration *self)
{
    int rc = 0;

    // Cancel the signal handling thread.
    // We dont care about the return since the signal thread might already be gone.
    pthread_cancel(sig_thread_id);

    // Wait for signal handling thread to complete
    if ((rc = pthread_join(sig_thread_id, NULL)) != 0) {
        Log(error, "An error occurred while joining SignalThread.\n");
        exit(-1);
    }

    return rc;
}

//--------------------------------------------------------------------
//
// IsQuit - A check on the underlying value of whether we should quit
//
//--------------------------------------------------------------------
bool IsQuit(struct ProcDumpConfiguration *self)
{
    return (self->nQuit != 0);
}


//--------------------------------------------------------------------
//
// SetQuit - Sets the quit value and signals the event
//
//--------------------------------------------------------------------
int SetQuit(struct ProcDumpConfiguration *self, int quit)
{
    self->nQuit = quit;
    SetEvent(&self->evtQuit.event);

    return self->nQuit;
}

//--------------------------------------------------------------------
//
// ContinueMonitoring - Should we keep monitoring or should we clean up our thread
//
//--------------------------------------------------------------------
bool ContinueMonitoring(struct ProcDumpConfiguration *self)
{
    // Procdump exiting
    if (self->nQuit == 1)
    {
        return false;
    }

    // Are we generating leak reports?
    if (self->bLeakReportInProgress == true)
    {
        return true;
    }

    // Have we reached the dump limit?
    if (self->NumberOfDumpsCollected >= self->NumberOfDumpsToCollect || self->NumberOfLeakReportsCollected >= self->NumberOfDumpsToCollect)
    {
        return false;
    }

    // Do we already know the process is terminated?
    if (self->bTerminated)
    {
        return false;
    }

    // check if any process are running with PGID
    if(self->bProcessGroup && kill(-1 * self->ProcessGroup, 0))
    {
        self->bTerminated = true;
        return false;
    }

    // Let's check to make sure the process is still alive then
    // note: kill([pid], 0) doesn't send a signal but does perform error checking
    //       therefore, if it returns 0, the process is still alive, -1 means it errored out
    if (self->ProcessId != NO_PID && kill(self->ProcessId, 0))
    {
        self->bTerminated = true;
        Log(warn, "Target process %d is no longer alive", self->ProcessId);
        return false;
    }

    // Otherwise, keep going!
    return true;
}

//--------------------------------------------------------------------
//
// BeginMonitoring - Sync up monitoring threads
//
//--------------------------------------------------------------------
bool BeginMonitoring(struct ProcDumpConfiguration *self)
{
    return SetEvent(&(self->evtStartMonitoring.event));
}

extern long HZ;                                // clock ticks per second

//--------------------------------------------------------------------
//
// WaitThreads - Cancels the threads and waits for the specified threads
// using join.
//
//--------------------------------------------------------------------
void WaitThreads(std::vector<pthread_t>& threads)
{
    for (auto& thread : threads)
    {
        //
        // If user hit CTRL+C, cancel the thread.
        //
        if(g_sigint == true)
        {
            pthread_cancel(thread);
        }

        pthread_join(thread, NULL);
    }
}

//--------------------------------------------------------------------
//
// CommitMonitoringThread - Thread monitoring for memory consumption
//
//--------------------------------------------------------------------
void *CommitMonitoringThread(void *thread_args /* struct ProcDumpConfiguration* */)
{
    Trace("CommitMonitoringThread: Enter [id=%d]", gettid());
    struct ProcDumpConfiguration *config = (struct ProcDumpConfiguration *)thread_args;

    long pageSize_kb;
    unsigned long memUsage = 0;
    struct ProcessStat proc = {0};
    int rc = 0;
    auto_free struct CoreDumpWriter *writer = NULL;
    auto_free char* dumpFileName = NULL;
    std::vector<pthread_t> leakReportThreads;

    writer = NewCoreDumpWriter(COMMIT, config);

    pageSize_kb = sysconf(_SC_PAGESIZE) >> 10; // convert bytes to kilobytes (2^10)

    if ((rc = WaitForQuitOrEvent(config, &config->evtStartMonitoring, INFINITE_WAIT)) == WAIT_OBJECT_0 + 1)
    {
        while ((rc = WaitForQuit(config, config->PollingInterval)) == WAIT_TIMEOUT)
        {
            if (GetProcessStat(config->ProcessId, &proc))
            {
#ifdef __linux__                
                // Calc Commit
                memUsage = (proc.rss * pageSize_kb) >> 10;    // get Resident Set Size
                memUsage += (proc.nswap * pageSize_kb) >> 10; // get Swap size
#elif __APPLE__
                memUsage = proc.rss / (1024.0 * 1024.0);       // get Resident Set Size
#endif

                // Commit Trigger
                if ((config->bMemoryTriggerBelowValue && (memUsage < config->MemoryThreshold[config->MemoryCurrentThreshold])) ||
                    (!config->bMemoryTriggerBelowValue && (memUsage >= config->MemoryThreshold[config->MemoryCurrentThreshold])))
                {
                    Log(info, "Trigger: Commit usage:%ldMB on process ID: %d", memUsage, config->ProcessId);

                    if(config->bRestrackGenerateDump == true)
                    {
                        // Only generate core dump if user did not specify the "nodump" restrack option
                        dumpFileName = WriteCoreDump(writer);
                        if(dumpFileName == NULL)
                        {
                            SetQuit(config, 1);
                        }
                    }

                    //
                    // Check to see if restrack is specified, if so, save current resource usage to file.
                    //
#ifdef __linux__                    
                    if(config->bRestrackEnabled == true)
                    {
                        pthread_t id = WriteRestrackSnapshot(config, writer->Type);
                        if (id == 0)
                        {
                            SetQuit(config, 1);
                        }
                        else
                        {
                            leakReportThreads.push_back(id);
                        }
                    }
#endif                    


                    config->MemoryCurrentThreshold++;

                    if ((rc = WaitForQuit(config, config->ThresholdSeconds * 1000)) != WAIT_TIMEOUT)
                    {
                        break;
                    }
                }
            }
            else
            {
                Log(error, "An error occurred while fetching memory info\n");
                exit(-1);
            }
        }
    }

    //
    // Wait for the leak reporting threads to finish
    //
    WaitThreads(leakReportThreads);

    Trace("CommitMonitoringThread: Exit [id=%d]", gettid());
    return NULL;
}

//--------------------------------------------------------------------
//
// ThreadCountMonitoringThread - Thread monitoring for thread count
//
//--------------------------------------------------------------------
void* ThreadCountMonitoringThread(void *thread_args /* struct ProcDumpConfiguration* */)
{
    Trace("ThreadCountMonitoringThread: Enter [id=%d]", gettid());
    struct ProcDumpConfiguration *config = (struct ProcDumpConfiguration *)thread_args;

    struct ProcessStat proc = {0};
    int rc = 0;
    auto_free struct CoreDumpWriter *writer = NULL;
    auto_free char* dumpFileName = NULL;
    std::vector<pthread_t> leakReportThreads;

    writer = NewCoreDumpWriter(THREAD, config);

    if ((rc = WaitForQuitOrEvent(config, &config->evtStartMonitoring, INFINITE_WAIT)) == WAIT_OBJECT_0 + 1)
    {
        while ((rc = WaitForQuit(config, config->PollingInterval)) == WAIT_TIMEOUT)
        {
            if (GetProcessStat(config->ProcessId, &proc))
            {
                if (proc.num_threads >= config->ThreadThreshold)
                {
                    Log(info, "Trigger: Thread count:%ld on process ID: %d", proc.num_threads, config->ProcessId);

                    if(config->bRestrackGenerateDump == true)
                    {
                        // Only generate core dump if user did not specify the "nodump" restrack option
                        dumpFileName = WriteCoreDump(writer);
                        if(dumpFileName == NULL)
                        {
                            SetQuit(config, 1);
                        }
                    }

                    //
                    // Check to see if restrack is specified, if so, save current resource usage to file.
                    //
#ifdef __linux__                    
                    if(config->bRestrackEnabled == true)
                    {
                        pthread_t id = WriteRestrackSnapshot(config, writer->Type);
                        if (id == 0)
                        {
                            SetQuit(config, 1);
                        }
                        else
                        {
                            leakReportThreads.push_back(id);
                        }
                    }
#endif                    

                    if ((rc = WaitForQuit(config, config->ThresholdSeconds * 1000)) != WAIT_TIMEOUT)
                    {
                        break;
                    }
                }
            }
            else
            {
                Log(error, "An error occurred while parsing procfs\n");
                exit(-1);
            }
        }
    }

    //
    // Wait for the leak reporting threads to finish
    //
    WaitThreads(leakReportThreads);

    Trace("ThreadCountMonitoringThread: Exit [id=%d]", gettid());
    return NULL;
}


//--------------------------------------------------------------------
//
// FileDescriptorCountMonitoringThread - Thread monitoring for file
// descriptor count
//
//--------------------------------------------------------------------
void* FileDescriptorCountMonitoringThread(void *thread_args /* struct ProcDumpConfiguration* */)
{
    Trace("FileDescriptorCountMonitoringThread: Enter [id=%d]", gettid());
    struct ProcDumpConfiguration *config = (struct ProcDumpConfiguration *)thread_args;

    struct ProcessStat proc = {0};
    int rc = 0;
    auto_free struct CoreDumpWriter *writer = NULL;
    auto_free char* dumpFileName = NULL;
    std::vector<pthread_t> leakReportThreads;

    writer = NewCoreDumpWriter(FILEDESC, config);

    if ((rc = WaitForQuitOrEvent(config, &config->evtStartMonitoring, INFINITE_WAIT)) == WAIT_OBJECT_0 + 1)
    {
        while ((rc = WaitForQuit(config, config->PollingInterval)) == WAIT_TIMEOUT)
        {
            if (GetProcessStat(config->ProcessId, &proc))
            {
                if (proc.num_filedescriptors >= config->FileDescriptorThreshold)
                {
                    if(config->bRestrackGenerateDump == true)
                    {
                        // Only generate core dump if user did not specify the "nodump" restrack option
                        dumpFileName = WriteCoreDump(writer);
                        if(dumpFileName == NULL)
                        {
                            SetQuit(config, 1);
                        }
                    }

                    //
                    // Check to see if restrack is specified, if so, save current resource usage to file.
                    //
#ifdef __linux__                    
                    if(config->bRestrackEnabled == true)
                    {
                        pthread_t id = WriteRestrackSnapshot(config, writer->Type);
                        if (id == 0)
                        {
                            SetQuit(config, 1);
                        }
                        else
                        {
                            leakReportThreads.push_back(id);
                        }
                    }
#endif                    

                    if ((rc = WaitForQuit(config, config->ThresholdSeconds * 1000)) != WAIT_TIMEOUT)
                    {
                        break;
                    }
                }
            }
            else
            {
                Log(error, "An error occurred while parsing procfs\n");
                exit(-1);
            }
        }
    }

    //
    // Wait for the leak reporting threads to finish
    //
    WaitThreads(leakReportThreads);
    Trace("FileDescriptorCountMonitoringThread: Exit [id=%d]", gettid());
    return NULL;
}

//
// This thread monitors for a specific signal to be sent to target process.
// It uses ptrace (PTRACE_SEIZE) and once the signal with the corresponding
// signal number is intercepted, it detaches from the target process in a stopped state
// followed by invoking gcore to generate the dump. Once completed, a SIGCONT followed by the
// original signal is sent to the target process. Signals of non-interest are simply forwarded
// to the target process.
//
// Polling interval has no meaning during signal monitoring.
//
void* SignalMonitoringThread(void *thread_args /* struct ProcDumpConfiguration* */)
{
    Trace("SignalMonitoringThread: Enter [id=%d]", gettid());
#ifdef __linux__
    struct ProcDumpConfiguration *config = (struct ProcDumpConfiguration *)thread_args;
    int wstatus;
    int signum=-1;
    int rc = 0;
    auto_free struct CoreDumpWriter *writer = NULL;
    auto_free char* dumpFileName = NULL;
    std::vector<pthread_t> leakReportThreads;

    writer = NewCoreDumpWriter(SIGNAL, config);

    if ((rc = WaitForQuitOrEvent(config, &config->evtStartMonitoring, INFINITE_WAIT)) == WAIT_OBJECT_0 + 1)
    {
        // Attach to the target process. We use SEIZE here to avoid
        // the SIGSTOP issues of the ATTACH method.
        if (ptrace(PTRACE_SEIZE, config->ProcessId, NULL, NULL) == -1)
        {
            Log(error, "Unable to PTRACE the target process");
        }
        else
        {
            while(1)
            {
                // Wait for signal to be delivered
                waitpid(config->ProcessId, &wstatus, 0);
                if(WIFEXITED(wstatus) || WIFSIGNALED(wstatus))
                {
                    ptrace(PTRACE_DETACH, config->ProcessId, 0, 0);
                    break;
                }

                pthread_mutex_lock(&config->ptrace_mutex);

                // We are now in a signal-stop state

                signum = WSTOPSIG(wstatus);
                bool found = false;
                for(int i = 0; i < config->SignalCount; i++)
                {
                    if(signum == config->SignalNumber[i])
                    {
                        found = true;
                        break;
                    }
                }

                if(found == true)
                {
                    // We have to detach in a STOP state so we can invoke gcore
                    if(ptrace(PTRACE_DETACH, config->ProcessId, 0, SIGSTOP) == -1)
                    {
                        Log(error, "Unable to PTRACE (DETACH) the target process");
                        pthread_mutex_unlock(&config->ptrace_mutex);
                        break;
                    }

                    // Write core dump
                    Log(info, "Trigger: Signal:%d on process ID: %d", signum, config->ProcessId);

                    if(config->bRestrackGenerateDump == true)
                    {
                        // Only generate core dump if user did not specify the "nodump" restrack option
                        dumpFileName = WriteCoreDump(writer);
                        if(dumpFileName == NULL)
                        {
                            ptrace(PTRACE_CONT, config->ProcessId, NULL, signum);
                            ptrace(PTRACE_DETACH, config->ProcessId, 0, 0);
                            break;
                        }
                    }

                    //
                    // Check to see if restrack is specified, if so, save current resource usage to file.
                    //
                    if(config->bRestrackEnabled == true)
                    {
                        pthread_t id = WriteRestrackSnapshot(config, writer->Type);
                        if (id != 0)
                        {
                            leakReportThreads.push_back(id);
                        }
                    }

                    ptrace(PTRACE_CONT, config->ProcessId, NULL, signum);

                    if(config->NumberOfDumpsCollected >= config->NumberOfDumpsToCollect || config->NumberOfLeakReportsCollected >= config->NumberOfDumpsToCollect)
                    {
                        // If we are over the max number of dumps to collect, send the original signal we intercepted.
                        pthread_mutex_unlock(&config->ptrace_mutex);
                        ptrace(PTRACE_DETACH, config->ProcessId, 0, 0);
                        break;
                    }

                    // Re-attach to the target process
                    if (ptrace(PTRACE_SEIZE, config->ProcessId, NULL, NULL) == -1)
                    {
                        Log(error, "Unable to PTRACE the target process");
                        pthread_mutex_unlock(&config->ptrace_mutex);
                        break;
                    }

                    pthread_mutex_unlock(&config->ptrace_mutex);
                    continue;
                }

                // Resume execution of the target process
                ptrace(PTRACE_CONT, config->ProcessId, NULL, signum);
                pthread_mutex_unlock(&config->ptrace_mutex);
            }
        }
    }

    //
    // Wait for the leak reporting threads to finish
    //
    WaitThreads(leakReportThreads);
#endif
    Trace("SignalMonitoringThread: Exit [id=%d]", gettid());
    return NULL;
}

//--------------------------------------------------------------------
//
// CpuMonitoringThread - Thread monitoring for CPU usage.
//
//--------------------------------------------------------------------
void *CpuMonitoringThread(void *thread_args /* struct ProcDumpConfiguration* */)
{
    Trace("CpuMonitoringThread: Enter [id=%d]", gettid());
    struct ProcDumpConfiguration *config = (struct ProcDumpConfiguration *)thread_args;

    int cpuUsage;
    auto_free struct CoreDumpWriter *writer = NULL;
    auto_free char* dumpFileName = NULL;
#ifdef __linux__
    std::vector<pthread_t> leakReportThreads;
#endif

    writer = NewCoreDumpWriter(CPU, config);

    int rc = 0;
    struct ProcessStat proc = {0};

    if ((rc = WaitForQuitOrEvent(config, &config->evtStartMonitoring, INFINITE_WAIT)) == WAIT_OBJECT_0 + 1)
    {
        while ((rc = WaitForQuit(config, config->PollingInterval)) == WAIT_TIMEOUT)
        {
            if (GetProcessStat(config->ProcessId, &proc))
            {
                cpuUsage = GetCpuUsage(config->ProcessId);
                Trace("CpuMonitoringThread: CPU usage:%d%% on process ID: %d", cpuUsage, config->ProcessId);

                // CPU Trigger
                if ((config->bCpuTriggerBelowValue && (cpuUsage < config->CpuThreshold)) ||
                    (!config->bCpuTriggerBelowValue && (cpuUsage >= config->CpuThreshold)))
                {
                    Log(info, "Trigger: CPU usage:%d%% on process ID: %d", cpuUsage, config->ProcessId);
                    if(config->bRestrackGenerateDump == true)
                    {
                        // Only generate core dump if user did not specify the "nodump" restrack option
                        dumpFileName = WriteCoreDump(writer);
                        if(dumpFileName == NULL)
                        {
                            SetQuit(config, 1);
                        }
                    }

                    //
                    // Check to see if restrack is specified, if so, save current resource usage to file.
                    //
#ifdef __linux__                    
                    if(config->bRestrackEnabled == true)
                    {
                        pthread_t id = WriteRestrackSnapshot(config, writer->Type);
                        if (id == 0)
                        {
                            SetQuit(config, 1);
                        }
                        else
                        {
                            leakReportThreads.push_back(id);
                        }
                    }
#endif                    

                    if ((rc = WaitForQuit(config, config->ThresholdSeconds * 1000)) != WAIT_TIMEOUT)
                    {
                        break;
                    }
                }
            }
            else
            {
                Log(error, "An error occurred while parsing procfs\n");
                exit(-1);
            }
        }
    }

    //
    // Wait for the leak reporting threads to finish
    //
#ifdef __linux__    
    WaitThreads(leakReportThreads);
#endif

    Trace("CpuMonitoringThread: Exit [id=%d]", gettid());
    return NULL;
}

//--------------------------------------------------------------------
//
// TimerThread - Thread that creates dumps based on specified timer
// interval.
//
//--------------------------------------------------------------------
void *TimerThread(void *thread_args /* struct ProcDumpConfiguration* */)
{
    Trace("TimerThread: Enter [id=%d]", gettid());

    struct ProcDumpConfiguration *config = (struct ProcDumpConfiguration *)thread_args;
    auto_free struct CoreDumpWriter *writer = NULL;
    auto_free char* dumpFileName = NULL;
    std::vector<pthread_t> leakReportThreads;

    writer = NewCoreDumpWriter(TIME, config);

    int rc = 0;

    if ((rc = WaitForQuitOrEvent(config, &config->evtStartMonitoring, INFINITE_WAIT)) == WAIT_OBJECT_0 + 1)
    {
        while ((rc = WaitForQuit(config, 0)) == WAIT_TIMEOUT)
        {
            Log(info, "Trigger: Timer:%ld(s) on process ID: %d", config->PollingInterval/1000, config->ProcessId);
            if(config->bRestrackGenerateDump == true)
            {
                // Only generate core dump if user did not specify the "nodump" restrack option
                dumpFileName = WriteCoreDump(writer);
                if(dumpFileName == NULL)
                {
                    SetQuit(config, 1);
                }
            }

            //
            // Check to see if restrack is specified, if so, save current resource usage to file.
            //
#ifdef __linux__            
            if(config->bRestrackEnabled == true)
            {
                pthread_t id = WriteRestrackSnapshot(config, writer->Type);
                if (id == 0)
                {
                    SetQuit(config, 1);
                }
                else
                {
                    leakReportThreads.push_back(id);
                }
            }
#endif            

            if ((rc = WaitForQuit(config, config->ThresholdSeconds * 1000)) != WAIT_TIMEOUT) {
                break;
            }
        }
    }

    //
    // Wait for the leak reporting threads to finish
    //
    WaitThreads(leakReportThreads);

    Trace("TimerThread: Exit [id=%d]", gettid());
    return NULL;
}

//--------------------------------------------------------------------
//
// DotNetMonitoringThread - Thread that creates dumps based on
// dotnet triggers.
//
// NOTE: .NET only.
// NOTE: At the moment, .NET triggers are mutually exclusive meaning
// only one can be specified at a time. For example, you cannot specify
// both exception based monitoring and GC based monitoring.
//
//--------------------------------------------------------------------
void *DotNetMonitoringThread(void *thread_args /* struct ProcDumpConfiguration* */)
{
    Trace("DotNetMonitoringThread: Enter [id=%d]", gettid());
#ifdef __linux__    
    struct ProcDumpConfiguration *config = (struct ProcDumpConfiguration *)thread_args;
    auto_free char* fullDumpPath = NULL;
    auto_cancel_thread pthread_t waitForProfilerCompletion = -1;
    auto_free char* clientData = NULL;

    int rc = 0;

    if ((rc = WaitForQuitOrEvent(config, &config->evtStartMonitoring, INFINITE_WAIT)) == WAIT_OBJECT_0 + 1)
    {
        if(config->CoreDumpName == NULL)
        {
            // We don't have a dump name so we just use the path (append a '/' to indicate its a base path)
            if(config->CoreDumpPath[strlen(config->CoreDumpPath)-1] != '/')
            {
                fullDumpPath = (char*) malloc(strlen(config->CoreDumpPath) + 2);    // +1 = '\0', +1 = '/'
                if(fullDumpPath == NULL)
                {
                    Trace("DotNetMonitoringThread: Failed to allocate memory.");
                    return NULL;
                }

                snprintf(fullDumpPath, strlen(config->CoreDumpPath) + 2, "%s/", config->CoreDumpPath);
            }
            else
            {
                fullDumpPath = (char*) malloc(strlen(config->CoreDumpPath) + 1);
                if(fullDumpPath == NULL)
                {
                    Trace("DotNetMonitoringThread: Failed to allocate memory.");
                    return NULL;
                }

                snprintf(fullDumpPath, strlen(config->CoreDumpPath) + 1, "%s", config->CoreDumpPath);
            }
        }
        else
        {
            // We have a dump name, let's append to dump path
            if(config->CoreDumpPath[strlen(config->CoreDumpPath)] != '/')
            {
                fullDumpPath = (char*) malloc(strlen(config->CoreDumpPath) + strlen(config->CoreDumpName) + 2);    // +1 = '\0', +1 = '/'
                if(fullDumpPath == NULL)
                {
                    Trace("DotNetMonitoringThread: Failed to allocate memory.");
                    return NULL;
                }

                snprintf(fullDumpPath, strlen(config->CoreDumpPath) + strlen(config->CoreDumpName) + 2, "%s/%s", config->CoreDumpPath, config->CoreDumpName);
            }
            else
            {
                fullDumpPath = (char*) malloc(strlen(config->CoreDumpPath) + strlen(config->CoreDumpName) + 1);    // +1 = '\0'
                if(fullDumpPath == NULL)
                {
                    Trace("DotNetMonitoringThread: Failed to allocate memory.");
                    return NULL;
                }

                snprintf(fullDumpPath, strlen(config->CoreDumpPath) + strlen(config->CoreDumpName) + 1, "%s%s", config->CoreDumpPath, config->CoreDumpName);
            }
        }

        // Create thread to wait for profiler completion
        if ((pthread_create(&waitForProfilerCompletion, NULL, WaitForProfilerCompletion, (void *) config)) != 0)
        {
            Trace("DotNetMonitoringThread: failed to create WaitForProfilerCompletion thread.");
            return NULL;
        }

        // Wait for the socket to be available from WaitForProfilerCompletion thread
        pthread_mutex_lock(&config->dotnetMutex);
        while(!config->bSocketInitialized)
        {
            pthread_cond_wait(&config->dotnetCond, &config->dotnetMutex);
        }
        pthread_mutex_unlock(&config->dotnetMutex);

        // Get the corresponding client data to be sent to profiler
        clientData = GetClientData(config, fullDumpPath);
        if(clientData == NULL)
        {
            Trace("DotNetMonitoringThread: Failed to get client data.");
            return NULL;
        }

        // Inject the profiler into the target process
        if(InjectProfiler(config->ProcessId, clientData)!=0)
        {
            Trace("DotNetMonitoringThread: Failed to inject the profiler.");
            pthread_cancel(waitForProfilerCompletion);
        }

        pthread_join(waitForProfilerCompletion, NULL);
    }
#endif
    Trace("DotNetMonitoringThread: Exit [id=%d]", gettid());
    return NULL;
}

//--------------------------------------------------------------------
//
// RestrackThread - Thread that handles resource tracking
//
//--------------------------------------------------------------------
void *RestrackThread(void *thread_args /* struct ProcDumpConfiguration* */)
{
    Trace("RestrackThread: Enter [id=%d]", gettid());
#ifdef __linux__    
    struct ProcDumpConfiguration *config = (struct ProcDumpConfiguration *)thread_args;
    auto_free char* fullDumpPath = NULL;
    struct procdump_ebpf* skel = NULL;
    int rc = 0;

    if ((skel = RunRestrack(config)) == NULL)
    {
        Trace("RestrackThread: Failed to run restrack eBPF program.");
        return NULL;
    }

	//
    // Set up ring buffer polling
    //
	struct ring_buffer *ringBuffer = ring_buffer__new(bpf_map__fd(skel->maps.ringBuffer), RestrackHandleEvent, NULL, NULL);
	if (!ringBuffer)
    {
        Trace("RestrackThread: Failed to create ring buffer.");
		return NULL;
    }

    if ((rc = WaitForQuitOrEvent(config, &config->evtStartMonitoring, INFINITE_WAIT)) == WAIT_OBJECT_0 + 1)
    {
        while ((rc = WaitForQuit(config, 0)) == WAIT_TIMEOUT)
        {
            //
            // Loop and poll eBPF buffer for memory allocation events
            //
            int err = ring_buffer__poll(ringBuffer, 100);
            if (err == -EINTR)
            {
                err = 0;
                break;
            }
            if (err < 0)
            {
                printf("RestrackThread: Error polling ring buffer: %d\n", err);
                break;
            }

            if ((rc = WaitForQuit(config, 1000)) != WAIT_TIMEOUT)
            {
                break;
            }
        }
    }

#endif
    Trace("RestrackThread: Exit [id=%d]", gettid());
    return NULL;
}

//-------------------------------------------------------------------------------------
//
// GetClientData
//
// Gets the client data string depending on which triggers were requested in the
// specified config.
//
//-------------------------------------------------------------------------------------
char* GetClientData(struct ProcDumpConfiguration *self, char* fullDumpPath)
{
    Trace("GetClientData: Entering GetClientData");
    char* clientData = NULL;
#ifdef __linux__    
    auto_free char* exceptionFilter = NULL;
    auto_free char* thresholds = NULL;

    if(self->bDumpOnException)
    {
        // exception_trigger;<fullpathtodumplocation>;<pidofprocdump>;<exception>:<numdumps>;<exception>:<numdumps>,...
        exceptionFilter = GetEncodedExceptionFilter(self->ExceptionFilter, self->NumberOfDumpsToCollect);
        if(exceptionFilter == NULL)
        {
            Trace("GetClientData: Failed to get exception filter.");
            return NULL;
        }

        clientData = GetClientDataHelper(Exception, fullDumpPath, "%s", exceptionFilter);
        if(clientData == NULL)
        {
            Trace("GetClientData: Failed to get client data (-e).");
            return NULL;
        }
    }
    else if (self->bMonitoringGCMemory)
    {
        // GC Memory trigger (-gcm);<fullpathtodumplocation>;<pidofprocdump>;Generation:Threshold1;Threshold2,...
        thresholds = GetThresholds(self);
        if(thresholds == NULL)
        {
            Trace("GetClientData: Failed to get thresholds.");
            return NULL;
        }

        clientData = GetClientDataHelper(GCThreshold, fullDumpPath, "%d;%s", self->DumpGCGeneration == -1 ? CUMULATIVE_GC_SIZE : self->DumpGCGeneration, thresholds);
        if(clientData == NULL)
        {
            Trace("GetClientData: Failed to get client data (-gcm).");
            return NULL;
        }
    }
    else if(self->DumpGCGeneration != -1 && self->MemoryThreshold == NULL)
    {
        // GC Generation (-gcgen);<fullpathtodumplocation>;<pidofprocdump>;GCGeneration
        clientData = GetClientDataHelper(GCGeneration, fullDumpPath, "%d", self->DumpGCGeneration);
        if(clientData == NULL)
        {
            Trace("GetClientData: Failed to get client data (-gcgen).");
            return NULL;
        }
    }
    else
    {
        Trace("GetClientData: Invalid trigger specified");
        return NULL;
    }

#endif
    Trace("GetClientData: Exiting GetClientData");
    return clientData;
}

//-------------------------------------------------------------------------------------
//
// GetClientDataHelper
//
// Helper that fetches client data based on format specified.
//
//-------------------------------------------------------------------------------------
char* GetClientDataHelper(enum TriggerType triggerType, char* path, const char* format, ...)
{
    unsigned int clientDataSize = 0;
    unsigned int clientDataPrefixSize = 0;
    char* clientData = NULL;

    va_list args;
    va_start(args, format);

    clientDataPrefixSize = snprintf(NULL, 0, "%d;%s;%d;", triggerType, path, getpid());
    va_list args_copy;
    va_copy(args_copy, args);
    clientDataSize = clientDataPrefixSize + vsnprintf(NULL, 0, format, args_copy) + 1;
    va_end(args_copy);
    clientData = (char*) malloc(clientDataSize);
    if(clientData == NULL)
    {
        Trace("GetClientDataHelper: Failed to allocate memory for client data.");
        va_end(args);
        return NULL;
    }

    snprintf(clientData, clientDataSize, "%d;%s;%d;", triggerType, path, getpid());
    vsnprintf(clientData+clientDataPrefixSize, clientDataSize, format, args);

    va_end(args);
    return clientData;
}

//-------------------------------------------------------------------------------------
//
// GetThresholds
//
// Returns a ; separated string of GC mem thresholds specified in
// self->MemoryThreshold
//
//-------------------------------------------------------------------------------------
// On GCC 11 it generates a false positive leak for thresholds.
#if (__GNUC__ >= 11 && __GNUC__ < 12)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
#endif
char* GetThresholds(struct ProcDumpConfiguration *self)
{
    Trace("GetThresholds: Entering GetThresholds");
    int thresholdLen = 0;
    char* thresholds = NULL;

    for(int i = 0; i < self->MemoryThresholdCount; i++)
    {
        thresholdLen += snprintf(NULL, 0, "%d", self->MemoryThreshold[i]);
        if(i != self->MemoryThresholdCount - 1)
        {
            thresholdLen++;     // Comma
        }
    }

    thresholdLen++;     // NULL terminator

    thresholds = (char*) malloc(thresholdLen);
    if(thresholds != NULL)
    {
        char* writePos = thresholds;
        for(int i = 0; i < self->MemoryThresholdCount; i++)
        {
	    int len = snprintf(writePos, thresholdLen, "%d", self->MemoryThreshold[i]);
	    writePos += len;
	    thresholdLen -= len;
	    if(i != self->MemoryThresholdCount - 1)
            {
		*writePos = ';';
		writePos++;
		thresholdLen--;
            }
        }

        *(writePos) = '\0';
    }

    Trace("GetThresholds: Exiting GetThresholds");
    return thresholds;
}
#if (__GNUC__ >= 11 && __GNUC__ < 12)
#pragma GCC diagnostic pop
#endif

//-------------------------------------------------------------------------------------
//
// WaitForProfilerCompletion
//
// Waits for profiler to send a status on (<prefix>/procdump-status-<pid>).
// Status is in the form of:
// <[uint]payload_len><[byte] 0=failure, 1=success><[uint] dumpfile_path_len><[char*]Dumpfile path>
//
// Where the failure byte is interpreted as following:
//  '0' - Dump with the given path failed to generate
//  '1' - Dump with the given path was generated
//  'F' - Profiler encountered an error and unloaded itself (path not applicable)
//  'H' - Profiler pinging to see if procdump is still running (path not applicable)
//
// The socket created is in the form: <socket_path>/procdump-status-<procdumpPid>-<targetPid>
//
//-------------------------------------------------------------------------------------
void *WaitForProfilerCompletion(void *thread_args /* struct ProcDumpConfiguration* */)
{
    Trace("WaitForProfilerCompletion: Enter [id=%d]", gettid());
#ifdef __linux__    
    struct ProcDumpConfiguration *config = (struct ProcDumpConfiguration *)thread_args;
    unsigned int t, s2;
    struct sockaddr_un local, remote;
    int len;
    pthread_t processMonitor = -1;
    auto_free char* tmpFolder = NULL;
    auto_free_fd int s=-1;

    tmpFolder = GetSocketPath(const_cast<char*>("procdump/procdump-status-"), getpid(), config->ProcessId);
    config->socketPath = tmpFolder;
    Trace("WaitForProfilerCompletion: Status socket path: %s", tmpFolder);

    if((s = socket(AF_UNIX, SOCK_STREAM, 0))==-1)
    {
        Trace("WaitForProfilerCompletion: Failed to create socket\n");
        unlink(tmpFolder);
        config->socketPath = NULL;
        return NULL;
    }

    local.sun_family = AF_UNIX;
    strcpy(local.sun_path, tmpFolder);
    unlink(local.sun_path);
    len = strlen(local.sun_path) + sizeof(local.sun_family);
    if(bind(s, (struct sockaddr *)&local, len)==-1)
    {
        Trace("WaitForProfilerCompletion: Failed to bind to socket\n");
        unlink(tmpFolder);
        config->socketPath = NULL;
        return NULL;
    }

    //
    // Change perms on the socket to be read/write for 'others' since the profiler is loaded into
    // an unknown user process
    //
    chmod(tmpFolder, 0777);

    //
    // Create a thread that will monitor for abnormal process terminations of the target process.
    // In case of an abnormal process termination, it cancels the socket that procdump is waiting on
    // for status from target process and we can exit promptly.
    //
    config->statusSocket = s;
    if ((pthread_create(&processMonitor, NULL, ProcessMonitor, (void *) config)) != 0)
    {
        Trace("WaitForProfilerCompletion: failed to create ProcessMonitor thread.");
        unlink(tmpFolder);
        config->socketPath = NULL;
        return NULL;
    }

    //
    // Since the profiler callbacks can be invoked concurrently, we can also have X number of
    // pending status calls to ProcDump. We cap this to 50 by default which should be plenty for most
    // scenarios
    //
    if(listen(s, MAX_PROFILER_CONNECTIONS)==-1)
    {
        Trace("WaitForProfilerCompletion: Failed to listen on socket\n");
        unlink(tmpFolder);
        config->socketPath = NULL;
        ExitProcessMonitor(config, processMonitor);
        return NULL;
    }

    //
    // Notify that the socket is now available for the target process to communicate with
    //
    pthread_mutex_lock(&config->dotnetMutex);
    config->bSocketInitialized = true;
    pthread_cond_signal(&config->dotnetCond);
    pthread_mutex_unlock(&config->dotnetMutex);

    while(true)
    {
        Trace("WaitForProfilerCompletion:Waiting for status");

        t = sizeof(remote);
        if((s2 = accept(s, (struct sockaddr *)&remote, &t))==-1)
        {
            // This means the target process died and we need to return
            Trace("WaitForProfilerCompletion: Failed in accept call on socket\n");
            unlink(tmpFolder);
            config->socketPath = NULL;
            ExitProcessMonitor(config, processMonitor);
            return NULL;
        }

        // packet looks like this: <payload_len><[byte] 0=failure, 1=success><[uint_32] dumpfile_path_len><[char*]Dumpfile path>
        int payloadLen = 0;
        if(recv_all(s2, &payloadLen, sizeof(int))==-1)
        {
            // This means the target process died and we need to return
            Trace("WaitForProfilerCompletion: Failed in recv on accept socket\n");
            unlink(tmpFolder);
            close(s2);
            config->socketPath = NULL;
            ExitProcessMonitor(config, processMonitor);
            return NULL;
        }

        if(payloadLen>0)
        {
            Trace("Received payload len %d", payloadLen);
            char* payload = (char*) malloc(payloadLen);
            if(payload==NULL)
            {
                Trace("WaitForProfilerCompletion: Failed to allocate memory for payload\n");
                unlink(tmpFolder);
                close(s2);
                config->socketPath = NULL;
                ExitProcessMonitor(config, processMonitor);
                return NULL;
            }

            if(recv_all(s2, payload, payloadLen)==-1)
            {
                Trace("WaitForProfilerCompletion: Failed to allocate memory for payload\n");
                unlink(tmpFolder);
                close(s2);
                free(payload);
                config->socketPath = NULL;
                ExitProcessMonitor(config, processMonitor);
                return NULL;
            }

            char status = payload[0];
            Trace("WaitForProfilerCompletion: Received status %c", status);

            int dumpLen = 0;
            memcpy(&dumpLen, payload+1, sizeof(int));
            Trace("WaitForProfilerCompletion: Received dump length %d", dumpLen);

            if(dumpLen > PATH_MAX+1)
            {
                Trace("WaitForProfilerCompletion: Payload contained invalid dumplen %s\n", dumpLen);
                unlink(tmpFolder);
                close(s2);
                free(payload);
                config->socketPath = NULL;
                ExitProcessMonitor(config, processMonitor);
                return NULL;
            }

            char* dump =(char*) malloc(dumpLen+1);
            if(dump==NULL)
            {
                Trace("WaitForProfilerCompletion: Failed to allocate memory for dump\n");
                unlink(tmpFolder);
                close(s2);
                free(payload);
                config->socketPath = NULL;
                ExitProcessMonitor(config, processMonitor);
                return NULL;
            }

            memcpy(dump, payload+1+sizeof(int), dumpLen);
            dump[dumpLen] = '\0';
            Trace("WaitForProfilerCompletion: Received dump path %s", dump);

            free(payload);

            if(status=='1')
            {
                Log(info, "Core dump generated: %s", dump);
                config->NumberOfDumpsCollected++;
                if(config->NumberOfDumpsCollected == config->NumberOfDumpsToCollect)
                {
                    Trace("WaitForProfilerCompletion: Total dump count has been reached: %d", config->NumberOfDumpsCollected);
                    unlink(tmpFolder);
                    close(s2);
                    free(dump);
                    config->socketPath = NULL;
                    break;
                }
            }
            else if(status=='2')
            {
                Log(error, "Failed to generate core dump: %s", dump);
            }
            else if(status=='F')
            {
                Log(error, "Exception monitoring failed.");
                Trace("WaitForProfilerCompletion: Total dump count has been reached: %d", config->NumberOfDumpsCollected);
                unlink(tmpFolder);
                free(dump);
                close(s2);
                config->socketPath = NULL;
                break;
            }
            else if(status=='H')
            {
                Trace("WaitForProfilerCompletion: Received health check ping from profiler");

                if(config->NumberOfLeakReportsCollected == config->NumberOfDumpsToCollect)
                {
                    //
                    // Since the protocol between profiler and procdump dictates when and how dumps are generated,
                    // we may still be in restrack only mode. In that case, we use the Health check call from the
                    // profiler to check if the number of leak reports that have been generated are at max.
                    //
                    Trace("WaitForProfilerCompletion: Total leak report count has been reached: %d", config->NumberOfLeakReportsCollected);
                    unlink(tmpFolder);
                    free(dump);
                    close(s2);
                    config->socketPath = NULL;
                    break;
                }
            }

            free(dump);
        }

        close(s2);
    }

    unlink(tmpFolder);
    config->socketPath = NULL;

    ExitProcessMonitor(config, processMonitor);
#endif
    Trace("WaitForProfilerCompletion: Exiting WaitForProfilerCompletion Thread [id=%d]", gettid());
    return NULL;
}

//--------------------------------------------------------------------
//
// ProcessMonitor - Thread that monitors for the existence of the
// target process.
//
//--------------------------------------------------------------------
void *ProcessMonitor(void *thread_args /* struct ProcDumpConfiguration* */)
{
    Trace("ProcessMonitor: Enter [id=%d]", gettid());
    struct ProcDumpConfiguration *config = (struct ProcDumpConfiguration *)thread_args;
    int rc = 0;

    while ((rc = WaitForQuit(config, 0)) == WAIT_TIMEOUT && config->bExitProcessMonitor == false)
    {
        if(!LookupProcessByPid(config->ProcessId))
        {
            break;
        }
    }

    //
    // Target process terminated, cancel the status socket to unblock WaitForProfiler...
    //
    shutdown(config->statusSocket, SHUT_RD);

    Trace("ProcessMonitor: Exit [id=%d]", gettid());
    return NULL;
}

//--------------------------------------------------------------------
//
// ExitProcessMonitor - Sets ProcessMonitor thread to exit state and
// waits for the thread to exit.
//
//--------------------------------------------------------------------
bool ExitProcessMonitor(struct ProcDumpConfiguration* config, pthread_t processMonitor)
{
    config->bExitProcessMonitor = true;
    pthread_join(processMonitor, NULL);

    return true;
}
