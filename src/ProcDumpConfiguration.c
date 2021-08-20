// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License

//--------------------------------------------------------------------
//
// The global configuration structure and utilities header
//
//--------------------------------------------------------------------

#include "Procdump.h"
#include "ProcDumpConfiguration.h"

struct Handle g_evtConfigurationInitialized = HANDLE_MANUAL_RESET_EVENT_INITIALIZER("ConfigurationInitialized");

static sigset_t sig_set;
static pthread_t sig_thread_id;
static pthread_t sig_monitor_thread_id;
extern pthread_mutex_t LoggerLock;
long HZ;                                // clock ticks per second
int MAXIMUM_CPU;                        // maximum cpu usage percentage (# cores * 100)
struct ProcDumpConfiguration g_config;  // backbone of the program

//--------------------------------------------------------------------
//
// SignalThread - Thread for handling graceful Async signals (e.g., SIGINT, SIGTERM)
//
//--------------------------------------------------------------------
void *SignalThread(void *input)
{
    struct ProcDumpConfiguration *config = (struct ProcDumpConfiguration *)input;
    int sig_caught, rc;

    if ((rc = sigwait(&sig_set, &sig_caught)) != 0) {
        Log(error, "Failed to wait on signal");
        exit(-1);
    }
    
    switch (sig_caught)
    {
    case SIGINT:
        SetQuit(config, 1);
        if(config->gcorePid != NO_PID) {
            Log(info, "Shutting down gcore");
            if((rc = kill(-config->gcorePid, SIGKILL)) != 0) {            // pass negative PID to kill entire PGRP with value of gcore PID
                Log(error, "Failed to shutdown gcore.");
            }
        }

        // Need to make sure we detach from ptrace (if not attached it will silently fail)
        // To avoid situations where we have intercepted a signal and CTRL-C is hit, we synchronize
        // access to the signal path (in SignalMonitoringThread). Note, there is still a race but
        // acceptable since it is very unlikely to occur. We also cancel the SignalMonitorThread to
        // break it out of waitpid call. 
        if(config->SignalNumber != -1)
        {
            pthread_mutex_lock(&ptrace_mutex);
            ptrace(PTRACE_DETACH, config->ProcessId, 0, 0);
            pthread_mutex_unlock(&ptrace_mutex);

            if ((rc = pthread_cancel(sig_monitor_thread_id)) != 0) {
                Log(error, "An error occurred while canceling SignalMonitorThread.\n");
                exit(-1);
            }
        }

        Log(info, "Quit");
        break;
    default:
        fprintf (stderr, "\nUnexpected signal %d\n", sig_caught);
        break;
    }

    pthread_exit(NULL);
}

//--------------------------------------------------------------------
//
// InitProcDump - initalize procdump
//
//--------------------------------------------------------------------
void InitProcDump()
{
    openlog("ProcDump", LOG_PID, LOG_USER);
    if(CheckKernelVersion() == false)
    {
        Log(error, "Kernel version lower than 3.5+.");
        exit(-1);
    }
    InitProcDumpConfiguration(&g_config);
    pthread_mutex_init(&LoggerLock, NULL);
    pthread_mutex_init(&ptrace_mutex, NULL);
}

//--------------------------------------------------------------------
//
// ExitProcDump - cleanup during exit. 
//
//--------------------------------------------------------------------
void ExitProcDump()
{
    pthread_mutex_destroy(&LoggerLock);
    closelog();
    FreeProcDumpConfiguration(&g_config);
}

