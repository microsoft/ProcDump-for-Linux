// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License

//--------------------------------------------------------------------
//
// The global configuration structure and utilities header
//
//--------------------------------------------------------------------
#include "Includes.h"

extern pthread_mutex_t LoggerLock;
long HZ;                                                        // clock ticks per second
int MAXIMUM_CPU;                                                // maximum cpu usage percentage (# cores * 100)
struct ProcDumpConfiguration g_config;                          // backbone of the program
struct ProcDumpConfiguration * target_config;                   // list of configs for target group processes or matching names
extern pthread_mutex_t activeConfigurationsMutex;

sigset_t sig_set;

//--------------------------------------------------------------------
//
// ApplyDefaults - Apply default values to configuration
//
//--------------------------------------------------------------------
void ApplyDefaults(struct ProcDumpConfiguration *self)
{
    if(self->NumberOfDumpsToCollect == -1)
    {
        self->NumberOfDumpsToCollect = DEFAULT_NUMBER_OF_DUMPS;
    }

    if(self->ThresholdSeconds == -1)
    {
        self->ThresholdSeconds = DEFAULT_DELTA_TIME;
    }

    if(self->PollingInterval == -1)
    {
        self->PollingInterval = MIN_POLLING_INTERVAL;
    }

    if(self->SampleRate == 0)
    {
        self->SampleRate = DEFAULT_SAMPLE_RATE;
    }
}

//--------------------------------------------------------------------
//
// InitProcDump - initalize procdump
//
//--------------------------------------------------------------------
void InitProcDump()
{
    openlog("ProcDump", LOG_PID, LOG_USER);
    if(CheckKernelVersion(MIN_KERNEL_VERSION, MIN_KERNEL_PATCH) == false)
    {
        Log(error, "ProcDump requires kernel version %d.%d+.", MIN_KERNEL_VERSION, MIN_KERNEL_PATCH);
        exit(-1);
    }
    InitProcDumpConfiguration(&g_config);
    pthread_mutex_init(&LoggerLock, NULL);
    pthread_mutex_init(&activeConfigurationsMutex, NULL);

    sigemptyset (&sig_set);
    sigaddset (&sig_set, SIGINT);
    sigaddset (&sig_set, SIGTERM);
    pthread_sigmask (SIG_BLOCK, &sig_set, NULL);

    char* prefixTmpFolder = NULL;

    // Create the directories where our sockets will be stored
    // If $TMPDIR is set, use it as the path, otherwise we use /tmp
    prefixTmpFolder = getenv("TMPDIR");
    if(prefixTmpFolder==NULL)
    {
        createDir("/tmp/procdump", 0777);
    }
    else
    {
        int len = strlen(prefixTmpFolder) + strlen("/procdump") + 1;
        char* t = (char*) malloc(len);
        if(t == NULL)
        {
            Log(error, INTERNAL_ERROR);
            Trace("InitProcDump: failed to allocate memory.");
            exit(-1);
        }

        snprintf(t, len, "%s%s", prefixTmpFolder, "/procdump");
        createDir(t, 0777);
        free(t);
    }
}

//--------------------------------------------------------------------
//
// ExitProcDump - cleanup during exit.
//
//--------------------------------------------------------------------
void ExitProcDump()
{
    Trace("ExitProcDump: Enter");
    pthread_mutex_destroy(&LoggerLock);
    closelog();

    // Try to delete the profiler lib and restrack program in case
    // they were left over...
    unlink(PROCDUMP_DIR "/" PROFILER_FILE_NAME);

    Trace("ExitProcDump: Exit");
}

//--------------------------------------------------------------------
//
// InitProcDumpConfiguration - initalize a config
//
//--------------------------------------------------------------------
void InitProcDumpConfiguration(struct ProcDumpConfiguration *self)
{
    MAXIMUM_CPU = 100 * (int)sysconf(_SC_NPROCESSORS_ONLN);
    HZ = sysconf(_SC_CLK_TCK);

#ifdef __linux__
    sysinfo(&(self->SystemInfo));
#endif

#ifdef __linux__
    pthread_mutex_init(&self->ptrace_mutex, NULL);
    pthread_mutex_init(&self->memAllocMapMutex, NULL);
#endif

    InitNamedEvent(&(self->evtCtrlHandlerCleanupComplete.event), true, false, const_cast<char*>("CtrlHandlerCleanupComplete"));
    self->evtCtrlHandlerCleanupComplete.type = EVENT;

    InitNamedEvent(&(self->evtBannerPrinted.event), true, false, const_cast<char*>("BannerPrinted"));
    self->evtBannerPrinted.type = EVENT;

    InitNamedEvent(&(self->evtConfigurationPrinted.event), true, false, const_cast<char*>("ConfigurationPrinted"));
    self->evtConfigurationPrinted.type = EVENT;

    InitNamedEvent(&(self->evtDebugThreadInitialized.event), true, false, const_cast<char*>("DebugThreadInitialized"));
    self->evtDebugThreadInitialized.type = EVENT;

    InitNamedEvent(&(self->evtQuit.event), true, false, const_cast<char*>("Quit"));
    self->evtQuit.type = EVENT;

    InitNamedEvent(&(self->evtStartMonitoring.event), true, false, const_cast<char*>("StartMonitoring"));
    self->evtStartMonitoring.type = EVENT;

    //sem_init(&(self->semAvailableDumpSlots.semaphore), 0, 1);
    self->semAvailableDumpSlots.semaphore = sem_open("/procdump_sem", O_CREAT, 0644, 1);
    self->semAvailableDumpSlots.type = SEMAPHORE;

    // Additional initialization
    self->ProcessId =                   NO_PID;
    self->bProcessGroup =               false;
    self->ProcessGroup =                NO_PID;
    self->NumberOfDumpsCollected =      0;
    self->NumberOfLeakReportsCollected = 0;
    self->NumberOfDumpsToCollect =      -1;
    self->CpuThreshold =                -1;
    self->bCpuTriggerBelowValue =       false;
    self->MemoryThreshold =             NULL;
    self->MemoryThresholdCount =        -1;
    self->MemoryCurrentThreshold =      0;
    self->bMonitoringGCMemory =         false;
    self->DumpGCGeneration =            -1;
    self->ThreadThreshold =             -1;
    self->FileDescriptorThreshold =     -1;
    self->SignalNumber =                NULL;
    self->SignalCount =                 0;
    self->ThresholdSeconds =            -1;
    self->bMemoryTriggerBelowValue =    false;
    self->bTimerThreshold =             false;
    self->WaitingForProcessName =       false;
    self->DiagnosticsLoggingEnabled =   none;
    self->gcorePid =                    NO_PID;
    self->PollingInterval =             -1;
    self->CoreDumpPath =                NULL;
    self->CoreDumpName =                NULL;
    self->nQuit =                       0;
    self->bDumpOnException =            false;
    self->bDumpOnException =            false;
    self->ExceptionFilter =             NULL;
    self->ExcludeFilter =               NULL;
    self->bRestrackEnabled =            false;
    self->bRestrackGenerateDump =       true;
    self->bLeakReportInProgress =       false;
    self->SampleRate =                  0;
    self->CoreDumpMask =                -1;

    self->socketPath =                  NULL;
    self->statusSocket =                -1;

    self->bSocketInitialized =          false;
    self->bExitProcessMonitor =         false;
    pthread_mutex_init(&self->dotnetMutex, NULL);
    pthread_cond_init(&self->dotnetCond, NULL);

#ifdef __linux__
    if(self->memAllocMap.size() > 0)
    {
        self->memAllocMap.clear();
    }
#endif
}

