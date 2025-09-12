// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License

//--------------------------------------------------------------------
//
// Restrack.cpp
//
//--------------------------------------------------------------------
#define _Bool bool
#include "procdump_ebpf.skel.h"

#include <sys/time.h>
#include <sys/resource.h>

#include "Includes.h"

#include "bcc_elf.h"
#include "bcc_perf_map.h"
#include "bcc_proc.h"
#include "bcc_syms.h"

#include <vector>
#include <algorithm>
#include <string>
#include <fstream>
#include <memory>

typedef struct {
    unsigned int type;
    unsigned long allocCount;
    unsigned long allocSize;
    unsigned long totalAllocSize;
    unsigned int callStackLen;
    __u64 stackTrace[MAX_CALL_STACK_FRAMES];
} groupedAllocEntry;

typedef struct {
    std::string symbolName;
    std::string demangledSymbolName;
    char* fullName;
    uint64_t offset;
    __u64 pc;
} stackFrame;

typedef struct {
    ProcDumpConfiguration* config;
    const char* filename;
} leakThreadArgs;


extern std::unordered_map<int, ProcDumpConfiguration*> activeConfigurations;
extern pthread_mutex_t activeConfigurationsMutex;
extern struct ProcDumpConfiguration g_config;



// ------------------------------------------------------------------------------------------
// libbpf_print_fn
//
// Call back to print eBPF errors
// ------------------------------------------------------------------------------------------
static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args)
{
    if(g_config.DiagnosticsLoggingEnabled != none)
    {
        return vfprintf(stderr, format, args);
    }

    return 0;
}


// ------------------------------------------------------------------------------------------
// SetMaxRLimit
//
// Sets the rlimit to max. Required by eBPF.
// ------------------------------------------------------------------------------------------
void SetMaxRLimit()
{
    struct rlimit lim = {
        .rlim_cur = RLIM_INFINITY,
        .rlim_max = RLIM_INFINITY,
    };

    setrlimit(RLIMIT_MEMLOCK, &lim);
}

//--------------------------------------------------------------------
//
// StopRestrack
//
// Stops the Restrack eBPF program
//
//--------------------------------------------------------------------
void StopRestrack(struct procdump_ebpf* skel)
{
    procdump_ebpf__destroy(skel);
}

//--------------------------------------------------------------------
//
// RunRestrack
//
// Loads the restrack eBPF program and attaches to the memory alloc
// APIs.
//
//--------------------------------------------------------------------
struct procdump_ebpf* RunRestrack(struct ProcDumpConfiguration *config)
{
    int ret = -1;
    struct procdump_ebpf *skel = NULL;

    SetMaxRLimit();

    //
    // Setup extended error logging
    //
    if(config->DiagnosticsLoggingEnabled != none)
    {
        libbpf_set_print(libbpf_print_fn);
    }

    //
    // Open the eBPF program
    //
    skel = procdump_ebpf__open();
    if (!skel)
    {
        return skel;
    }

    //
    // Set eBPF program globals
    //
    std::string path = "/proc/" + std::to_string(config->ProcessId) + "/ns/pid";
    struct stat sb = {};
    if (stat(path.c_str(), &sb) == -1)
    {
        Trace("Failed to stat %s (%s)\n", path.c_str(), strerror(errno));
        return NULL;
    }

    skel->bss->dev = sb.st_dev;
    skel->bss->inode = sb.st_ino;
    skel->bss->target_PID = config->ProcessId;
    skel->bss->sampleRate = config->SampleRate;
    skel->bss->currentSampleCount = 1;
    if(config->DiagnosticsLoggingEnabled != none)
    {
        skel->bss->isLoggingEnabled = true;
    }

    ret = procdump_ebpf__load(skel);
    if (ret)
    {
        return NULL;
    }

    ret = procdump_ebpf__attach(skel);
    if (ret)
    {
        procdump_ebpf__destroy(skel);
        return NULL;
    }

    return skel;
}