//--------------------------------------------------------------------
//
// InitProcDumpConfiguration - initalize a config
//
//--------------------------------------------------------------------
void InitProcDumpConfiguration(struct ProcDumpConfiguration *self)
{
    if (WaitForSingleObject(&g_evtConfigurationInitialized, 0) == WAIT_OBJECT_0) {
        return; // The configuration has already been initialized
    }

    MAXIMUM_CPU = 100 * (int)sysconf(_SC_NPROCESSORS_ONLN);
    HZ = sysconf(_SC_CLK_TCK);

    sysinfo(&(self->SystemInfo));

    InitNamedEvent(&(self->evtCtrlHandlerCleanupComplete.event), true, false, "CtrlHandlerCleanupComplete");
    self->evtCtrlHandlerCleanupComplete.type = EVENT;

    InitNamedEvent(&(self->evtBannerPrinted.event), true, false, "BannerPrinted");
    self->evtBannerPrinted.type = EVENT;

    InitNamedEvent(&(self->evtConfigurationPrinted.event), true, false, "ConfigurationPrinted");
    self->evtConfigurationPrinted.type = EVENT;

    InitNamedEvent(&(self->evtDebugThreadInitialized.event), true, false, "DebugThreadInitialized");
    self->evtDebugThreadInitialized.type = EVENT;

    InitNamedEvent(&(self->evtQuit.event), true, false, "Quit");
    self->evtQuit.type = EVENT;

    InitNamedEvent(&(self->evtStartMonitoring.event), true, false, "StartMonitoring");
    self->evtStartMonitoring.type = EVENT;

    sem_init(&(self->semAvailableDumpSlots.semaphore), 0, 1);
    self->semAvailableDumpSlots.type = SEMAPHORE;

    // Additional initialization
    self->ProcessId =                   NO_PID;
    self->NumberOfDumpsCollected =      0;
    self->NumberOfDumpsToCollect =      DEFAULT_NUMBER_OF_DUMPS;
    self->CpuThreshold =                -1;
    self->MemoryThreshold =             -1;
    self->ThreadThreshold =             -1;
    self->FileDescriptorThreshold =     -1;
    self->SignalNumber =                -1;
    self->ThresholdSeconds =            DEFAULT_DELTA_TIME;
    self->bCpuTriggerBelowValue =       false;
    self->bMemoryTriggerBelowValue =    false;
    self->bTimerThreshold =             false;
    self->WaitingForProcessName =       false;
    self->DiagnosticsLoggingEnabled =   false;
    self->gcorePid =                    NO_PID;
    self->PollingInterval =             MIN_POLLING_INTERVAL;
    self->CoreDumpPath =                NULL;
    self->CoreDumpName =                NULL;

    SetEvent(&g_evtConfigurationInitialized.event); // We've initialized and are now re-entrant safe
}


//--------------------------------------------------------------------
//
// FreeProcDumpConfiguration - ensure destruction of config and contents
//
//--------------------------------------------------------------------
void FreeProcDumpConfiguration(struct ProcDumpConfiguration *self)
{
    DestroyEvent(&(self->evtCtrlHandlerCleanupComplete.event));
    DestroyEvent(&(self->evtBannerPrinted.event));
    DestroyEvent(&(self->evtConfigurationPrinted.event));
    DestroyEvent(&(self->evtDebugThreadInitialized.event));
    DestroyEvent(&(self->evtQuit.event));
    DestroyEvent(&(self->evtStartMonitoring.event));

    sem_destroy(&(self->semAvailableDumpSlots.semaphore));

    if(strcmp(self->ProcessName, EMPTY_PROC_NAME) != 0){
        // The string constant is not on the heap.
        free(self->ProcessName);
    }

    free(self->CoreDumpPath);
    free(self->CoreDumpName);
}