//--------------------------------------------------------------------
//
// FreeProcDumpConfiguration - ensure destruction of config and contents
//
//--------------------------------------------------------------------
void FreeProcDumpConfiguration(struct ProcDumpConfiguration *self)
{
    Trace("FreeProcDumpConfiguration: Enter");
    DestroyEvent(&(self->evtCtrlHandlerCleanupComplete.event));
    DestroyEvent(&(self->evtBannerPrinted.event));
    DestroyEvent(&(self->evtConfigurationPrinted.event));
    DestroyEvent(&(self->evtDebugThreadInitialized.event));
    DestroyEvent(&(self->evtQuit.event));
    DestroyEvent(&(self->evtStartMonitoring.event));

    pthread_mutex_destroy(&self->ptrace_mutex);
#ifdef __linux__
    pthread_mutex_destroy(&self->memAllocMapMutex);
#endif
    //sem_destroy(&(self->semAvailableDumpSlots.semaphore));
    sem_close(self->semAvailableDumpSlots.semaphore);
    sem_unlink("/procdump_sem");


    pthread_mutex_destroy(&self->dotnetMutex);
    pthread_cond_destroy(&self->dotnetCond);

    if(self->ProcessName)
    {
        free(self->ProcessName);
        self->ProcessName = NULL;
    }

    if(self->statusSocket != -1)
    {
        close(self->statusSocket);
        self->statusSocket = -1;
    }

    if(self->socketPath)
    {
        unlink(self->socketPath);
        free(self->socketPath);
        self->socketPath = NULL;
    }

    if(self->ExceptionFilter)
    {
        free(self->ExceptionFilter);
        self->ExceptionFilter = NULL;
    }

    if(self->ExcludeFilter)
    {
        free(self->ExcludeFilter);
        self->ExcludeFilter = NULL;
    }

    if(self->CoreDumpPath)
    {
        free(self->CoreDumpPath);
        self->CoreDumpPath = NULL;
    }

    if(self->CoreDumpName)
    {
        free(self->CoreDumpName);
        self->CoreDumpName = NULL;
    }

    if(self->MemoryThreshold)
    {
        free(self->MemoryThreshold);
        self->MemoryThreshold = NULL;
    }

    if(self->SignalNumber)
    {
        free(self->SignalNumber);
        self->SignalNumber = NULL;
    }

#ifdef __linux__
    for (const auto& pair : self->memAllocMap)
    {
        if(pair.second)
        {
            free(pair.second);
        }
    }
    self->memAllocMap.clear();
#endif

    Trace("FreeProcDumpConfiguration: Exit");
}


//--------------------------------------------------------------------
//
// CopyProcDumpConfiguration - deep copy of Procdump Config struct
//
//--------------------------------------------------------------------
struct ProcDumpConfiguration * CopyProcDumpConfiguration(struct ProcDumpConfiguration *self)
{
    struct ProcDumpConfiguration * copy = new ProcDumpConfiguration();