// ------------------------------------------------------------------------------------------
// RestrackHandleEvent
//
// Handles events from the Restrack eBPF program
// ------------------------------------------------------------------------------------------
int RestrackHandleEvent(void *ctx, void *data, size_t data_sz)
{
    ResourceInformation* event = (ResourceInformation*) data;

    if(event->resourceType == RESTRACK_ALLOC)
    {
        //
        // We need to make a copy of the data otherwise the ring buffer might free/overwrite.
        //
        event = (ResourceInformation*) malloc(sizeof(ResourceInformation));
        memcpy(event, data, sizeof(ResourceInformation));

        //
        // Add to allocation map
        //
        pthread_mutex_lock(&activeConfigurationsMutex);
        if(activeConfigurations.find(event->pid) != activeConfigurations.end())
        {
            pthread_mutex_lock(&activeConfigurations[event->pid]->memAllocMapMutex);
            activeConfigurations[event->pid]->memAllocMap[(uintptr_t) event->allocAddress] = event;
            pthread_mutex_unlock(&activeConfigurations[event->pid]->memAllocMapMutex);

            if(activeConfigurations[event->pid]->DiagnosticsLoggingEnabled != none)
            {
                Trace("Got event: Alloc size: %ld 0x%lx\n", event->allocSize, event->allocAddress);
            }
        }

        pthread_mutex_unlock(&activeConfigurationsMutex);
    }
    else if (event->resourceType == RESTRACK_FREE)
    {
        pthread_mutex_lock(&activeConfigurationsMutex);
        if(activeConfigurations.find(event->pid) != activeConfigurations.end())
        {
            if(activeConfigurations[event->pid]->memAllocMap.find((uintptr_t) event->allocAddress) != activeConfigurations[event->pid]->memAllocMap.end())
            {
                //
                // If in the allocation map, remove the allocation
                //
                pthread_mutex_lock(&activeConfigurations[event->pid]->memAllocMapMutex);
                free(activeConfigurations[event->pid]->memAllocMap[(uintptr_t) event->allocAddress]);
                activeConfigurations[event->pid]->memAllocMap.erase((uintptr_t) event->allocAddress);
                pthread_mutex_unlock(&activeConfigurations[event->pid]->memAllocMapMutex);

                if(activeConfigurations[event->pid]->DiagnosticsLoggingEnabled != none)
                {
                    Trace("Got event: free 0x%lx\n", event->allocAddress);
                }
            }
        }
        pthread_mutex_unlock(&activeConfigurationsMutex);
    }

	return 0;
}

// ------------------------------------------------------------------------------------------
// strlwr
//
// Convert string to lowercase.
// ------------------------------------------------------------------------------------------
inline int strlwr(char* str, size_t len)
{
    for (size_t i = 0; i < len; ++i)
    {
        str[i] = tolower(str[i]);
    }
    return 0;
}

// ------------------------------------------------------------------------------------------
// WildcardSearch
//
// Same wild card search as the Windows version.
// ------------------------------------------------------------------------------------------
bool WildcardSearch(char* entry, char* search)
{
    if ((entry == NULL) || (search == NULL))
        return false;

    char* classLowerMalloc = (char*)malloc(sizeof(char)*(strlen(entry)+1));
    if (classLowerMalloc == NULL)
        return false;

    char* searchLowerMalloc = (char*)malloc(sizeof(char)*(strlen(search)+1));
    if (searchLowerMalloc == NULL)
    {
        free(classLowerMalloc);
        return false;
    }

    char* classLower = classLowerMalloc;
    strcpy(classLower, entry);
    strlwr(classLower, (strlen(entry)+1));

    char* searchLower = searchLowerMalloc;
    strcpy(searchLower, search);
    strlwr(searchLower, (strlen(search)+1));

    while ((*searchLower != '\0') && (*classLower != '\0'))
    {
        if (*searchLower != '*')
        {
            // Straight (case insensitive) compare
            if (*searchLower != *classLower)
            {
                free(classLowerMalloc);
                classLowerMalloc = NULL;

                free(searchLowerMalloc);
                searchLowerMalloc = NULL;

                return false;
            }

            searchLower++;
            classLower++;
            continue;
        }

        //
        // Wildcard processing
        //

ContinueWildcard:
        searchLower++;

        // The wildcard is on the end; e.g. '*' 'blah*' or '*blah*'
        // Must be a match
        if (*searchLower == '\0')
        {
            free(classLowerMalloc);
            classLowerMalloc = NULL;

            free(searchLowerMalloc);
            searchLowerMalloc = NULL;
            return true;
        }

        // Double Wildcard; e.g. '**' 'blah**' or '*blah**'
        if (*searchLower == '*')
            goto ContinueWildcard;

        // Find the length of the sub-string to search for
        int endpos = 0;
        while ((searchLower[endpos] != '\0') && (searchLower[endpos] != '*'))
            endpos++;

        // Find a match of the sub-search string anywhere within the class string
        int cc = 0; // Offset in to the Class
        int ss = 0; // Offset in to the Sub-Search
        while (ss < endpos)
        {
            if (classLower[ss+cc] == '\0')
            {
                free(classLowerMalloc);
                classLowerMalloc = NULL;

                free(searchLowerMalloc);
                searchLowerMalloc = NULL;

                return false;
            }

            if (searchLower[ss] != classLower[ss+cc])
            {
                cc++;
                ss = 0;
                continue;
            }
            ss++;
        }

        // If we get here, we found a match; move each string forward
        searchLower += ss;
        classLower += (ss + cc);
    }

    // Do we have a trailing wildcard?
    // This happens when Class = ABC.XYZ and Search = *XYZ*
    // Needed as the trailing wildcard code (above) doesn't run after the ss/cc search as Class is null
    while (*searchLower == '*')
    {
        searchLower++;
    }

    // If Class and Search have no residual, this is a match.
    if ((*searchLower == '\0') && (*classLower == '\0'))
    {
        free(classLowerMalloc);
        classLowerMalloc = NULL;

        free(searchLowerMalloc);
        searchLowerMalloc = NULL;

        return true;
    }

    free(classLowerMalloc);
    classLowerMalloc = NULL;

    free(searchLowerMalloc);
    searchLowerMalloc = NULL;

    return false;
}