//--------------------------------------------------------------------
//
// GetOptions - Unpack command line inputs
//
//--------------------------------------------------------------------
int GetOptions(struct ProcDumpConfiguration *self, int argc, char *argv[])
{
    // Make sure config has been initialized
    if (WaitForSingleObject(&g_evtConfigurationInitialized, 0) != WAIT_OBJECT_0) {
        Trace("GetOptions: Configuration not initialized.");
        return -1;
    }

    if (argc < 2) {
        Trace("GetOptions: Invalid number of command line arguments.");
        return PrintUsage(self);
    }

    // parse arguments
	int next_option;
    int option_index = 0;
    const char* short_options = "+p:C:c:M:m:n:s:w:T:F:G:I:o:dh";
    const struct option long_options[] = {
    	{ "pid",                       required_argument,  NULL,           'p' },
    	{ "cpu",                       required_argument,  NULL,           'C' },
    	{ "lower-cpu",                 required_argument,  NULL,           'c' },
    	{ "memory",                    required_argument,  NULL,           'M' },
    	{ "lower-mem",                 required_argument,  NULL,           'm' },
        { "number-of-dumps",           required_argument,  NULL,           'n' },
        { "time-between-dumps",        required_argument,  NULL,           's' },
        { "wait",                      required_argument,  NULL,           'w' },
        { "threads",                   required_argument,  NULL,           'T' },
        { "filedescriptors",           required_argument,  NULL,           'F' },
        { "signal",                    required_argument,  NULL,           'G' },
        { "pollinginterval",           required_argument,  NULL,           'I' },                        
        { "output-path",               required_argument,  NULL,           'o' },
        { "diag",                      no_argument,        NULL,           'd' },
        { "help",                      no_argument,        NULL,           'h' },
        { NULL,                        0,                  NULL,            0  }
    };

    char *tempOutputPath = NULL;
    struct stat statbuf;

    // start parsing command line arguments
    while ((next_option = getopt_long(argc, argv, short_options, long_options, &option_index)) != -1) {
        switch (next_option) {
            case 'p':
                self->ProcessId = (pid_t)atoi(optarg);
                if (!LookupProcessByPid(self)) {
                    Log(error, "Invalid PID - failed looking up process name by PID.");
                    return PrintUsage(self);
                }
                break;

            case 'C':
                if (self->CpuThreshold != -1 || !IsValidNumberArg(optarg) ||
                    (self->CpuThreshold = atoi(optarg)) < 0 || self->CpuThreshold > MAXIMUM_CPU) {
                    Log(error, "Invalid CPU threshold specified.");
                    return PrintUsage(self);
                }
                break;

            case 'I':
                if (!IsValidNumberArg(optarg) || (self->PollingInterval = atoi(optarg)) < 0 || self->PollingInterval < MIN_POLLING_INTERVAL) {
                    Log(error, "Invalid polling interval specified (minimum %d).", MIN_POLLING_INTERVAL);
                    return PrintUsage(self);
                }
                break;

            case 'T':
                if (self->ThreadThreshold != -1 || !IsValidNumberArg(optarg) ||
                    (self->ThreadThreshold = atoi(optarg)) < 0 ) {
                    Log(error, "Invalid threads threshold specified.");
                    return PrintUsage(self);
                }
                break;

            case 'F':
                if (self->FileDescriptorThreshold != -1 || !IsValidNumberArg(optarg) ||
                    (self->FileDescriptorThreshold = atoi(optarg)) < 0 ) {
                    Log(error, "Invalid file descriptor threshold specified.");
                    return PrintUsage(self);
                }
                break;

            case 'G':
                if (self->SignalNumber != -1 || !IsValidNumberArg(optarg) ||
                    (self->SignalNumber = atoi(optarg)) < 0 ) {
                    Log(error, "Invalid signal specified.");
                    return PrintUsage(self);
                }
                break;                

            case 'c':
                if (self->CpuThreshold != -1 || !IsValidNumberArg(optarg) ||
                    (self->CpuThreshold = atoi(optarg)) < 0 || self->CpuThreshold > MAXIMUM_CPU) {
                    Log(error, "Invalid CPU threshold specified.");
                    return PrintUsage(self);
                }
                self->bCpuTriggerBelowValue = true;
                break;

            case 'M':
                if (self->MemoryThreshold != -1 || 
                    !IsValidNumberArg(optarg) ||
                    (self->MemoryThreshold = atoi(optarg)) < 0) {
                    Log(error, "Invalid memory threshold specified.");
                    return PrintUsage(self);
                }
                break;

            case 'm':
                if (self->MemoryThreshold != -1 || 
                    !IsValidNumberArg(optarg) ||
                    (self->MemoryThreshold = atoi(optarg)) < 0) {
                    Log(error, "Invalid memory threshold specified.");
                    return PrintUsage(self);
                }            
                self->bMemoryTriggerBelowValue = true;
                break;

            case 'n':
                if (!IsValidNumberArg(optarg) ||
                    (self->NumberOfDumpsToCollect = atoi(optarg)) < 0) {
                    Log(error, "Invalid dumps threshold specified.");
                    return PrintUsage(self);
                }                
                break;

            case 's':
                if (!IsValidNumberArg(optarg) ||
                    (self->ThresholdSeconds = atoi(optarg)) == 0) {
                    Log(error, "Invalid time threshold specified.");
                    return PrintUsage(self);
                }
                break;

            case 'w':
                self->WaitingForProcessName = true;
                self->ProcessName = strdup(optarg);
                break;

            case 'o':
                tempOutputPath = strdup(optarg);

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
                    tempOutputPath = strdup(optarg);
                    self->CoreDumpName = strdup(basename(tempOutputPath));
                    free(tempOutputPath);
                }

                // Check if the path portion of the output format is valid
                if (stat(self->CoreDumpPath, &statbuf) < 0 || !S_ISDIR(statbuf.st_mode)) {
                    Log(error, "Invalid directory (\"%s\") provided for core dump output.",
                        self->CoreDumpPath);
                    return PrintUsage(self);
                }
                break;

            case 'd':
                self->DiagnosticsLoggingEnabled = true;
                break;

            case 'h':
                return PrintUsage(self);

            default:
                Log(error, "Invalid switch specified");
                return PrintUsage(self);
        }
    }

    // If no path was provided, assume the current directory
    if (self->CoreDumpPath == NULL) {
        self->CoreDumpPath = strdup(".");
    }

    // Check for multi-arg situations
    // if number of dumps is set, but no thresholds, just go on timer
    if (self->NumberOfDumpsToCollect != -1 &&
        self->MemoryThreshold == -1 &&
        self->CpuThreshold == -1 &&
        self->ThreadThreshold == -1 &&
        self->FileDescriptorThreshold == -1) {
            self->bTimerThreshold = true;
        }


    // If signal dump is specified, it can be the only trigger that is used.
    // Otherwise we might run into a situation where the other triggers invoke
    // gcore while the target is being ptraced due to signal trigger.
    // Interval has no meaning during signal monitoring.
    // 
    if(self->SignalNumber != -1) 
    {
        if(self->CpuThreshold != -1 || self->ThreadThreshold != -1 || self->FileDescriptorThreshold != -1 || self->MemoryThreshold != -1)
        {
            Log(error, "Signal trigger must be the only trigger specified.");
            return PrintUsage(self);            
        }
        if(self->PollingInterval != MIN_POLLING_INTERVAL)
        {
            Log(error, "Polling interval has no meaning during signal monitoring.");
            return PrintUsage(self);            
        }

        // Again, we cant have another trigger (in this case timer) kicking off another dump generation since we will already
        // be attached via ptrace. 
        self->bTimerThreshold = false;
    }


    if(self->ProcessId == NO_PID && !self->WaitingForProcessName){
        Log(error, "A valid PID or process name must be specified");
        return PrintUsage(self);
    }

    if(self->ProcessId != NO_PID && self->WaitingForProcessName){
	Log(error, "Please only specify one of -p or -w");
	return PrintUsage(self);
    }

    if(!self->WaitingForProcessName) {
        self->ProcessName = GetProcessName(self->ProcessId);
    }

    Trace("GetOpts and initial Configuration finished");

    return 0;
}


