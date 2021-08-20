// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License

//--------------------------------------------------------------------
//
// thread processes
//
//--------------------------------------------------------------------

#include "TriggerThreadProcs.h"
extern long HZ;                                // clock ticks per second

void *CommitMonitoringThread(void *thread_args /* struct ProcDumpConfiguration* */)
{
    Trace("CommitMonitoringThread: Starting Trigger Thread");
    struct ProcDumpConfiguration *config = (struct ProcDumpConfiguration *)thread_args;

    long pageSize_kb;
    unsigned long memUsage = 0;
    struct ProcessStat proc = {0};
    int rc = 0;
    struct CoreDumpWriter *writer = NewCoreDumpWriter(COMMIT, config); 

    pageSize_kb = sysconf(_SC_PAGESIZE) >> 10; // convert bytes to kilobytes (2^10)

    if ((rc = WaitForQuitOrEvent(config, &config->evtStartMonitoring, INFINITE_WAIT)) == WAIT_OBJECT_0 + 1)
    {
        while ((rc = WaitForQuit(config, config->PollingInterval)) == WAIT_TIMEOUT)
        {
            if (GetProcessStat(config->ProcessId, &proc))
            {
                // Calc Commit
                memUsage = (proc.rss * pageSize_kb) >> 10;    // get Resident Set Size
                memUsage += (proc.nswap * pageSize_kb) >> 10; // get Swap size

                // Commit Trigger
                if ((config->bMemoryTriggerBelowValue && (memUsage < config->MemoryThreshold)) ||
                    (!config->bMemoryTriggerBelowValue && (memUsage >= config->MemoryThreshold)))
                {
                    Log(info, "Commit: %ld MB", memUsage);
                    rc = WriteCoreDump(writer);

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

    free(writer);
    Trace("CommitMonitoringThread: Exiting Trigger Thread");
    pthread_exit(NULL);
}

void* ThreadCountMonitoringThread(void *thread_args /* struct ProcDumpConfiguration* */)
{
    Trace("ThreadCountMonitoringThread: Starting Thread Thread");
    struct ProcDumpConfiguration *config = (struct ProcDumpConfiguration *)thread_args;

    struct ProcessStat proc = {0};
    int rc = 0;
    struct CoreDumpWriter *writer = NewCoreDumpWriter(THREAD, config); 

    if ((rc = WaitForQuitOrEvent(config, &config->evtStartMonitoring, INFINITE_WAIT)) == WAIT_OBJECT_0 + 1)
    {
        while ((rc = WaitForQuit(config, config->PollingInterval)) == WAIT_TIMEOUT)
        {
            if (GetProcessStat(config->ProcessId, &proc))
            {
                if (proc.num_threads >= config->ThreadThreshold)
                {
                    Log(info, "Threads: %ld", proc.num_threads);
                    rc = WriteCoreDump(writer);

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

    free(writer);
    Trace("ThreadCountMonitoringThread: Exiting Thread trigger Thread");
    pthread_exit(NULL);
}


void* FileDescriptorCountMonitoringThread(void *thread_args /* struct ProcDumpConfiguration* */)
{
    Trace("FileDescriptorCountMonitoringThread: Starting Filedescriptor Thread");
    struct ProcDumpConfiguration *config = (struct ProcDumpConfiguration *)thread_args;

    struct ProcessStat proc = {0};
    int rc = 0;
    struct CoreDumpWriter *writer = NewCoreDumpWriter(FILEDESC, config); 

    if ((rc = WaitForQuitOrEvent(config, &config->evtStartMonitoring, INFINITE_WAIT)) == WAIT_OBJECT_0 + 1)
    {
        while ((rc = WaitForQuit(config, config->PollingInterval)) == WAIT_TIMEOUT)
        {
            if (GetProcessStat(config->ProcessId, &proc))
            {
                if (proc.num_filedescriptors >= config->FileDescriptorThreshold)
                {
                    Log(info, "File descriptors: %ld", proc.num_filedescriptors);
                    rc = WriteCoreDump(writer);

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

    free(writer);
    Trace("FileDescriptorCountMonitoringThread: Exiting Filedescriptor trigger Thread");
    pthread_exit(NULL);
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
    Trace("SignalMonitoringThread: Starting SignalMonitoring Thread");
    struct ProcDumpConfiguration *config = (struct ProcDumpConfiguration *)thread_args;
    int wstatus;
    int signum=-1;
    int rc = 0;
    struct CoreDumpWriter *writer = NewCoreDumpWriter(SIGNAL, config); 

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

                pthread_mutex_lock(&ptrace_mutex);

                // We are now in a signal-stop state

                signum = WSTOPSIG(wstatus);
                if(signum == config->SignalNumber)
                {
                    // We have to detach in a STOP state so we can invoke gcore
                    if(ptrace(PTRACE_DETACH, config->ProcessId, 0, SIGSTOP) == -1)
                    {
                        Log(error, "Unable to PTRACE (DETACH) the target process");                        
                        pthread_mutex_unlock(&ptrace_mutex);
                        break; 
                    }

                    // Write core dump
                    Log(info, "Signal intercepted: %d", signum);
                    rc = WriteCoreDump(writer);

                    kill(config->ProcessId, SIGCONT);

                    if(config->NumberOfDumpsCollected >= config->NumberOfDumpsToCollect)
                    {
                        // If we are over the max number of dumps to collect, send the original signal we intercepted.
                        kill(config->ProcessId, signum);    
                        pthread_mutex_unlock(&ptrace_mutex);                    
                        break;
                    }

                    ptrace(PTRACE_CONT, config->ProcessId, NULL, signum);

                    // Re-attach to the target process
                    if (ptrace(PTRACE_SEIZE, config->ProcessId, NULL, NULL) == -1)
                    {
                        Log(error, "Unable to PTRACE the target process");
                        pthread_mutex_unlock(&ptrace_mutex);
                        break;
                    }

                    pthread_mutex_unlock(&ptrace_mutex);
                    continue;
                }

                // Resume execution of the target process
                ptrace(PTRACE_CONT, config->ProcessId, NULL, signum);
                pthread_mutex_unlock(&ptrace_mutex);
            }        
        }
    }

    free(writer);
    Trace("SignalMonitoringThread: Exiting SignalMonitoring Thread");
    pthread_exit(NULL);
}

void *CpuMonitoringThread(void *thread_args /* struct ProcDumpConfiguration* */)
{
    Trace("CpuMonitoringThread: Starting Trigger Thread");
    struct ProcDumpConfiguration *config = (struct ProcDumpConfiguration *)thread_args;

    unsigned long totalTime = 0;
    unsigned long elapsedTime = 0;
    struct sysinfo sysInfo;
    int cpuUsage;
    struct CoreDumpWriter *writer = NewCoreDumpWriter(CPU, config);

    int rc = 0;
    struct ProcessStat proc = {0};

    if ((rc = WaitForQuitOrEvent(config, &config->evtStartMonitoring, INFINITE_WAIT)) == WAIT_OBJECT_0 + 1)
    {
        while ((rc = WaitForQuit(config, config->PollingInterval)) == WAIT_TIMEOUT)
        {
            sysinfo(&sysInfo);

            if (GetProcessStat(config->ProcessId, &proc))
            {
                // Calc CPU
                totalTime = (unsigned long)((proc.utime + proc.stime) / HZ);
                elapsedTime = (unsigned long)(sysInfo.uptime - (long)(proc.starttime / HZ));
                cpuUsage = (int)(100 * ((double)totalTime / elapsedTime));

                // CPU Trigger
                if ((config->bCpuTriggerBelowValue && (cpuUsage < config->CpuThreshold)) ||
                    (!config->bCpuTriggerBelowValue && (cpuUsage >= config->CpuThreshold)))
                {
                    Log(info, "CPU:\t%d%%", cpuUsage);
                    rc = WriteCoreDump(writer);

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

    free(writer);
    Trace("CpuTCpuMonitoringThread: Exiting Trigger Thread");
    pthread_exit(NULL);
}

void *TimerThread(void *thread_args /* struct ProcDumpConfiguration* */)
{
    Trace("TimerThread: Starting Trigger Thread");

    struct ProcDumpConfiguration *config = (struct ProcDumpConfiguration *)thread_args;
    struct CoreDumpWriter *writer = NewCoreDumpWriter(TIME, config);

    int rc = 0;

    if ((rc = WaitForQuitOrEvent(config, &config->evtStartMonitoring, INFINITE_WAIT)) == WAIT_OBJECT_0 + 1)
    {
        while ((rc = WaitForQuit(config, 0)) == WAIT_TIMEOUT)
        {
            Log(info, "Timed:");
            rc = WriteCoreDump(writer);

            if ((rc = WaitForQuit(config, config->ThresholdSeconds * 1000)) != WAIT_TIMEOUT) {
                break;
            }
        }
    }

    free(writer);
    Trace("TimerThread: Exiting Trigger Thread");
    pthread_exit(NULL);
}