// ------------------------------------------------------------------------------------------
// ReportLeaks
//
// Reports on leaks
// ------------------------------------------------------------------------------------------
void* ReportLeaks(void* args)
{
    Trace("ReportLeaks:Enter");
    leakThreadArgs* leakArgs = (leakThreadArgs*) args;
    ProcDumpConfiguration* config = leakArgs->config;
    const char* filename = leakArgs->filename;

    std::ofstream file(filename);
    if (!file)
    {
        Trace("ReportLeaks: Failed to open file: %s", filename);
        return NULL;
    }

    config->bLeakReportInProgress = true;

    bool symbolResolutionFailed = false;
    int symbolResolutionErrorCode = 0;
    if(config->memAllocMap.size() > 0)
    {
        void* symResolver = bcc_symcache_new(config->ProcessId, NULL);

        //
        // Since its a snapshot, copy the alloc map so we can avoid synchronization issues.
        //
        pthread_mutex_lock(&config->memAllocMapMutex);

        std::vector<groupedAllocEntry> groupedAllocations;

        //
        // Group the call stacks
        //
        for (const auto& pair : config->memAllocMap)
        {
            bool found = false;
            for(int i=0; i<(int) groupedAllocations.size(); i++)
            {
                if(groupedAllocations[i].callStackLen == pair.second->callStackLen && (groupedAllocations[i].allocSize == pair.second->allocSize || groupedAllocations[i].allocSize == 0))
                {
                    bool match = true;
                    for(int j=0; j<pair.second->callStackLen; j++)
                    {
                        if(groupedAllocations[i].stackTrace[j] != pair.second->stackTrace[j])
                        {
                            match = false;
                            break;
                        }
                    }

                    if(match == true)
                    {
                        groupedAllocations[i].allocCount++;
                        groupedAllocations[i].totalAllocSize += pair.second->allocSize;
                        found = true;
                        break;
                    }
                }
            }

            if(found == false)
            {
                groupedAllocEntry entry = {};
                entry.type = pair.second->resourceType;
                entry.allocCount = 1;
                entry.allocSize = pair.second->allocSize;
                entry.totalAllocSize = pair.second->allocSize;
                entry.callStackLen = pair.second->callStackLen;
                memcpy(entry.stackTrace, pair.second->stackTrace, sizeof(__u64) * pair.second->callStackLen);
                groupedAllocations.push_back(entry);
            }
        }

        pthread_mutex_unlock(&config->memAllocMapMutex);


        // Sort the vector based on the totalAllocSize field in ascending order
        std::sort(groupedAllocations.begin(), groupedAllocations.end(), [](const groupedAllocEntry& a, const groupedAllocEntry& b) {
            return a.totalAllocSize > b.totalAllocSize;
        });

        //
        // Print out the leaks
        //
        unsigned long totalLeak = 0;
        for (const auto& pair : groupedAllocations)
        {
            std::vector<stackFrame> callStack;
            for(unsigned int i=0; i<pair.callStackLen; i++)
            {
                //
                // Now we get the symbol information for the allocation call stacks that are outstanding.
                //
                if(pair.stackTrace[i] > 0)
                {
                    bcc_symbol sym;
                    int resolveResult = bcc_symcache_resolve(symResolver, pair.stackTrace[i], &sym);

                    stackFrame frame = {};
                    frame.offset = sym.offset;
                    if(sym.name != NULL)
                    {
                        frame.symbolName = sym.name;
                    }

                    if(sym.demangle_name != NULL)
                    {
                        frame.demangledSymbolName = sym.demangle_name;
                    }

                    // Track if symbol resolution failed
                    if(resolveResult != 0 || sym.name == NULL)
                    {
                        symbolResolutionFailed = true;
                        if(resolveResult != 0) {
                            symbolResolutionErrorCode = resolveResult;
                        }
                    }

                    frame.pc = pair.stackTrace[i];

                    int len = snprintf(NULL, 0, "\t[0x%llx] %s+0x%lx\n", pair.stackTrace[i], frame.demangledSymbolName.c_str(), frame.offset);
                    frame.fullName = (char*) malloc(len+1);
                    snprintf(frame.fullName, len, "\t[0x%llx] %s+0x%lx\n", pair.stackTrace[i], frame.demangledSymbolName.c_str(), frame.offset);

                    callStack.push_back(frame);
                }
            }

            //
            // If the stack contains an ignore frame, don't print it
            //
            bool found = false;
            if(config->ExcludeFilter != NULL)
            {
                for (const auto& st : callStack)
                {
                    if(WildcardSearch(st.fullName, config->ExcludeFilter) == true)
                    {
                        found = true;
                        break;
                    }
                }
            }

            if(found == false)
            {
                totalLeak += pair.totalAllocSize;

                file << "+++ Leaked Allocation [allocation size: 0x" << std::hex << pair.allocSize << " count:0x" << std::hex << pair.allocCount << " total size:0x" << std::hex << pair.totalAllocSize << "]\n";

                for (const auto& st : callStack)
                {
                    if(st.demangledSymbolName.length() > 0)
                    {
                        file << "\t[0x" << std::hex << st.pc << "] " << st.demangledSymbolName.c_str() << "+0x" << std::hex << st.offset << "\n";
                    }
                    else
                    {
                        file << "\t[0x" << std::hex << st.pc << "]\n";
                    }
                }

                file << "\n";
            }
        }

        file << "\nTotal leaked: 0x" << std::hex << totalLeak << "\n";
        
        // Inform user if symbol resolution failed
        if(symbolResolutionFailed)
        {
            file << "\nNote: Some stack frames could not be resolved. This may indicate missing debug symbols.\n";
            file << "Consider installing debug symbols for the target application or ensure symbols are available.\n";
        }
    }
    else
    {
        file << "No leaks detected.\n";
    }

    Log(info, "Leak report generated: %s", filename);
    
    // Also log to console if symbol resolution failed
    if(symbolResolutionFailed)
    {
        if(symbolResolutionErrorCode != 0) {
            Log(info, "Some stack frames could not be resolved (error code: %d). This may indicate missing debug symbols for the target application.", symbolResolutionErrorCode);
        } else {
            Log(info, "Some stack frames could not be resolved. This may indicate missing debug symbols for the target application.");
        }
    }

    free(const_cast<char*>(leakArgs->filename));
    free(leakArgs);

    config->bLeakReportInProgress = false;
    Trace("ReportLeaks:Exit");
    return NULL;
}

// ------------------------------------------------------------------------------------------
// WriteRestrackRaw
//
// Writes the raw stack trace (IPs only) to the specified file.
// ------------------------------------------------------------------------------------------
pthread_t WriteRestrackSnapshot(ProcDumpConfiguration* config, ECoreDumpType type)
{
    //
    // Generate the restrack filename
    //
    char* dumpFileName = GetCoreDumpName(config, type);
    std::unique_ptr<char[]> owndumpFileName(dumpFileName);
    std::string filename = (std::string(dumpFileName) + ".restrack");

    //
    // Create a thread to write the snapshot to avoid delays in the calling thread.
    // Important due to symbol resolution possibly taking a longer time.
    //
    leakThreadArgs* args = (leakThreadArgs*) malloc(sizeof(leakThreadArgs));
    args->config = config;
    args->filename = strdup(filename.c_str());
    pthread_t thread = 0;
    int ret = pthread_create(&thread, NULL, ReportLeaks, args);
    if(ret != 0)
    {
        Trace("Error creating thread to write restrack snapshot: %d", ret);
        return 0;
    }

    config->NumberOfLeakReportsCollected++;
    return thread;
}