//--------------------------------------------------------------------
//
// LookupProcessByPid - Find process using PID provided.  
//
//--------------------------------------------------------------------
bool LookupProcessByPid(struct ProcDumpConfiguration *self)
{
    char statFilePath[32];

    // check to see if pid is an actual process running
    sprintf(statFilePath, "/proc/%d/stat", self->ProcessId);
    FILE *fd = fopen(statFilePath, "r");
    if (fd == NULL) {
        Log(error, "No process matching the specified PID can be found.");
        Log(error, "Try elevating the command prompt (i.e., `sudo procdump ...`)");
        return false;
    }

    // close file pointer this is a valid process
    fclose(fd);
    return true;
}


//--------------------------------------------------------------------
//
// FilterForPid - Helper function for scandir to only return PIDs.
//
//--------------------------------------------------------------------
static int FilterForPid(const struct dirent *entry)
{
    return IsValidNumberArg(entry->d_name);
}

//--------------------------------------------------------------------
//
// WaitForProcessName - Actively wait until a process with the configured name is launched.
//
//--------------------------------------------------------------------
bool WaitForProcessName(struct ProcDumpConfiguration *self)
{
    Log(info, "Waiting for process '%s' to launch...", self->ProcessName);
    while (true) {
        struct dirent ** nameList;
        bool moreThanOne = false;
        pid_t matchingPid = NO_PID;
        int numEntries = scandir("/proc/", &nameList, FilterForPid, alphasort);
        for (int i = 0; i < numEntries; i++) {
            pid_t procPid = atoi(nameList[i]->d_name);
            char *nameForPid = GetProcessName(procPid);
            if (strcmp(nameForPid, EMPTY_PROC_NAME) == 0) {
                continue;
            }
            if (strcmp(nameForPid, self->ProcessName) == 0) {
                if (matchingPid == NO_PID) {
                    matchingPid = procPid;
                } else {
                    Log(error, "More than one matching process found, exiting...");
                    moreThanOne = true;
                    free(nameForPid);
                    break;
                }
            }
            free(nameForPid);
        }
        // Cleanup
        for (int i = 0; i < numEntries; i++) {
            free(nameList[i]);
        }
        free(nameList);

        // Check for exactly one match
        if (moreThanOne) {
            self->bTerminated = true;
            return false;
        } else if (matchingPid != NO_PID) {
            self->ProcessId = matchingPid;
            Log(info, "Found process with PID %d", matchingPid);
            return true;
        }
    }
}