    if(copy != NULL)
    {
        // Init new struct
        InitProcDumpConfiguration(copy);

        copy->bExitProcessMonitor = self->bExitProcessMonitor;

        // copy target data we need from original config
        copy->ProcessId = self->ProcessId;
        copy->bProcessGroup = self->bProcessGroup;
        copy->ProcessGroup = self->ProcessGroup;
        copy->ProcessName = self->ProcessName == NULL ? NULL : strdup(self->ProcessName);

        // copy runtime values from original config
        copy->NumberOfDumpsCollecting = self->NumberOfDumpsCollecting;
        copy->NumberOfDumpsCollected = self->NumberOfDumpsCollected;
        copy->NumberOfLeakReportsCollected = self->NumberOfLeakReportsCollected;
        copy->bTerminated = self->bTerminated;

        // copy trigger behavior from original config
        copy->bTriggerThenSnoozeCPU = self->bTriggerThenSnoozeCPU;
        copy->bTriggerThenSnoozeMemory = self->bTriggerThenSnoozeMemory;
        copy->bTriggerThenSnoozeTimer = self->bTriggerThenSnoozeTimer;

        // copy options from original config
        copy->CpuThreshold = self->CpuThreshold;
        copy->bCpuTriggerBelowValue = self->bCpuTriggerBelowValue;
        if(self->MemoryThreshold != NULL)
        {
            copy->NumberOfDumpsToCollect = self->NumberOfDumpsToCollect;
            copy->MemoryCurrentThreshold = self->MemoryCurrentThreshold;
            copy->MemoryThreshold = (int*) malloc(self->NumberOfDumpsToCollect*sizeof(int));
            if(copy->MemoryThreshold == NULL)
            {
                Trace("Failed to alloc memory for MemoryThreshold");
                if(copy->ProcessName)
                {
                    free(copy->ProcessName);
                }

                return NULL;
            }

            memcpy(copy->MemoryThreshold, self->MemoryThreshold, self->NumberOfDumpsToCollect*sizeof(int));
        }

        copy->bRestrackEnabled = self->bRestrackEnabled;
        copy->bRestrackGenerateDump = self->bRestrackGenerateDump;
        copy->bLeakReportInProgress = self->bLeakReportInProgress;
        copy->SampleRate = self->SampleRate;
        copy->CoreDumpMask = self->CoreDumpMask;
        copy->bMemoryTriggerBelowValue = self->bMemoryTriggerBelowValue;
        copy->MemoryThresholdCount = self->MemoryThresholdCount;
        copy->bMonitoringGCMemory = self->bMonitoringGCMemory;
        copy->DumpGCGeneration = self->DumpGCGeneration;
        copy->ThresholdSeconds = self->ThresholdSeconds;
        copy->bTimerThreshold = self->bTimerThreshold;
        copy->NumberOfDumpsToCollect = self->NumberOfDumpsToCollect;
        copy->WaitingForProcessName = self->WaitingForProcessName;
        copy->DiagnosticsLoggingEnabled = self->DiagnosticsLoggingEnabled;
        copy->ThreadThreshold = self->ThreadThreshold;
        copy->FileDescriptorThreshold = self->FileDescriptorThreshold;

        if(self->SignalNumber != NULL)
        {
            copy->SignalCount = self->SignalCount;
            copy->SignalNumber = (int*) malloc(self->SignalCount*sizeof(int));
            if(copy->SignalNumber == NULL)
            {
                Trace("Failed to alloc memory for SignalNumber");
                if(copy->ProcessName)
                {
                    free(copy->ProcessName);
                }

                if(copy->MemoryThreshold)
                {
                    free(copy->MemoryThreshold);
                }

                return NULL;
            }

            memcpy(copy->SignalNumber, self->SignalNumber, self->SignalCount*sizeof(int));
        }

        copy->PollingInterval = self->PollingInterval;
        copy->CoreDumpPath = self->CoreDumpPath == NULL ? NULL : strdup(self->CoreDumpPath);
        copy->CoreDumpName = self->CoreDumpName == NULL ? NULL : strdup(self->CoreDumpName);
        copy->ExceptionFilter = self->ExceptionFilter == NULL ? NULL : strdup(self->ExceptionFilter);
        copy->ExcludeFilter = self->ExcludeFilter == NULL ? NULL : strdup(self->ExcludeFilter);
        copy->socketPath = self->socketPath == NULL ? NULL : strdup(self->socketPath);
        copy->bDumpOnException = self->bDumpOnException;
        copy->statusSocket = self->statusSocket;
#ifdef __linux__
        copy->memAllocMap = self->memAllocMap;
#endif
        return copy;
    }
    else
    {
        Trace("Failed to alloc memory for Procdump config copy");
        return NULL;
    }
}