//--------------------------------------------------------------------
//
// GetProcessName - Get process name using PID provided.
//                  Returns EMPTY_PROC_NAME for null process name.
//
//--------------------------------------------------------------------
char * GetProcessName(pid_t pid){
	char procFilePath[32];
	char fileBuffer[MAX_CMDLINE_LEN];
	int charactersRead = 0;
	int	itr = 0;
	char * stringItr;
	char * processName;
	FILE * procFile;
	
	
        if(sprintf(procFilePath, "/proc/%d/cmdline", pid) < 0){
		return EMPTY_PROC_NAME;
	}

	procFile = fopen(procFilePath, "r");

	if(procFile != NULL){
		if(fgets(fileBuffer, MAX_CMDLINE_LEN, procFile) == NULL) {
	    		fclose(procFile);
			if(strlen(fileBuffer) == 0){
                		Log(debug, "Empty cmdline.\n");
            		}else{
	        		Log(debug, "Failed to read from %s.\n", procFilePath);
			}
			return EMPTY_PROC_NAME;
		}
		// close file
		fclose(procFile);
	}
	else{
		Log(debug, "Failed to open %s.\n", procFilePath);
		return EMPTY_PROC_NAME;
	}
	

	// Extract process name
	stringItr = fileBuffer;
	charactersRead  = strlen(fileBuffer);
	for(int i = 0; i <= charactersRead; i++){
		if(fileBuffer[i] == '\0'){
			itr = i - itr;
			
			if(strcmp(stringItr, "sudo") != 0){		// do we have the process name including filepath?
				processName = strrchr(stringItr, '/');	// does this process include a filepath?
				
				if(processName != NULL){
					return strdup(processName + 1);	// +1 to not include '/' character
				}
				else{
					return strdup(stringItr);
				}
			}
			else{
				stringItr += (itr+1); 	// +1 to move past '\0'
			}
		}
	}

	Log(debug, "Failed to extract process name from /proc/PID/cmdline");
	return EMPTY_PROC_NAME;
}

//--------------------------------------------------------------------
//
// CreateTriggerThreads - Create each of the threads that will be running as a trigger 
//
//--------------------------------------------------------------------
int CreateTriggerThreads(struct ProcDumpConfiguration *self)
{    
    int rc = 0;
    self->nThreads = 0;

    if((rc=sigemptyset (&sig_set)) < 0)
    {
        Trace("CreateTriggerThreads: sigemptyset failed.");
        return rc;
    }
    if((rc=sigaddset (&sig_set, SIGINT)) < 0)
    {
        Trace("CreateTriggerThreads: sigaddset failed.");
        return rc;
    }
    if((rc=sigaddset (&sig_set, SIGTERM)) < 0)
    {
        Trace("CreateTriggerThreads: sigaddset failed.");
        return rc;
    }

    if((rc = pthread_sigmask (SIG_BLOCK, &sig_set, NULL)) != 0)
    {
        Trace("CreateTriggerThreads: pthread_sigmask failed.");
        return rc;
    }

    // create threads
    if (self->CpuThreshold != -1) {
        if ((rc = pthread_create(&self->Threads[self->nThreads++], NULL, CpuMonitoringThread, (void *)self)) != 0) {
            Trace("CreateTriggerThreads: failed to create CpuThread.");            
            return rc;
        }
    }

    if (self->MemoryThreshold != -1) {
        if ((rc = pthread_create(&self->Threads[self->nThreads++], NULL, CommitMonitoringThread, (void *)self)) != 0) {
            Trace("CreateTriggerThreads: failed to create CommitThread.");            
            return rc;
        }
    }

    if (self->ThreadThreshold != -1) {
        if ((rc = pthread_create(&self->Threads[self->nThreads++], NULL, ThreadCountMonitoringThread, (void *)self)) != 0) {
            Trace("CreateTriggerThreads: failed to create ThreadThread.");            
            return rc;
        }
    }

    if (self->FileDescriptorThreshold != -1) {
        if ((rc = pthread_create(&self->Threads[self->nThreads++], NULL, FileDescriptorCountMonitoringThread, (void *)self)) != 0) {
            Trace("CreateTriggerThreads: failed to create FileDescriptorThread.");            
            return rc;
        }
    }

    if (self->SignalNumber != -1) {
        if ((rc = pthread_create(&sig_monitor_thread_id, NULL, SignalMonitoringThread, (void *)self)) != 0) {
            Trace("CreateTriggerThreads: failed to create SignalMonitoringThread.");            
            return rc;
        }
    }

    if (self->bTimerThreshold) {
        if ((rc = pthread_create(&self->Threads[self->nThreads++], NULL, TimerThread, (void *)self)) != 0) {
            Trace("CreateTriggerThreads: failed to create TimerThread.");
            return rc;
        }
    }
    
    if((rc = pthread_create(&sig_thread_id, NULL, SignalThread, (void *)self))!= 0)
    {
        Trace("CreateTriggerThreads: failed to create SignalThread.");
        return rc;
    }

    return 0;
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


//--------------------------------------------------------------------
//
// WaitForAllThreadsToTerminate - Wait for all threads to terminate  
//
//--------------------------------------------------------------------
int WaitForAllThreadsToTerminate(struct ProcDumpConfiguration *self)
{
    int rc = 0;


    // Wait for the signal monitoring thread if there is one. If there is one, it will be the only
    // one. 
    if(self->SignalNumber != -1)
    {
        if ((rc = pthread_join(sig_monitor_thread_id, NULL)) != 0) {
            Log(error, "An error occurred while joining SignalMonitorThread.\n");
            exit(-1);
        }
    }
    else
    {
        // Wait for the other monitoring threads
        for (int i = 0; i < self->nThreads; i++) {
            if ((rc = pthread_join(self->Threads[i], NULL)) != 0) {
                Log(error, "An error occurred while joining threads\n");
                exit(-1);
            }
        }
    }

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
// PrintConfiguration - Prints the current configuration to the command line
//
//--------------------------------------------------------------------
bool PrintConfiguration(struct ProcDumpConfiguration *self)
{
    if (WaitForSingleObject(&self->evtConfigurationPrinted,0) == WAIT_TIMEOUT) {
        if(self->SignalNumber != -1)
        {
            printf("** NOTE ** Signal triggers use PTRACE which will impact the performance of the target process\n\n");
        }
        printf("Process:\t\t%s", self->ProcessName);
        if (!self->WaitingForProcessName) {
            printf(" (%d)", self->ProcessId);
        } else {
            printf(" (pending)");
        }
        printf("\n");

        // CPU
        if (self->CpuThreshold != -1) {
            if (self->bCpuTriggerBelowValue) {
                printf("CPU Threshold:\t\t<%d\n", self->CpuThreshold);
            } else {
                printf("CPU Threshold:\t\t>=%d\n", self->CpuThreshold);
            }
        } else {
            printf("CPU Threshold:\t\tn/a\n");
        }

        // Memory
        if (self->MemoryThreshold != -1) {
            if (self->bMemoryTriggerBelowValue) {
                printf("Commit Threshold:\t<%d\n", self->MemoryThreshold);
            } else {
                printf("Commit Threshold:\t>=%d\n", self->MemoryThreshold);
            }
        } else {
            printf("Commit Threshold:\tn/a\n");
        }

        // Thread
        if (self->ThreadThreshold != -1) {
            printf("Thread Threshold:\t>=%d\n", self->ThreadThreshold);
        }
        else {
            printf("Thread Threshold:\t\tn/a\n");
        }

        // File descriptor
        if (self->FileDescriptorThreshold != -1) {
            printf("File descriptor Threshold:\t>=%d\n", self->FileDescriptorThreshold);
        }
        else {
            printf("File descriptor Threshold:\t\tn/a\n");
        }

        // Signal
        if (self->SignalNumber != -1) {
            printf("Signal number:\t%d\n", self->SignalNumber);
        }
        else {
            printf("Signal:\t\tn/a\n");
        }        

        // Polling inverval
        printf("Polling interval (ms):\t%d\n", self->PollingInterval);

        // time
        printf("Threshold (s):\t%d\n", self->ThresholdSeconds);

        // number of dumps and others
        printf("Number of Dumps:\t%d\n", self->NumberOfDumpsToCollect);

        // Output directory and filename
        printf("Output directory for core dumps:\t%s\n", self->CoreDumpPath);
        if (self->CoreDumpName != NULL) {
            printf("Custom name for core dumps:\t%s_<counter>.<pid>\n", self->CoreDumpName);
        }

        SetEvent(&self->evtConfigurationPrinted.event);
        return true;
    }
    return false;
}


//--------------------------------------------------------------------
//
// ContinueMonitoring - Should we keep monitoring or should we clean up our thread 
//
//--------------------------------------------------------------------
bool ContinueMonitoring(struct ProcDumpConfiguration *self)
{
    // Have we reached the dump limit?
    if (self->NumberOfDumpsCollected >= self->NumberOfDumpsToCollect) {
        return false;
    }

    // Do we already know the process is terminated?
    if (self->bTerminated) {
        return false;
    }

    // Let's check to make sure the process is still alive then
    // note: kill([pid], 0) doesn't send a signal but does perform error checking
    //       therefore, if it returns 0, the process is still alive, -1 means it errored out
    if (kill(self->ProcessId, 0)) {
        self->bTerminated = true;
        Log(error, "Target process is no longer alive");
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

//--------------------------------------------------------------------
//
// IsValidNumberArg - quick helper function for ensuring arg is a number 
//
//--------------------------------------------------------------------
bool IsValidNumberArg(const char *arg)
{
    int strLen = strlen(arg);

    for (int i = 0; i < strLen; i++) {
        if (!isdigit(arg[i]) && !isspace(arg[i])) {
            return false;
        }
    }

    return true;
}

//--------------------------------------------------------------------
//
// CheckKernelVersion - Check to see if current kernel is 3.5+.
// 
// ProcDump won't proceed if current kernel is less than 3.5.
// Returns true if >= 3.5+, returns false otherwise or error.
//--------------------------------------------------------------------
bool CheckKernelVersion()
{
    struct utsname kernelInfo;
    if(uname(&kernelInfo) == 0)
    {
        int version, patch = 0;
        if(sscanf(kernelInfo.release,"%d.%d",&version,&patch) != 2)
        {
            Log(error, "Cannot validate kernel version");
            Trace("%s",strerror(errno));
            return false;
        }

        if(version > MIN_KERNEL_VERSION) return true;
        if(version == MIN_KERNEL_VERSION && patch >= MIN_KERNEL_PATCH) return true;

    }
    else
    {
        Log(error, strerror(errno));
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
    printf("\nProcDump v1.1.1 - Sysinternals process dump utility\n");
    printf("Copyright (C) 2020 Microsoft Corporation. All rights reserved. Licensed under the MIT license.\n");
    printf("Mark Russinovich, Mario Hewardt, John Salem, Javid Habibi\n");

    printf("Monitors a process and writes a dump file when the process meets the\n");
    printf("specified criteria.\n\n");
}


//--------------------------------------------------------------------
//
// PrintUsage - Print usage
//
//--------------------------------------------------------------------
int PrintUsage(struct ProcDumpConfiguration *self)
{
    printf("\nUsage: procdump [OPTIONS...] TARGET\n");
    printf("   OPTIONS\n");
    printf("      -h          Prints this help screen\n");
    printf("      -C          Trigger core dump generation when CPU exceeds or equals specified value (0 to 100 * nCPU)\n");
    printf("      -c          Trigger core dump generation when CPU is less than specified value (0 to 100 * nCPU)\n");
    printf("      -M          Trigger core dump generation when memory commit exceeds or equals specified value (MB)\n");
    printf("      -m          Trigger core dump generation when when memory commit is less than specified value (MB)\n");
    printf("      -T          Trigger when thread count exceeds or equals specified value.\n");
    printf("      -F          Trigger when file descriptor count exceeds or equals specified value.\n");  
    printf("      -G          Trigger when signal with the specified value (num) is sent (uses PTRACE and will affect performance of target process).\n");  
    printf("      -I          Polling frequency in milliseconds (default is %d)\n", MIN_POLLING_INTERVAL);        
    printf("      -n          Number of core dumps to write before exiting (default is %d)\n", DEFAULT_NUMBER_OF_DUMPS);
    printf("      -s          Consecutive seconds before dump is written (default is %d)\n", DEFAULT_DELTA_TIME);
    printf("      -o          Path and/or filename prefix where the core dump is written to\n");
    printf("      -d          Writes diagnostic logs to syslog\n");
    printf("   TARGET must be exactly one of these:\n");
    printf("      -p          pid of the process\n");
    printf("      -w          Name of the process executable\n\n");

    return -1;
}