//--------------------------------------------------------------------
//
// GetOptions - Unpack command line inputs
//
//--------------------------------------------------------------------
int GetOptions(struct ProcDumpConfiguration *self, int argc, char *argv[])
{
    bool bProcessSpecified = false;
    int dotnetTriggerCount = 0;

    if (argc < 2) {
        Trace("GetOptions: Invalid number of command line arguments.");
        return PrintUsage();
    }

    for( int i = 1; i < argc; i++ )
    {
        if (0 == strcasecmp( argv[i], "/?" ) || 0 == strcasecmp( argv[i], "-?" ))
        {
            return PrintUsage();
        }
        else if ( 0 == strcasecmp( argv[i], "/c" ) ||
                   0 == strcasecmp( argv[i], "-c" ) ||
                   0 == strcasecmp( argv[i], "/cl" ) ||
                   0 == strcasecmp( argv[i], "-cl" ))
        {
            if( i+1 >= argc || self->CpuThreshold != -1 ) return PrintUsage();
            if(!ConvertToInt(argv[i+1], &self->CpuThreshold)) return PrintUsage();

            if(self->CpuThreshold < 0)
            {
                Log(error, "Invalid CPU threshold count specified.");
                return PrintUsage();
            }

            if( 0 == strcasecmp( argv[i], "/cl" ) || 0 == strcasecmp( argv[i], "-cl"))
            {
                self->bCpuTriggerBelowValue = true;
            }

            i++;
        }
        else if( 0 == strcasecmp( argv[i], "/m" ) ||
                    0 == strcasecmp( argv[i], "-m" ) ||
                    0 == strcasecmp( argv[i], "/ml" ) ||
                    0 == strcasecmp( argv[i], "-ml" ))
        {
            if( i+1 >= argc || self->MemoryThresholdCount != -1 ) return PrintUsage();
            self->MemoryThreshold = GetSeparatedValues(argv[i+1], const_cast<char*>(","), &self->MemoryThresholdCount);

            if(self->MemoryThreshold == NULL || self->MemoryThresholdCount == 0) return PrintUsage();

            for(int i = 0; i < self->MemoryThresholdCount; i++)
            {
                if(self->MemoryThreshold[i] < 0)
                {
                    Log(error, "Invalid memory threshold specified.");
                    free(self->MemoryThreshold);
                    return PrintUsage();
                }
            }

            if( 0 == strcasecmp( argv[i], "/ml" ) || 0 == strcasecmp( argv[i], "-ml" ))
            {
                self->bMemoryTriggerBelowValue = true;
            }

            i++;
        }
#ifdef __linux__
        else if( 0 == strcasecmp( argv[i], "/gcm" ) ||
                    0 == strcasecmp( argv[i], "-gcm" ))
        {
            if( i+1 >= argc || self->MemoryThresholdCount != -1) return PrintUsage();
            if(strchr(argv[i+1], ':') != NULL)
            {
                char* token = NULL;
                char* copy = strdup(argv[i+1]);
                if(copy == NULL)
                {
                    Trace("Failed to strdup.");
                    Log(error, INTERNAL_ERROR);
                    return 1;
                }

                token = strtok(copy, ":");
                if(token != NULL)
                {
                    if(!ConvertToInt(token, &self->DumpGCGeneration))
                    {
                        if(strcasecmp(token, "loh") == 0)
                        {
                            self->DumpGCGeneration = 3;
                        }
                        else if(strcasecmp(token, "poh") == 0)
                        {
                            self->DumpGCGeneration = 4;
                        }
                        else
                        {
                            free(copy);
                            return PrintUsage();
                        }
                    }

                    token = strtok(NULL, ":");
                    if(token == NULL)
                    {
                        free(copy);
                        return PrintUsage();
                    }

                    self->MemoryThreshold = GetSeparatedValues(token, const_cast<char*>(","), &self->MemoryThresholdCount);
                }
                else
                {
                    free(copy);
                    return PrintUsage();
                }

                free(copy);
            }
            else
            {
                self->DumpGCGeneration = CUMULATIVE_GC_SIZE;        // Indicates that we want to check against total managed heap size (across all generations)
                self->MemoryThreshold = GetSeparatedValues(argv[i+1], const_cast<char*>(","), &self->MemoryThresholdCount);
            }

            for(int i = 0; i < self->MemoryThresholdCount; i++)
            {
                if(self->MemoryThreshold[i] < 0)
                {
                    Log(error, "Invalid memory threshold specified.");
                    free(self->MemoryThreshold);
                    return PrintUsage();
                }
            }

            if(self->DumpGCGeneration < 0 || (self->DumpGCGeneration > MAX_GC_GEN+2 && self->DumpGCGeneration != CUMULATIVE_GC_SIZE))   // +2 for LOH and POH
            {
                Log(error, "Invalid GC generation or heap specified.");
                free(self->MemoryThreshold);
                return PrintUsage();
            }

            dotnetTriggerCount++;
            self->bMonitoringGCMemory = true;
            i++;
        }
        else if( 0 == strcasecmp( argv[i], "/gcgen" ) ||
                    0 == strcasecmp( argv[i], "-gcgen" ))
        {
            if( i+1 >= argc || self->DumpGCGeneration != -1 ) return PrintUsage();
            if(!ConvertToInt(argv[i+1], &self->DumpGCGeneration)) return PrintUsage();
            if(self->DumpGCGeneration < 0 || self->DumpGCGeneration > MAX_GC_GEN)
            {
                Log(error, "Invalid GC generation specified.");
                return PrintUsage();
            }

            self->NumberOfDumpsToCollect = 2;               // This accounts for 1 dump at the start of the GC and 1 at the end.
            dotnetTriggerCount++;
            i++;
        }
        else if( 0 == strcasecmp( argv[i], "/restrack" ) ||
                    0 == strcasecmp( argv[i], "-restrack" ))
        {
            if(CheckKernelVersion(MIN_RESTRACK_KERNEL_VERSION, MIN_RESTRACK_KERNEL_PATCH) == false)
            {
                Log(error, "Restrack requires kernel version %d.%d+.", MIN_RESTRACK_KERNEL_VERSION, MIN_RESTRACK_KERNEL_PATCH);
                return PrintUsage();
            }

            if( i+1 >= argc)
            {
                return PrintUsage();
            }

            if(strcasecmp(argv[i+1], "nodump") == 0 )
            {
                self->bRestrackGenerateDump = false;
                i++;
            }

            self->bRestrackEnabled = true;
        }
        else if( 0 == strcasecmp( argv[i], "/sr" ) ||
                    0 == strcasecmp( argv[i], "-sr" ))
        {
            if( i+1 >= argc  ) return PrintUsage();
            if(!ConvertToInt(argv[i+1], &self->SampleRate)) return PrintUsage();
            if(self->SampleRate < 0)
            {
                Log(error, "Invalid sample rate specified.");
                return PrintUsage();
            }

            i++;
        }
        else if( 0 == strcasecmp( argv[i], "/sig" ) ||
                    0 == strcasecmp( argv[i], "-sig" ))
        {
            if( i+1 >= argc || self->SignalCount != 0 ) return PrintUsage();
            self->SignalNumber = GetSeparatedValues(argv[i+1], const_cast<char*>(","), &self->SignalCount);

            if(self->SignalNumber == NULL || self->SignalCount == 0) return PrintUsage();

            for(int i = 0; i < self->SignalCount; i++)
            {
                if(self->SignalNumber[i] < 0)
                {
                    Log(error, "Invalid signal specified.");
                    free(self->SignalNumber);
                    return PrintUsage();
                }
            }

            i++;
        }
        else if( 0 == strcasecmp( argv[i], "/mc" ) ||
                    0 == strcasecmp( argv[i], "-mc" ))
        {
            if( i+1 >= argc || self->CoreDumpMask != -1 ) return PrintUsage();

            if(ConvertToIntHex(argv[i+1], &self->CoreDumpMask) == false) return PrintUsage();
            if(self->CoreDumpMask < 0)
            {
                Log(error, "Invalid core dump mask specified.");
                return PrintUsage();
            }

            i++;
        }
#endif
        else if( 0 == strcasecmp( argv[i], "/tc" ) ||
                    0 == strcasecmp( argv[i], "-tc" ))
        {
            if( i+1 >= argc || self->ThreadThreshold != -1 ) return PrintUsage();
            if(!ConvertToInt(argv[i+1], &self->ThreadThreshold)) return PrintUsage();
            if(self->ThreadThreshold < 0)
            {
                Log(error, "Invalid thread threshold count specified.");
                return PrintUsage();
            }

            i++;
        }
        else if( 0 == strcasecmp( argv[i], "/fc" ) ||
                    0 == strcasecmp( argv[i], "-fc" ))
        {
            if( i+1 >= argc || self->FileDescriptorThreshold != -1 ) return PrintUsage();
            if(!ConvertToInt(argv[i+1], &self->FileDescriptorThreshold)) return PrintUsage();
            if(self->FileDescriptorThreshold < 0)
            {
                Log(error, "Invalid file descriptor threshold count specified.");
                return PrintUsage();
            }

            i++;
        }
        else if( 0 == strcasecmp( argv[i], "/pf" ) ||
                    0 == strcasecmp( argv[i], "-pf" ))
        {
            if( i+1 >= argc || self->PollingInterval != -1 ) return PrintUsage();
            if(!ConvertToInt(argv[i+1], &self->PollingInterval)) return PrintUsage();
            if(self->PollingInterval < 0)
            {
                Log(error, "Invalid polling inverval specified.");
                return PrintUsage();
            }

            i++;
        }
        else if( 0 == strcasecmp( argv[i], "/n" ) ||
                    0 == strcasecmp( argv[i], "-n" ))
        {
            if( i+1 >= argc || self->NumberOfDumpsToCollect != -1) return PrintUsage();
            if(!ConvertToInt(argv[i+1], &self->NumberOfDumpsToCollect)) return PrintUsage();
            if(self->NumberOfDumpsToCollect < 0)
            {
                Log(error, "Invalid number of dumps specified.");
                return PrintUsage();
            }

            if(self->NumberOfDumpsToCollect > MAX_DUMP_COUNT)
            {
                Log(error, "Max dump count must be less than %d.", MAX_DUMP_COUNT);
                return PrintUsage();
            }

            i++;
        }
        else if( 0 == strcasecmp( argv[i], "/s" ) ||
                    0 == strcasecmp( argv[i], "-s" ))
        {
            if( i+1 >= argc || self->ThresholdSeconds != -1 ) return PrintUsage();
            if(!ConvertToInt(argv[i+1], &self->ThresholdSeconds)) return PrintUsage();
            if(self->ThresholdSeconds < 0)
            {
                Log(error, "Invalid seconds specified.");
                return PrintUsage();
            }

            i++;
        }
        else if( 0 == strcasecmp( argv[i], "/log" ) ||
                    0 == strcasecmp( argv[i], "-log" ))
        {
            if( i+1 >= argc) return PrintUsage();

            if( 0 == strcasecmp( argv[i+1], "stdout" ) )
            {
                self->DiagnosticsLoggingEnabled = diag_stdout;
            }
            else if( 0 == strcasecmp( argv[i+1], "syslog" ) )
            {
                self->DiagnosticsLoggingEnabled = diag_syslog;
            }
            else
            {
                Log(error, "Invalid diagnostics stream specified.");
                return PrintUsage();
            }

            i++;
        }
#ifdef __linux__
        else if( 0 == strcasecmp( argv[i], "/e" ) ||
                    0 == strcasecmp( argv[i], "-e" ))
        {
            if( i+1 >= argc) return PrintUsage();
            dotnetTriggerCount++;
            self->bDumpOnException = true;
        }
        else if( 0 == strcasecmp( argv[i], "/f" ) ||
                   0 == strcasecmp( argv[i], "-f" ))
        {
            if( i+1 >= argc || self->ExceptionFilter)
            {
                if(self->ExceptionFilter)
                {
                    free(self->ExceptionFilter);
                }

                return PrintUsage();
            }

            self->ExceptionFilter = strdup(argv[i+1]);
            if(self->ExceptionFilter==NULL)
            {
                Log(error, INTERNAL_ERROR);
                Trace("GetOptions: failed to strdup ExceptionFilter");
                return -1;
            }
            if( tolower( self->ExceptionFilter[0] ) >  'z' || ( self->ExceptionFilter[0] != '*' && tolower( self->ExceptionFilter[0] ) <  'a' ) )
            {
                free(self->ExceptionFilter);
                return PrintUsage();
            }

            i++;
        }
        else if( 0 == strcasecmp( argv[i], "/fx" ) ||
                   0 == strcasecmp( argv[i], "-fx" ))
        {
            if( i+1 >= argc || self->ExcludeFilter)
            {
                if(self->ExcludeFilter)
                {
                    free(self->ExcludeFilter);
                }

                return PrintUsage();
            }

            self->ExcludeFilter = strdup(argv[i+1]);
            if(self->ExcludeFilter==NULL)
            {
                Log(error, INTERNAL_ERROR);
                Trace("GetOptions: failed to strdup ExcludeFilter");
                return -1;
            }

            i++;
        }
#endif
        else if( 0 == strcasecmp( argv[i], "/o" ) ||
                    0 == strcasecmp( argv[i], "-o" ))
        {
            self->bOverwriteExisting = true;
        }
        else if( 0 == strcasecmp( argv[i], "/w" ) ||
                    0 == strcasecmp( argv[i], "-w" ))
        {
            self->WaitingForProcessName = true;
        }
#ifdef __linux__
        else if( 0 == strcasecmp( argv[i], "/pgid" ) ||
                    0 == strcasecmp( argv[i], "-pgid" ))
        {
            self->bProcessGroup = true;
        }
#endif
        else
        {
            // Process targets
            int j;
            if( bProcessSpecified && self->CoreDumpPath )
            {
                return PrintUsage();
            } else if(!bProcessSpecified)
            {
                bProcessSpecified = true;
                bool isPid = true;

                for( j = 0; j < (int) strlen( argv[i]); j++ )
                {

                    if( !isdigit( argv[i][j]) )
                    {

                        isPid = false;
                        break;
                    }
                }
                if( !isPid )
                {

                    self->ProcessName = strdup(argv[i]);
                    if(self->ProcessName==NULL)
                    {
                        Log(error, INTERNAL_ERROR);
                        Trace("GetOptions: failed to strdup ProcessName");
                        return -1;
                    }
                } else
                {
                    if(self->bProcessGroup)
                    {
                        if( !sscanf( argv[i], "%d", &self->ProcessGroup ))
                        {

                            return PrintUsage();
                        }
                    }
                    else
                    {
                        if( !sscanf( argv[i], "%d", &self->ProcessId ))
                        {

                            return PrintUsage();
                        }
                    }
                }

            } else if(!self->CoreDumpPath)
            {
                char *tempOutputPath = NULL;
                tempOutputPath = strdup(argv[i]);
                if(tempOutputPath==NULL)
                {
                    Log(error, INTERNAL_ERROR);
                    Trace("GetOptions: failed to strdup tempOutputPath");
                    return -1;
                }

                struct stat statbuf;

                // Check if the user provided an existing directory or a path
                // ending in a '/'. In this case, use the default naming
                // convention but place the files in the given directory.
                if ((stat(tempOutputPath, &statbuf) == 0 && S_ISDIR(statbuf.st_mode)) ||
                        tempOutputPath[strlen(tempOutputPath)-1] == '/') {
                    self->CoreDumpPath = tempOutputPath;
                    self->CoreDumpName = NULL;
                } else {
                    self->CoreDumpPath = strdup(dirname(tempOutputPath));
                    free(tempOutputPath);
                    if(self->CoreDumpPath==NULL)
                    {
                        Log(error, INTERNAL_ERROR);
                        Trace("GetOptions: failed to strdup CoreDumpPath");
                        return -1;
                    }

                    tempOutputPath = strdup(argv[i]);
                    if(tempOutputPath==NULL)
                    {
                        Log(error, INTERNAL_ERROR);
                        Trace("GetOptions: failed to strdup tempOutputPath");
                        return -1;
                    }
                    self->CoreDumpName = strdup(basename(tempOutputPath));
                    if(self->CoreDumpName==NULL)
                    {
                        Log(error, INTERNAL_ERROR);
                        Trace("GetOptions: failed to strdup CoreDumpName");
                        return -1;
                    }

                    free(tempOutputPath);
                }

                // Check if the path portion of the output format is valid
                if (stat(self->CoreDumpPath, &statbuf) < 0 || !S_ISDIR(statbuf.st_mode)) {
                    Log(error, "Invalid directory (\"%s\") provided for core dump output.",
                        self->CoreDumpPath);
                    return PrintUsage();
                }
            }
        }
    }

    //
    // Validate multi arguments
    //

#ifdef __linux__
    // .NET triggers are mutually exclusive
    if(dotnetTriggerCount > 1)
    {
        Log(error, "Only one .NET trigger can be specified.");
        return PrintUsage();
    }
#endif

    // Ensure consistency between number of thresholds specified and the -n switch
    if(self->MemoryThresholdCount > 1 && self->NumberOfDumpsToCollect != -1)
    {
        Log(error, "When specifying more than one memory threshold the number of dumps switch (-n) is invalid.");
        return PrintUsage();
    }

    if(self->MemoryThresholdCount > 1)
    {
        self->NumberOfDumpsToCollect = self->MemoryThresholdCount;
    }

#ifdef __linux__
    // If exception filter is provided with no -e switch exit
    if((self->ExceptionFilter && self->bDumpOnException == false))
    {
        Log(error, "Please use the -e switch when specifying an exception filter (-f)");
        return PrintUsage();
    }

    // If sample rate is specified it also requires restrack
    if((self->SampleRate > 0 && self->bRestrackEnabled == false))
    {
        Log(error, "Please use the -restrack switch when specifying a sample rate (-samplerate)");
        return PrintUsage();
    }


    // Make sure exclude filter is provided with switches that supports exclusion.
    if((self->ExcludeFilter && self->bRestrackEnabled == false))
    {
        Log(error, "Please use the -restrack switch when specifying an exclude filter (-fx)");
        return PrintUsage();
    }
#endif
    // If no path was provided, assume the current directory
    if (self->CoreDumpPath == NULL) {
        self->CoreDumpPath = strdup(".");
        if(self->CoreDumpPath==NULL)
        {
            Log(error, INTERNAL_ERROR);
            Trace("GetOptions: failed to strdup CoreDumpPath");
            return -1;
        }

    }

    // Wait
    if((self->WaitingForProcessName && self->ProcessId != NO_PID))
    {
        Log(error, "The wait option requires the process be specified by name.");
        return PrintUsage();
    }

    // If number of dumps to collect is set, but there is no other criteria, enable Timer here...
    if ((self->CpuThreshold == -1) &&
        (self->MemoryThreshold == NULL) &&
        (self->ThreadThreshold == -1) &&
        (self->FileDescriptorThreshold == -1) &&
        (self->DumpGCGeneration == -1) &&
        (self->SignalCount == 0))
    {
        self->bTimerThreshold = true;
    }

#ifdef __linux__
    // Signal trigger can only be specified alone
    if(self->SignalCount > 0 || self->bDumpOnException)
    {
        if(self->CpuThreshold != -1 || self->ThreadThreshold != -1 || self->FileDescriptorThreshold != -1 || self->MemoryThreshold != NULL)
        {
            Log(error, "Signal/Exception trigger must be the only trigger specified.");
            return PrintUsage();
        }
        if(self->PollingInterval != -1)
        {
            Log(error, "Polling interval has no meaning during Signal/Exception monitoring.");
            return PrintUsage();
        }

        // Again, we cant have another trigger (in this case timer) kicking off another dump generation since we will already
        // be attached via ptrace.
        self->bTimerThreshold = false;
    }
#endif
    // If we are monitoring multiple process, setting dump name doesn't make sense (path is OK)
    if ((self->bProcessGroup || self->WaitingForProcessName) && self->CoreDumpName)
    {
        Log(error, "Setting core dump name in multi process monitoring is invalid (path is ok).");
        return PrintUsage();
    }

    // Apply default values for any config values that were not specified by user
    ApplyDefaults(self);

    Trace("GetOpts and initial Configuration finished");
    return 0;
}

//--------------------------------------------------------------------
//
// PrintConfiguration - Prints the current configuration to the command line
//
//--------------------------------------------------------------------
bool PrintConfiguration(struct ProcDumpConfiguration *self)
{
    if (WaitForSingleObject(&self->evtConfigurationPrinted,0) == WAIT_TIMEOUT)
    {
        if(self->SignalCount > 0)
        {
            printf("** NOTE ** Signal triggers use PTRACE which will impact the performance of the target process\n\n");
        }

        if (self->bProcessGroup)
        {
            printf("%-40s%d\n", "Process Group:", self->ProcessGroup);
        }
        else if (self->WaitingForProcessName)
        {
            printf("%-40s%s\n", "Process Name:", self->ProcessName);
        }
        else
        {
            printf("%-40s%s (%d)\n", "Process:", self->ProcessName, self->ProcessId);
        }

        // CPU
        if (self->CpuThreshold != -1)
        {
            if (self->bCpuTriggerBelowValue)
            {
                printf("%-40s< %d%%\n", "CPU Threshold:", self->CpuThreshold);
            }
            else
            {
                printf("%-40s>= %d%%\n", "CPU Threshold:", self->CpuThreshold);
            }
        }
        else
        {
            printf("%-40s%s\n", "CPU Threshold:", "n/a");
        }

        // Memory
        if (self->MemoryThreshold != NULL)
        {
            if (self->bMemoryTriggerBelowValue)
            {
                printf("%-40s< ", "Commit Threshold:");
            }
            else
            {
                if(self->bMonitoringGCMemory == true)
                {
                    printf("%-40s>= ", ".NET Memory Threshold:");
                }
                else
                {
                    printf("%-40s>= ", "Commit Threshold:");
                }
            }

            for(int i=0; i<self->NumberOfDumpsToCollect; i++)
            {
                printf("%d MB", self->MemoryThreshold[i]);
                if(i < self->NumberOfDumpsToCollect -1)
                {
                    printf(",");
                }
            }

            printf("\n");
        }
        else
        {
            printf("%-40s%s\n", "Commit Threshold:", "n/a");
        }

        // Thread
        if (self->ThreadThreshold != -1)
        {
            printf("%-40s%d\n", "Thread Threshold:", self->ThreadThreshold);
        }
        else
        {
            printf("%-40s%s\n", "Thread Threshold:", "n/a");
        }

        // File descriptor
        if (self->FileDescriptorThreshold != -1)
        {
            printf("%-40s%d\n", "File Descriptor Threshold:", self->FileDescriptorThreshold);
        }
        else
        {
            printf("%-40s%s\n", "File Descriptor Threshold:", "n/a");
        }

#ifdef __linux__
        // GC Generation
        if (self->DumpGCGeneration != -1)
        {
            printf("%-40s", "GC Generation/heap:");

            if(self->DumpGCGeneration == CUMULATIVE_GC_SIZE)
            {
                printf("Cumulative\n");
            }
            else if(self->DumpGCGeneration == 3)
            {
                printf("LOH\n");
            }
            else if(self->DumpGCGeneration == 4)
            {
                printf("POH\n");
            }
            else
            {
                printf("%d\n", self->DumpGCGeneration);
            }
        }
        else
        {
            printf("%-40s%s\n", "GC Generation:", "n/a");
        }
        // Restrack
        if (self->bRestrackEnabled == true)
        {
            printf("%-40s%s\n", "Resource tracking:", "On");
            printf("%-40s%d\n", "Resource tracking sample rate:", self->SampleRate);
        }
        else
        {
            printf("%-40s%s\n", "Resource tracking:", "n/a");
            printf("%-40s%s\n", "Resource tracking sample rate:", "n/a");
        }
        // Signal
        if (self->SignalCount > 0)
        {
            printf("%-40s", "Signal(s):");
            for(int i=0; i<self->SignalCount; i++)
            {
                printf("%d", self->SignalNumber[i]);
                if(i < self->SignalCount -1)
                {
                    printf(",");
                }
                else
                {
                    printf("\n");
                }
            }
        }
        else
        {
            printf("%-40s%s\n", "Signal:", "n/a");
        }
        // Exception
        if (self->bDumpOnException)
        {
            printf("%-40s%s\n", "Exception monitor:", "On");
            printf("%-40s%s\n", "Exception filter:", self->ExceptionFilter ? self->ExceptionFilter : "n/a");
        }
        else
        {
            printf("%-40s%s\n", "Exception monitor:", "n/a");
        }
        // Exclude filter
        if (self->ExcludeFilter)
        {
            printf("%-40s%s\n", "Exclude filter:", self->ExcludeFilter);
        }
#endif

        // Polling inverval
        printf("%-40s%d\n", "Polling Interval (ms):", self->PollingInterval);

        // time
        printf("%-40s%d\n", "Threshold (s):", self->ThresholdSeconds);

        // number of dumps and others
        printf("%-40s%d\n", "Number of Dumps:", self->NumberOfDumpsToCollect);

        // Output directory and filename
        printf("%-40s%s\n", "Output directory:", self->CoreDumpPath);
        if (self->CoreDumpName != NULL)
        {
            printf("%-40s%s_<counter>\n", "Custom name for core dumps:", self->CoreDumpName);
        }

        SetEvent(&self->evtConfigurationPrinted.event);
        return true;
    }

    return false;
}

//--------------------------------------------------------------------
//
// PrintBanner - Not re-entrant safe banner printer. Function must be called before trigger threads start.
//
//--------------------------------------------------------------------
void PrintBanner()
{
    printf("\nProcDump v%s - Sysinternals process dump utility\n", STRFILEVER);
    printf("Copyright (C) 2025 Microsoft Corporation. All rights reserved. Licensed under the MIT license.\n");
    printf("Mark Russinovich, Mario Hewardt, John Salem, Javid Habibi\n");
    printf("Sysinternals - www.sysinternals.com\n\n");

    printf("Monitors one or more processes and writes a core dump file when the processes exceeds the\n");
    printf("specified criteria.\n\n");
}


//--------------------------------------------------------------------
//
// PrintUsage - Print usage
//
//--------------------------------------------------------------------
int PrintUsage()
{
    printf("\nCapture Usage: \n");
    printf("   procdump [-n Count]\n");
    printf("            [-s Seconds]\n");
    printf("            [-c|-cl CPU_Usage]\n");
    printf("            [-m|-ml Commit_Usage1[,Commit_Usage2...]]\n");
    printf("            [-tc Thread_Threshold]\n");
    printf("            [-fc FileDescriptor_Threshold]\n");
#ifdef __linux__
    printf("            [-gcm [<GCGeneration>: | LOH: | POH:]Memory_Usage1[,Memory_Usage2...]]\n");
    printf("            [-gcgen Generation]\n");
    printf("            [-restrack [nodump]]\n");
    printf("            [-sr Sample_Rate]\n");
    printf("            [-sig Signal_Number1[,Signal_Number2...]]\n");
    printf("            [-e]\n");
    printf("            [-f Include_Filter,...]\n");
    printf("            [-fx Exclude_Filter]\n");
    printf("            [-mc Custom_Dump_Mask]\n");
#endif
    printf("            [-pf Polling_Frequency]\n");
    printf("            [-o]\n");
    printf("            [-log syslog|stdout]\n");
    printf("            {\n");
#ifdef __linux__
    printf("             {{[-w] Process_Name | [-pgid] PID} [Dump_File | Dump_Folder]}\n");
#elif defined(__APPLE__)
    printf("             {{[-w] Process_Name | PID} [Dump_File | Dump_Folder]}\n");
#endif
    printf("            }\n");
    printf("\n");
    printf("Options:\n");
    printf("   -n      Number of dumps to write before exiting.\n");
    printf("   -s      Consecutive seconds before dump is written (default is 10).\n");
    printf("   -c      CPU threshold above which to create a dump of the process.\n");
    printf("   -cl     CPU threshold below which to create a dump of the process.\n");
    printf("   -tc     Thread count threshold above which to create a dump of the process.\n");
    printf("   -fc     File descriptor count threshold above which to create a dump of the process.\n");
#ifdef __linux__
    printf("   -m      Memory commit threshold(s) (MB) above which to create dumps.\n");
    printf("   -ml     Memory commit threshold(s) (MB) below which to create dumps.\n");
    printf("   -gcm    [.NET] GC memory threshold(s) (MB) above which to create dumps for the specified generation or heap (default is total .NET memory usage).\n");
    printf("   -gcgen  [.NET] Create dump when the garbage collection of the specified generation starts and finishes.\n");
    printf("   -restrack Enable memory leak tracking (malloc family of APIs). Use the nodump option to prevent dump generation and only produce restrack report(s).\n");
    printf("   -sr     Sample rate when using -restrack.\n");
    printf("   -sig    Comma separated list of signal number(s) during which any signal results in a dump of the process.\n");
    printf("   -e      [.NET] Create dump when the process encounters an exception.\n");
    printf("   -f      Filter (include) on the content of .NET exceptions (comma separated). Wildcards (*) are supported.\n");
    printf("   -fx     Filter (exclude) on the content of -restrack call stacks. Wildcards (*) are supported.\n");
    printf("   -mc     Custom core dump mask (in hex) indicating what memory should be included in the core dump. Please see 'man core' (/proc/[pid]/coredump_filter) for available options.\n");
    printf("   -pgid   Process ID specified refers to a process group ID.\n");
#endif
    printf("   -pf     Polling frequency.\n");
    printf("   -o      Overwrite existing dump file.\n");
    printf("   -log    Writes extended ProcDump tracing to the specified output stream (syslog or stdout).\n");
    printf("   -w      Wait for the specified process to launch if it's not running.\n");

    return -1;
}
