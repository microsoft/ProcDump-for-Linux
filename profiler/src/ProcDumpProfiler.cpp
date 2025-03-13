// Copyright (c) .NET Foundation and contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include "ProcDumpProfiler.h"
#include "corhlpr.h"
#include "CComPtr.h"
#include "profiler_pal.h"
#include <string>
#include <stdio.h>

INITIALIZE_EASYLOGGINGPP

//------------------------------------------------------------------------------------------------------------------------------------------------------
// HealthThread
//
// Periodically (default 5s) calls the procdump status pipe for a health check. If we're unable to communicate with procdump it means it has been
// terminated and we can unload.
//------------------------------------------------------------------------------------------------------------------------------------------------------
void* HealthThread(void* args)
{
    LOG(TRACE) << "HealthThread: Enter";

    CorProfiler* profiler = static_cast<CorProfiler*> (args);
    char* sockPath = NULL;

    while(true)
    {
        if(profiler->SendDumpCompletedStatus("", PROFILER_STATUS_HEALTH)==-1)
        {
            LOG(TRACE) << "HealthThread: Procdump not reachable..unloading ourselves";
            sockPath = profiler->GetSocketPath("procdump/procdump-status-", profiler->procDumpPid, getpid());
            if(sockPath)
            {
                LOG(TRACE) << "HealthThread: Unlinking the socket path " << sockPath;
                // if procdump exited abnormally, the socket file will still exist so we clean it up.
                unlink(sockPath);

                free(sockPath);
            }

            profiler->UnloadProfiler();
            break;
        }

        LOG(TRACE) << "HealthThread: Procdump running...";

        sleep(HEALTH_POLL_FREQ);        // sleep is a thread cancellation point
    }

    LOG(TRACE) << "HealthThread: Exit";
    return NULL;
}

//--------------------------------------------------------------------
//
// CorProfiler::WildcardSearch
// Search string supports '*' anywhere and any number of times
//
//--------------------------------------------------------------------
bool CorProfiler::WildcardSearch(WCHAR* szClassName, WCHAR* szSearch)
{
    if ((szClassName == NULL) || (szSearch == NULL))
        return false;

    WCHAR* szClassLowerMalloc = (WCHAR*)malloc(sizeof(WCHAR)*(wcslen(szClassName)+1));
    if (szClassLowerMalloc == NULL)
        return false;

    WCHAR* szSearchLowerMalloc = (WCHAR*)malloc(sizeof(WCHAR)*(wcslen(szSearch)+1));
    if (szSearchLowerMalloc == NULL)
    {
        free(szClassLowerMalloc);
        return false;
    }

    WCHAR* szClassLower = szClassLowerMalloc;
    wcscpy(szClassLower, (wcslen(szClassName)+1), szClassName);
    wcslwr(szClassLower, (wcslen(szClassName)+1));

    WCHAR* szSearchLower = szSearchLowerMalloc;
    wcscpy(szSearchLower, (wcslen(szSearch)+1), szSearch);
    wcslwr(szSearchLower, (wcslen(szSearch)+1));

    while ((*szSearchLower != u'\0') && (*szClassLower != u'\0'))
    {
        if (*szSearchLower != u'*')
        {
            // Straight (case insensitive) compare
            if (*szSearchLower != *szClassLower)
            {
                free(szClassLowerMalloc);
                szClassLowerMalloc = NULL;

                free(szSearchLowerMalloc);
                szSearchLowerMalloc = NULL;

                return false;
            }

            szSearchLower++;
            szClassLower++;
            continue;
        }

        //
        // Wildcard processing
        //

ContinueWildcard:
        szSearchLower++;

        // The wildcard is on the end; e.g. '*' 'blah*' or '*blah*'
        // Must be a match
        if (*szSearchLower == u'\0')
        {
            free(szClassLowerMalloc);
            szClassLowerMalloc = NULL;

            free(szSearchLowerMalloc);
            szSearchLowerMalloc = NULL;
            return true;
        }

        // Double Wildcard; e.g. '**' 'blah**' or '*blah**'
        if (*szSearchLower == u'*')
            goto ContinueWildcard;

        // Find the length of the sub-string to search for
        int endpos = 0;
        while ((szSearchLower[endpos] != u'\0') && (szSearchLower[endpos] != u'*'))
            endpos++;

        // Find a match of the sub-search string anywhere within the class string
        int cc = 0; // Offset in to the Class
        int ss = 0; // Offset in to the Sub-Search
        while (ss < endpos)
        {
            if (szClassLower[ss+cc] == u'\0')
            {
                free(szClassLowerMalloc);
                szClassLowerMalloc = NULL;

                free(szSearchLowerMalloc);
                szSearchLowerMalloc = NULL;

                return false;
            }

            if (szSearchLower[ss] != szClassLower[ss+cc])
            {
                cc++;
                ss = 0;
                continue;
            }
            ss++;
        }

        // If we get here, we found a match; move each string forward
        szSearchLower += ss;
        szClassLower += (ss + cc);
    }

    // Do we have a trailing wildcard?
    // This happens when Class = ABC.XYZ and Search = *XYZ*
    // Needed as the trailing wildcard code (above) doesn't run after the ss/cc search as Class is null
    while (*szSearchLower == u'*')
    {
        szSearchLower++;
    }

    // If Class and Search have no residual, this is a match.
    if ((*szSearchLower == u'\0') && (*szClassLower == u'\0'))
    {
        free(szClassLowerMalloc);
        szClassLowerMalloc = NULL;

        free(szSearchLowerMalloc);
        szSearchLowerMalloc = NULL;

        return true;
    }

    free(szClassLowerMalloc);
    szClassLowerMalloc = NULL;

    free(szSearchLowerMalloc);
    szSearchLowerMalloc = NULL;

    return false;
}

//------------------------------------------------------------------------------------------------------------------------------------------------------
// CorProfiler::IsHighPerfBasicGC
//------------------------------------------------------------------------------------------------------------------------------------------------------
bool CorProfiler::IsHighPerfBasicGC()
{
    if(triggerType == GCThreshold || triggerType == GCGeneration)
    {
        return true;
    }

    return false;
}

//------------------------------------------------------------------------------------------------------------------------------------------------------
// CorProfiler::CorProfiler
//------------------------------------------------------------------------------------------------------------------------------------------------------
CorProfiler::CorProfiler() :
    refCount(0), corProfilerInfo8(nullptr), corProfilerInfo3(nullptr), corProfilerInfo(nullptr),
    procDumpPid(0), currentThresholdIndex(0), gcGeneration(-1), gcGenStarted(false)
{
    // Configure logging
    el::Loggers::reconfigureAllLoggers(el::ConfigurationType::Filename, LOG_FILE);
    el::Loggers::reconfigureAllLoggers(el::ConfigurationType::MaxLogFileSize, MAX_LOG_FILE_SIZE);
    el::Loggers::reconfigureAllLoggers(el::ConfigurationType::ToStandardOutput, "false");
    el::Loggers::reconfigureAllLoggers(el::ConfigurationType::Format, "%datetime %level [%thread] [%func] [%loc] %msg");

    pthread_mutex_init(&endDumpCondition, NULL);
}

//------------------------------------------------------------------------------------------------------------------------------------------------------
// CorProfiler::~CorProfiler
//------------------------------------------------------------------------------------------------------------------------------------------------------
CorProfiler::~CorProfiler()
{
    pthread_mutex_destroy(&endDumpCondition);
}

//------------------------------------------------------------------------------------------------------------------------------------------------------
// CorProfiler::InitializeForAttach
//------------------------------------------------------------------------------------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE CorProfiler::InitializeForAttach(IUnknown *pCorProfilerInfoUnk, void *pvClientData, UINT cbClientData)
{
    LOG(TRACE) << "CorProfiler::InitializeForAttach: Enter";

    char* clientData = new char[cbClientData+1];
    if(clientData == NULL)
    {
        LOG(TRACE) << "CorProfiler::InitializeForAttach: Failed to allocate memory for clientData filter";
        return E_FAIL;
    }

    memcpy(clientData, pvClientData, cbClientData);
    clientData[cbClientData]='\0';

    if(ParseClientData(clientData)==false)
    {
        LOG(TRACE) << "CorProfiler::InitializeForAttach: Failed to parse client data";
        delete[] clientData;
        return E_FAIL;
    }

    delete[] clientData;

    processName = GetProcessName();
    if(processName.empty())
    {
        LOG(TRACE) << "CorProfiler::InitializeForAttach: Failed to get process name";
        return E_FAIL;
    }

    HRESULT queryInterfaceResult = pCorProfilerInfoUnk->QueryInterface(__uuidof(ICorProfilerInfo8), reinterpret_cast<void **>(&this->corProfilerInfo8));
    if (FAILED(queryInterfaceResult))
    {
        LOG(TRACE) << "CorProfiler::InitializeForAttach: Failed to query for ICorProfilerInfo8";
        return E_FAIL;
    }

    queryInterfaceResult = pCorProfilerInfoUnk->QueryInterface(__uuidof(ICorProfilerInfo3), reinterpret_cast<void **>(&this->corProfilerInfo3));
    if (FAILED(queryInterfaceResult))
    {
        LOG(TRACE) << "CorProfiler::InitializeForAttach: Failed to query for ICorProfilerInfo3";
        return E_FAIL;
    }

    queryInterfaceResult = pCorProfilerInfoUnk->QueryInterface(__uuidof(ICorProfilerInfo), reinterpret_cast<void **>(&this->corProfilerInfo));
    if (FAILED(queryInterfaceResult))
    {
        LOG(TRACE) << "CorProfiler::InitializeForAttach: Failed to query for ICorProfilerInfo";
        return E_FAIL;
    }

    if(triggerType == Exception)
    {
        HRESULT hr = this->corProfilerInfo8->SetEventMask(COR_PRF_MONITOR_EXCEPTIONS);
        if(FAILED(hr))
        {
            LOG(TRACE) << "CorProfiler::InitializeForAttach: Failed to set event mask: COR_PRF_MONITOR_EXCEPTIONS";
            return E_FAIL;
        }
    }
    else if(IsHighPerfBasicGC() == true)
    {
        HRESULT hr = this->corProfilerInfo8->SetEventMask2(0, COR_PRF_HIGH_BASIC_GC);
        if(FAILED(hr))
        {
            LOG(TRACE) << "CorProfiler::InitializeForAttach: Failed to set event mask: COR_PRF_HIGH_BASIC_GC";
            return E_FAIL;
        }
    }
    else
    {
        LOG(TRACE) << "CorProfiler::InitializeForAttach: Invalid trigger type found";
        return E_FAIL;
    }

    // Create the health check thread which periodically pings procdump to see if its still alive
    pthread_create(&healthThread, NULL, HealthThread, this);

    LOG(TRACE) << "CorProfiler::InitializeForAttach: Exit";
    return S_OK;
}

//------------------------------------------------------------------------------------------------------------------------------------------------------
// CorProfiler::GetUint16
//
// Primitive conversion from char* to uint16_t*.
//
//------------------------------------------------------------------------------------------------------------------------------------------------------
WCHAR* CorProfiler::GetUint16(char* buffer)
{
    LOG(TRACE) << "CorProfiler::GetUint16: Enter";

    WCHAR* dumpFileNameW = NULL;

    if(buffer!=NULL)
    {
        dumpFileNameW = static_cast<WCHAR*> (malloc((strlen(buffer)+1)*sizeof(WCHAR)));
        if(!dumpFileNameW)
        {
            return NULL;
        }

        for(int i=0; i<(strlen(buffer)+1); i++)
        {
            dumpFileNameW[i] = static_cast<WCHAR> (buffer[i]);
        }
    }

    LOG(TRACE) << "CorProfiler::GetUint16: Exit";
    return dumpFileNameW;
}

//------------------------------------------------------------------------------------------------------------------------------------------------------
// CorProfiler::ParseClientData
//
// Syntax of client data: <trigger_type><fullpathtodumplocation>;...
//
//      DOTNET_EXCEPTION_TRIGGER;<fullpathtodumplocation>;<pidofprocdump>;<exception>:<numdumps>;<exception>:<numdumps>,...
//      DOTNET_GC_THRESHOLD_TRIGGER;<fullpathtodumplocation>;<pidofprocdump>;[<generation> | 3(LOH) | 4(POH) | 2008 (total mem)];Threshold1;Threshold2,...
//      DOTNET_GC_GEN_TRIGGER;<fullpathtodumplocation>;<pidofprocdump>;GCGeneration
//
//------------------------------------------------------------------------------------------------------------------------------------------------------
bool CorProfiler::ParseClientData(char* clientData)
{
    LOG(TRACE) << "CorProfiler::ParseClientData: Enter clientData = " << clientData;
    std::stringstream clientDataStream(clientData);
    std::string segment;
    std::vector<std::string> dataList;

    while(std::getline(clientDataStream, segment, ';'))
    {
        dataList.push_back(segment);
    }

    int i=0;
    for(std::string dataItem : dataList)
    {
        if(i == 0)
        {
            //
            // First part of list is the type of trigger that is being invoked.
            //
            triggerType = static_cast<enum TriggerType>(std::stoi(dataItem));
            LOG(TRACE) << "CorProfiler::ParseClientData: trigger type = " << triggerType;
            i++;
        }
        else if(i == 1)
        {
            //
            // Second part of the list is either:
            //    > base path to dump location (if it ends with '/')
            //    > full path to dump file
            //
            fullDumpPath = dataItem;

            LOG(TRACE) << "CorProfiler::ParseClientData: Full path to dump = " << fullDumpPath;
            i++;
        }
        else if(i == 2)
        {
            //
            // Third part of the list is always the procdump pid.
            // we we need this to communicate back status to procdump
            //
            procDumpPid = std::stoi(dataItem);
            LOG(TRACE) << "CorProfiler::ParseClientData: ProcDump PID = " << procDumpPid;
            i++;
        }
        else if(triggerType == Exception)
        {
            // exception filter
            std::string segment2;
            std::stringstream stream(dataItem);
            ExceptionMonitorEntry entry;
            entry.exceptionID = NULL;
            entry.collectedDumps = 0;
            std::getline(stream, segment2, ':');

            entry.exception = segment2;
            std::getline(stream, segment2, ':');
            entry.dumpsToCollect = std::stoi(segment2);

            exceptionMonitorList.push_back(entry);
        }
        else if (triggerType == GCThreshold)
        {
            // GC threshold list
            if(i == 3)
            {
                // first element is the generation
                gcGeneration = std::stoi(dataItem);
                i++;
            }
            else
            {
                gcMemoryThresholdMonitorList.push_back(std::stoi(dataItem) << 20);
            }
        }
        else if (triggerType == GCGeneration)
        {
            // GC generation
            gcGeneration = std::stoi(dataItem);
            break;
        }
        else
        {
            LOG(TRACE) << "CorProfiler::ParseClientData Unrecognized trigger type";
            return false;
        }
    }

    for (auto & element : exceptionMonitorList)
    {
        LOG(TRACE) << "CorProfiler::ParseClientData:Exception filter " << element.exception << " with dump count set to " << std::to_string(element.dumpsToCollect);
    }

    for (auto & element : gcMemoryThresholdMonitorList)
    {
        LOG(TRACE) << "CorProfiler::ParseClientData:GCMemoryThreshold  " << element;
    }

    if(gcGeneration != -1)
    {
        if( gcGeneration == CUMULATIVE_GC_SIZE)
        {
            LOG(TRACE) << "CorProfiler::ParseClientData:GCGeneration " << "Cumulative";
        }
        else
        {
            LOG(TRACE) << "CorProfiler::ParseClientData:GCGeneration " << gcGeneration;
        }
    }

    LOG(TRACE) << "CorProfiler::ParseClientData: Exit";
    return true;
}

//------------------------------------------------------------------------------------------------------------------------------------------------------
// CorProfiler::Initialize
//------------------------------------------------------------------------------------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE CorProfiler::Initialize(IUnknown *pICorProfilerInfoUnk)
{
    return S_OK;
}

//------------------------------------------------------------------------------------------------------------------------------------------------------
// CorProfiler::Shutdown
//------------------------------------------------------------------------------------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE CorProfiler::Shutdown()
{
    LOG(TRACE) << "CorProfiler::Shutdown: Enter";
    if (this->corProfilerInfo8 != nullptr)
    {
        this->corProfilerInfo8->Release();
        this->corProfilerInfo8 = nullptr;
    }

    if (this->corProfilerInfo3 != nullptr)
    {
        this->corProfilerInfo3->Release();
        this->corProfilerInfo3 = nullptr;
    }

    if (this->corProfilerInfo != nullptr)
    {
        this->corProfilerInfo->Release();
        this->corProfilerInfo = nullptr;
    }

    CleanupProfiler();

    LOG(TRACE) << "CorProfiler::Shutdown: Exit";
    return S_OK;
}


//------------------------------------------------------------------------------------------------------------------------------------------------------
// CorProfiler::SendCatastrophicFailureStatus
// Sends a catastrophic failure  notification to procdump. Procdump should immediately stop monitoring as a result since the profiler will unload itself
//------------------------------------------------------------------------------------------------------------------------------------------------------
void CorProfiler::SendCatastrophicFailureStatus()
{
    LOG(TRACE) << "CorProfiler::SendCatastrophicFailureStatus: Enter";

    SendDumpCompletedStatus("", PROFILER_STATUS_FAILURE);
    CleanupProfiler();
    UnloadProfiler();

    LOG(TRACE) << "CorProfiler::SendCatastrophicFailureStatus: Exit";
}

//------------------------------------------------------------------------------------------------------------------------------------------------------
// CorProfiler::SendDumpCompletedStatus
// Sends a status notification to procdump. The status flag should be one of the following:
//      > If dump was succesfully written: '1'
//      > 'H' for health pings. If it fails, means procdump was terminated.
//      > Any catastrophic failure of the profiler: PROFILER_STATUS_FAILURE (procdump should immediately stop monitoring as this results in an unload of the profiler)
//------------------------------------------------------------------------------------------------------------------------------------------------------
int CorProfiler::SendDumpCompletedStatus(std::string dump, char status)
{
    LOG(TRACE) << "CorProfiler::SendDumpCompletedStatus: Enter with status '" << status << "'";

    int s, len;
    struct sockaddr_un remote;
    char* tmpFolder = NULL;

    tmpFolder = GetSocketPath("procdump/procdump-status-", procDumpPid, getpid());
    LOG(TRACE) << "CorProfiler::SendDumpCompletedStatus: Socket path: " << tmpFolder;

    if((s = socket(AF_UNIX, SOCK_STREAM, 0))==-1)
    {
        LOG(TRACE) << "CorProfiler::SendDumpCompletedStatus: Failed to create socket: " << errno;
        delete[] tmpFolder;
        return -1;
    }

    // make sure the procdump listening on sockect before try to connect
    int retryCount = 0;
    while(access(tmpFolder,F_OK)!=0)
    {
        usleep(10);
        retryCount++;
        if(retryCount == 5)
        {
            LOG(TRACE) << "CorProfiler::SendDumpCompletedStatus: Socket path " << tmpFolder << " not found";
            delete[] tmpFolder;
            close(s);
            return -1;
        }
    }

    LOG(TRACE) << "CorProfiler::SendDumpCompletedStatus: Trying to connect...";

    remote.sun_family = AF_UNIX;
    strcpy(remote.sun_path, tmpFolder);
    len = strlen(remote.sun_path) + sizeof(remote.sun_family);
    if(connect(s, (struct sockaddr *) (&remote), len)==-1)
    {
        LOG(TRACE) << "CorProfiler::SendDumpCompletedStatus: Failed to connect: " << errno;
        delete[] tmpFolder;
        close(s);
        return -1;
    }

    LOG(TRACE) << "CorProfiler::SendDumpCompletedStatus: Connected";


    // packet looks like this: <[uint]payload_len><[byte] 0=failure, 1=success><[uint] dumpfile_path_len><[char*]Dumpfile path>
    uint totalPayloadLen = sizeof(uint) + sizeof(char) + sizeof(uint) + dump.length();
    uint payLoadLen = sizeof(char) + sizeof(uint) + dump.length();
    char* payload = new char[totalPayloadLen];
    if(payload==NULL)
    {
        LOG(TRACE) << "CorProfiler::SendDumpCompletedStatus: Failed memory allocation for payload";
        delete[] tmpFolder;
        close(s);
        return -1;
    }

    char* current = payload;
    memcpy(current, &payLoadLen, sizeof(uint));
    current+=sizeof(uint);

    memcpy(current, &status, sizeof(char));
    current+=sizeof(char);

    uint pathLen = dump.length();
    memcpy(current, &pathLen, sizeof(uint));
    current+=sizeof(uint);

    memcpy(current, dump.c_str(), strlen(dump.c_str()));
    current+=strlen(dump.c_str());

    if(send_all(s, payload, totalPayloadLen)==-1)
    {
        LOG(TRACE) << "CorProfiler::SendDumpCompletedStatus: Failed to send completion status";
        close(s);
        delete[] payload;
        delete[] tmpFolder;
        return -1;
    }

    delete[] payload;
    delete[] tmpFolder;

    LOG(TRACE) << "CorProfiler::SendDumpCompletedStatus: Exit";
    return 0;
}

//--------------------------------------------------------------------
//
// CorProfiler::GetProcessName - Get current process name
//
//--------------------------------------------------------------------
std::string CorProfiler::GetProcessName()
{
    LOG(TRACE) << "CorProfiler::GetProcessName: Enter";

    char fileBuffer[PATH_MAX+1] = {0};
    int charactersRead = 0;
    int	itr = 0;
    char* stringItr = NULL;
    char* processName = NULL;
    FILE* procFile = NULL;

    procFile = fopen("/proc/self/cmdline", "r");
    if(procFile != NULL)
    {
        if(fgets(fileBuffer, PATH_MAX, procFile) == NULL)
        {
            LOG(TRACE) << "CorProfiler::GetProcessName: Failed to get /proc/self/cmdline contents";
            fclose(procFile);
            return NULL;
        }
    }
    else
    {
        LOG(TRACE) << "CorProfiler::GetProcessName: Failed to open /proc/self/cmdline";
        return NULL;
    }

    fclose(procFile);

	// Extract process name
    stringItr = fileBuffer;
    charactersRead  = strlen(fileBuffer);
    for(int i = 0; i <= charactersRead; i++)
    {
        if(fileBuffer[i] == '\0')
        {
            itr = i - itr;

            if(strcmp(stringItr, "sudo") != 0)
            {
                processName = strrchr(stringItr, '/');

                if(processName != NULL)
                {
                    return std::string(strdup(processName + 1));
                }
                else
                {
                    return std::string(strdup(stringItr));
                }
            }
            else
            {
                stringItr += (itr+1); 	// +1 to move past '\0'
            }
        }
    }

    LOG(TRACE) << "CorProfiler::GetProcessName: Exit";
    return NULL;
}


//------------------------------------------------------------------------------------------------------------------------------------------------------
// CorProfiler::GetDumpName
//------------------------------------------------------------------------------------------------------------------------------------------------------
std::string CorProfiler::GetDumpName(u_int16_t dumpCount, std::string name)
{
    LOG(TRACE) << "CorProfiler::GetDumpName: Enter";
    std::ostringstream tmp;

    //
    // If the path ends in '/' it means we have a base path and need to create the full path according to:
    //  <base_path>/<process_name>_<dumpcount>_<name>_<date_time_stamp>
    //
    if(fullDumpPath[fullDumpPath.length()-1] == '/')
    {
        time_t rawTime = {0};
        struct tm* timerInfo = NULL;
        char date[DATE_LENGTH];

        LOG(TRACE) << "CorProfiler::GetDumpName: Base path specified.";

        // get time for current dump generated
        rawTime = time(NULL);
        if((timerInfo = localtime(&rawTime)) == NULL)
        {
            LOG(TRACE) << "CorProfiler::GetDumpName: Failed to get localtime.";
            return "";
        }
        strftime(date, 26, "%y%m%d_%H%M%S", timerInfo);
        LOG(TRACE) << "CorProfiler::GetDumpName: Date/time " << date;

        tmp << fullDumpPath << processName.c_str() << "_" << dumpCount << "_" << name << "_" << date;
        LOG(TRACE) << "CorProfiler::GetDumpName: Full path name " << tmp.str();
    }
    else
    {
        //
        // We have a full path...append the dump count to the very end (_X)
        //
        LOG(TRACE) << "CorProfiler::GetDumpName: Full path specified.";

        tmp << fullDumpPath << "_" << dumpCount;
        LOG(TRACE) << "CorProfiler::GetDumpName: Full path name " << tmp.str();
    }

    LOG(TRACE) << "CorProfiler::GetDumpName: Exit";
    return tmp.str();
}

//------------------------------------------------------------------------------------------------------------------------------------------------------
// CorProfiler::GetGCHeapSize
//------------------------------------------------------------------------------------------------------------------------------------------------------
uint64_t CorProfiler::GetGCHeapSize(int generation)
{
    LOG(TRACE) << "CorProfiler::GetGCHeapSize: Enter";
    uint64_t gcHeapSize = 0;

    ULONG nObjectRanges = 0;
    bool fHeapAlloc = false;
    COR_PRF_GC_GENERATION_RANGE* pObjectRanges = NULL;
    const ULONG cRanges = 32;
    COR_PRF_GC_GENERATION_RANGE objectRangesStackBuffer[cRanges];

    HRESULT hr = corProfilerInfo8->GetGenerationBounds(cRanges, &nObjectRanges, objectRangesStackBuffer);
    if (FAILED(hr))
    {
        LOG(TRACE) << "CorProfiler::GetGCHeapSize: Failed calling GetGenerationBounds " << hr;
        return 0;
    }

    if (nObjectRanges <= cRanges)
    {
        pObjectRanges = objectRangesStackBuffer;
    }

    if (pObjectRanges == NULL)
    {
        pObjectRanges = new COR_PRF_GC_GENERATION_RANGE[nObjectRanges];
        if (pObjectRanges == NULL)
        {
            LOG(TRACE) << "CorProfiler::GetGCHeapSize: Failed to allocate memory for object ranges ";
            return 0;
        }

        fHeapAlloc = true;

        ULONG nObjectRanges2 = 0;
        HRESULT hr = corProfilerInfo8->GetGenerationBounds(nObjectRanges, &nObjectRanges2, pObjectRanges);
        if (FAILED(hr) || nObjectRanges != nObjectRanges2)
        {
            LOG(TRACE) << "CorProfiler::GetGCHeapSize: Failed to call GetGenerationBounds with allocated memory";
            delete[] pObjectRanges;
            return 0;
        }
    }

    for (int i = nObjectRanges - 1; i >= 0; i--)
    {
        // Uncomment this to help track down .NET memory usage while debugging.
        //LOG(TRACE) << "Range Len: " << pObjectRanges[i].rangeLength << " Gen: " << pObjectRanges[i].generation;

        if(generation == CUMULATIVE_GC_SIZE)
        {
            gcHeapSize += pObjectRanges[i].rangeLength;
        }
        else if(pObjectRanges[i].generation == generation)
        {
            gcHeapSize += pObjectRanges[i].rangeLength;
        }
    }

    if(fHeapAlloc == true)
    {
        delete[] pObjectRanges;
    }

    LOG(TRACE) << "CorProfiler::GetGCHeapSize: Exit";
    return gcHeapSize;
}

//------------------------------------------------------------------------------------------------------------------------------------------------------
// CorProfiler::WriteDumpHelper
//------------------------------------------------------------------------------------------------------------------------------------------------------
bool CorProfiler::WriteDumpHelper(std::string dumpName)
{
    LOG(TRACE) << "CorProfiler::WriteDumpHelper: Enter";
    AutoMutex lock = AutoMutex(&endDumpCondition);

    char* socketName = NULL;
    if(IsCoreClrProcess(getpid(), &socketName))
    {
        LOG(TRACE) << "CorProfiler::WriteDumpHelper: Target is .NET process";
        bool res = GenerateCoreClrDump(socketName, const_cast<char*> (dumpName.c_str()));
        if(res == false)
        {
            LOG(TRACE) << "CorProfiler::WriteDumpHelper: Failed to generate core dump";
            delete[] socketName;
            return false;
        }

        // Notify procdump that a dump was generated
        if(SendDumpCompletedStatus(dumpName, PROFILER_STATUS_SUCCESS) == -1)
        {
            LOG(TRACE) << "CorProfiler::WriteDumpHelper: Failed to notify procdump about dump creation";
            delete[] socketName;
            return false;
        }

        LOG(TRACE) << "CorProfiler::WriteDumpHelper: Generating core dump result: " << res;

        delete[] socketName;
    }
    else
    {
        LOG(TRACE) << "CorProfiler::WriteDumpHelper: Target process is not a .NET process";
        return false;
    }

    LOG(TRACE) << "CorProfiler::WriteDumpHelper: Exit";
    return true;
}

//------------------------------------------------------------------------------------------------------------------------------------------------------
// CorProfiler::GarbageCollectionStarted
//------------------------------------------------------------------------------------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE CorProfiler::GarbageCollectionStarted(int cGenerations, BOOL generationCollected[], COR_PRF_GC_REASON reason)
{
    LOG(TRACE) << "CorProfiler::GarbageCollectionStarted: Enter";

    if(gcGeneration != -1 &&
       gcGenStarted == false &&
       gcGeneration == CUMULATIVE_GC_SIZE ||
       (gcGeneration < cGenerations &&
       generationCollected[gcGeneration] == true))
    {
        gcGenStarted = true;

        if(gcMemoryThresholdMonitorList.size() == 0)
        {
            // GC Generation dump
            LOG(TRACE) << "CorProfiler::GarbageCollectionStarted: Dump on generation: " << gcGeneration << " and cGenerations = " << cGenerations;
            std::string dump = GetDumpName(1, convertString<std::string,std::wstring>(L"gc_gen"));
            if(WriteDumpHelper(dump) == false)
            {
                SendCatastrophicFailureStatus();
                return E_FAIL;
            }
        }
    }
    else
    {
        LOG(TRACE) << "CorProfiler::GarbageCollectionStarted: Trigger = " << triggerType << " cGenerations " << cGenerations << " gcGeneration " << gcGeneration << " threshold size " << gcMemoryThresholdMonitorList.size();
    }

    LOG(TRACE) << "CorProfiler::GarbageCollectionStarted: Exit";
    return S_OK;
}

//------------------------------------------------------------------------------------------------------------------------------------------------------
// CorProfiler::GarbageCollectionFinished
//------------------------------------------------------------------------------------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE CorProfiler::GarbageCollectionFinished()
{
    LOG(TRACE) << "CorProfiler::GarbageCollectionFinished: Enter";

    if(gcGenStarted == true && gcMemoryThresholdMonitorList.size() > 0)
    {
        // During a GC threshold trigger, we only want to check heap sizes and thresholds after the gen collection
        uint64_t heapSize = 0;
        gcGenStarted = false;

        if(gcGeneration == CUMULATIVE_GC_SIZE)
        {
            // If a generation was not explicitly specified on the command line (for example: -gcm 10,20,30) we want to get _all_ the memory of
            // the managed heap and hence we add up all generations, LOH and POH.
            heapSize += GetGCHeapSize(CUMULATIVE_GC_SIZE);
            LOG(TRACE) << "CorProfiler::GarbageCollectionFinished: Cumulative heap size " << heapSize;
        }
        else
        {
            heapSize = GetGCHeapSize(gcGeneration);
            LOG(TRACE) << "CorProfiler::GarbageCollectionFinished: Generation  " << gcGeneration << " heap size " << heapSize;
        }

        if(currentThresholdIndex < gcMemoryThresholdMonitorList.size() && heapSize >= gcMemoryThresholdMonitorList[currentThresholdIndex])
        {
            LOG(TRACE) << "CorProfiler::GarbageCollectionFinished: Current threshold value " << gcMemoryThresholdMonitorList[currentThresholdIndex];

            std::string dump = GetDumpName(currentThresholdIndex + 1,convertString<std::string,std::wstring>(L"gc_size"));

            // Generate dump
            if(WriteDumpHelper(dump) == false)
            {
                SendCatastrophicFailureStatus();
                return E_FAIL;
            }
            else
            {
                currentThresholdIndex++;
            }

            if(currentThresholdIndex >= gcMemoryThresholdMonitorList.size())
            {
                // Stop monitoring
                LOG(TRACE) << "CorProfiler::GarbageCollectionFinished: Reached last threshold ";
                CleanupProfiler();
                UnloadProfiler();
            }
        }
    }
    else if(gcGenStarted == true && gcMemoryThresholdMonitorList.size() == 0)
    {
        // GC Generation dump
        LOG(TRACE) << "CorProfiler::GarbageCollectionFinished: Dump on generation: " << gcGeneration;
        gcGenStarted = false;

        std::string dump = GetDumpName(2, convertString<std::string,std::wstring>(L"gc_gen"));
        if(WriteDumpHelper(dump) == false)
        {
            SendCatastrophicFailureStatus();
            return E_FAIL;
        }

        // Stop monitoring
        CleanupProfiler();
        UnloadProfiler();
    }

    LOG(TRACE) << "CorProfiler::GarbageCollectionFinished: Exit";
    return S_OK;
}

//------------------------------------------------------------------------------------------------------------------------------------------------------
// CorProfiler::ExceptionThrown
//------------------------------------------------------------------------------------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE CorProfiler::ExceptionThrown(ObjectID thrownObjectId)
{
    LOG(TRACE) << "CorProfiler::ExceptionThrown: Enter";

    String exceptionNameAndMsg;
    String exceptionName = GetExceptionName(thrownObjectId);
    if(exceptionName.Length() == 0)
    {
        LOG(TRACE) << "CorProfiler::ExceptionThrown: Unable to get the name of the exception.";
        return E_FAIL;
    }

    String exceptionMsg = GetExceptionMessage(thrownObjectId);

    if(exceptionMsg.Length() == 0)
    {
        exceptionNameAndMsg += exceptionName;
        LOG(TRACE) << "CorProfiler::ExceptionThrown: exception name: " << exceptionNameAndMsg.ToCStr();
    }
    else
    {
        exceptionNameAndMsg += exceptionName;
        exceptionNameAndMsg += WCHAR(": ");
        exceptionNameAndMsg += exceptionMsg;
        LOG(TRACE) << "CorProfiler::ExceptionThrown: exception name: " << exceptionNameAndMsg.ToCStr();
    }

    WCHAR* exceptionWCHARs = getWCHARs(exceptionNameAndMsg);

    // Check to see if we have any matches on exceptions
    for (auto & element : exceptionMonitorList)
    {
        WCHAR* exception = GetUint16(const_cast<char*> (element.exception.c_str()));
        if(exception == NULL)
        {
            LOG(TRACE) << "CorProfiler::ExceptionThrown: Unable to get exception name (WHCAR).";
            SendCatastrophicFailureStatus();
            return E_FAIL;
        }

        if(WildcardSearch(exceptionWCHARs,exception) && element.exceptionID != thrownObjectId)
        {
            if(element.collectedDumps == element.dumpsToCollect)
            {
                LOG(TRACE) << "CorProfiler::ExceptionThrown: Dump count has been reached...exiting early";
                free(exception);
                return S_OK;
            }

            LOG(TRACE) << "CorProfiler::ExceptionThrown: Starting dump generation for exception " << exceptionName.ToCStr() << " with dump count set to " << std::to_string(element.dumpsToCollect);

            std::string dump = GetDumpName(element.collectedDumps,convertString<std::string,std::wstring>(exceptionName.ToWString()));

            if(WriteDumpHelper(dump) == false)
            {
                SendCatastrophicFailureStatus();
                free(exception);
                return E_FAIL;
            }

            //
            // In asp.net, an exception thrown in the app is caught by asp.net and then rethrown and caught again. To avoid
            // generating multiple dumps for the same exception instance we store the objectID. If we have already generated
            // a dump for that object ID we simply ignore it
            //
            element.exceptionID = thrownObjectId;

            element.collectedDumps++;
            if(element.collectedDumps == element.dumpsToCollect)
            {
                // We're done collecting dumps...
                LOG(TRACE) << "CorProfiler::ExceptionThrown: Dump count has been reached...cleaning up and unloading";
                CleanupProfiler();
                UnloadProfiler();
            }
        }
        free(exception);
    }
    free(exceptionWCHARs);

    LOG(TRACE) << "CorProfiler::ExceptionThrown: Exit";
    return S_OK;
}

//------------------------------------------------------------------------------------------------------------------------------------------------------
// CorProfiler::CleanupProfiler
//------------------------------------------------------------------------------------------------------------------------------------------------------
void CorProfiler::CleanupProfiler()
{
    LOG(TRACE) << "CorProfiler::CleanupProfiler: Enter";
    pthread_cancel(healthThread);
    pthread_join(healthThread, NULL);
    LOG(TRACE) << "CorProfiler::CleanupProfiler: Exit";
}

//------------------------------------------------------------------------------------------------------------------------------------------------------
// CorProfiler::UnloadProfiler
//------------------------------------------------------------------------------------------------------------------------------------------------------
void CorProfiler::UnloadProfiler()
{
    LOG(TRACE) << "CorProfiler::UnloadProfiler: Enter";

    corProfilerInfo3->RequestProfilerDetach(DETACH_TIMEOUT);

    LOG(TRACE) << "CorProfiler::UnloadProfiler: Exit";
}

//------------------------------------------------------------------------------------------------------------------------------------------------------
// CorProfiler::GetExceptionName
//------------------------------------------------------------------------------------------------------------------------------------------------------
String CorProfiler::GetExceptionName(ObjectID objectId)
{
    LOG(TRACE) << "CorProfiler::GetExceptionName: Enter with objectId =" << objectId;

    String name;
    ClassID classId;
    ModuleID moduleId;
    mdTypeDef typeDefToken;
    IMetaDataImport* metadata;
    ULONG read = 0;

    HRESULT hRes = corProfilerInfo->GetClassFromObject(objectId, &classId);
    if(FAILED(hRes))
    {
        LOG(TRACE) << "CorProfiler::GetExceptionName: Failed in call to GetClassFromObject " << hRes;
        return WCHAR("");
    }

    hRes = corProfilerInfo->GetClassIDInfo(classId, &moduleId, &typeDefToken);
    if(FAILED(hRes))
    {
        LOG(TRACE) << "CorProfiler::GetExceptionName: Failed in call to GetClassIDInfo " << hRes;
        return WCHAR("");
    }

    hRes = corProfilerInfo->GetModuleMetaData(moduleId, ofRead, IID_IMetaDataImport, (IUnknown**) (&metadata));
    if(FAILED(hRes))
    {
        LOG(TRACE) << "CorProfiler::GetExceptionName: Failed in call to GetModuleMetaData " << hRes;
        return WCHAR("");
    }

    WCHAR funcName[1024];
    hRes = metadata->GetTypeDefProps(typeDefToken, funcName, 1024, &read, NULL, NULL);
    if(FAILED(hRes))
    {
        LOG(TRACE) << "CorProfiler::GetExceptionName: Failed in call to GetTypeDefProps " << hRes;
        metadata->Release();
        return WCHAR("");
    }

    name+=funcName;

    metadata->Release();

    LOG(TRACE) << "CorProfiler::GetExceptionName: Exit";
    return name;
}

//------------------------------------------------------------------------------------------------------------------------------------------------------
// CorProfiler::GetExceptionMesssage
//------------------------------------------------------------------------------------------------------------------------------------------------------
String CorProfiler::GetExceptionMessage(ObjectID objectId)
{
    LOG(TRACE) << "CorProfiler::GetExceptionMessage: Enter with objectId =" << objectId;

    ClassID classId;
    ClassID parentClassId = NULL;
    ClassID systemExceptionClassId = NULL;
    ModuleID moduleId;
    mdTypeDef typeDefToken;
    IMetaDataImport* metadata = NULL;
    ULONG read = 0;
    WCHAR exceptionName[256];
    ULONG fieldCount;
    COR_FIELD_OFFSET* pFieldOffsets = NULL;
    WCHAR fieldName[256];
    ULONG stringLengthOffset;
    ULONG stringBufferOffset;
    LONG_PTR msgField;
    INT32 stringLength;
    WCHAR * stringBuffer = NULL;
    String msg;

    HRESULT hRes = corProfilerInfo8->GetClassFromObject(objectId, &classId);
    if(FAILED(hRes))
    {
        LOG(TRACE) << "CorProfiler::GetExceptionMessage: Failed in call to GetClassFromObject " << hRes;
        return WCHAR("");
    }

    //
    // Loop to find the classId for "System.Exception"
    //
    do
    {
        hRes = corProfilerInfo8->GetClassIDInfo2(classId, &moduleId, &typeDefToken, &parentClassId, 0, nullptr, nullptr);
        if(FAILED(hRes))
        {
            LOG(TRACE) << "CorProfiler::GetExceptionMessage: Failed in call to GetClassIDInfo2 " << hRes;
            return WCHAR("");
        }

        hRes = corProfilerInfo8->GetModuleMetaData(moduleId, ofRead, IID_IMetaDataImport, (IUnknown**) (&metadata));
        if(FAILED(hRes))
        {
            LOG(TRACE) << "CorProfiler::GetExceptionMessage: Failed in call to GetModuleMetaData " << hRes;
            return WCHAR("");
        }

        hRes = metadata->GetTypeDefProps(typeDefToken, exceptionName, 256, &read, NULL, NULL);
        if(FAILED(hRes))
        {
            LOG(TRACE) << "CorProfiler::GetExceptionMessage: Failed in call to GetTypeDefProps " << hRes;
            metadata->Release();
            return WCHAR("");
        }

        if(wcscmp(u"System.Exception",exceptionName)==0)
        {
            systemExceptionClassId = classId;
            break;
        }
        else
        {
            classId = parentClassId;
            metadata->Release();
            metadata = NULL;
        }
    }while(parentClassId != NULL);

    if(systemExceptionClassId != NULL)
    {
        hRes = corProfilerInfo8->GetClassLayout(systemExceptionClassId, NULL, 0, &fieldCount, NULL);
        if(FAILED(hRes))
        {
            LOG(TRACE) << "CorProfiler::GetExceptionMessage: Failed in call to GetClassLayout " << hRes;
            if(metadata != NULL) metadata->Release();
            return WCHAR("");
        }
        if(fieldCount == 0)
        {
            LOG(TRACE) << "CorProfiler::GetExceptionMessage: GetClassLayout returned zero fieldCount";
            if(metadata != NULL) metadata->Release();
            return WCHAR("");
        }

        pFieldOffsets = new COR_FIELD_OFFSET[fieldCount];
        if(pFieldOffsets==NULL)
        {
            LOG(TRACE) << "CorProfiler::GetExceptionMessage: Failed memory allocation for pFieldOffsets";
            if(metadata != NULL) metadata->Release();
            return WCHAR("");;
        }

        hRes = corProfilerInfo8->GetClassLayout(systemExceptionClassId, pFieldOffsets, fieldCount, &fieldCount, NULL);
        if(FAILED(hRes))
        {
            LOG(TRACE) << "CorProfiler::GetExceptionMessage: Failed in call to GetClassLayout " << hRes;
            if(metadata != NULL) metadata->Release();
            delete [] pFieldOffsets;
            return WCHAR("");
        }

        //
        // Loop to locate the "_message" field
        // Get stringLength from the "_message" field stringLengthOffset
        // Copy the number of (stringLength+1) WCHARs from the "_message" field stringBufferOffset to our stringBuffer
        //
        for (ULONG fieldIndex = 0; fieldIndex < fieldCount; fieldIndex++)
        {
            hRes = metadata->GetFieldProps(pFieldOffsets[fieldIndex].ridOfField, NULL, fieldName, 256, NULL, NULL,
                                    NULL, NULL, NULL, NULL, NULL);
            if(FAILED(hRes))
            {
                LOG(TRACE) << "CorProfiler::GetExceptionMessage: Failed in call to GetFieldProps " << hRes;
                if(metadata != NULL) metadata->Release();
                delete [] pFieldOffsets;
                return WCHAR("");
            }

            if(wcscmp(u"_message",fieldName)==0)
            {
                hRes = corProfilerInfo8->GetStringLayout2(&stringLengthOffset, &stringBufferOffset);
                if(FAILED(hRes))
                {
                    LOG(TRACE) << "CorProfiler::GetExceptionMessage: Failed in call to GetStringLayout2 " << hRes;
                    if(metadata != NULL) metadata->Release();
                    delete [] pFieldOffsets;
                    return WCHAR("");
                }

                msgField = *(PLONG_PTR)(((BYTE *)objectId) + pFieldOffsets[fieldIndex].ulOffset);
                if(msgField != NULL)
                {
                    stringLength = *(PINT32)((BYTE *)msgField + stringLengthOffset);
                    stringBuffer = new WCHAR[stringLength+1];
                    if(stringBuffer==NULL)
                    {
                        LOG(TRACE) << "CorProfiler::GetExceptionMessage: Failed memory allocation for stringBuffer";
                        if(metadata != NULL) metadata->Release();
                        delete [] pFieldOffsets;
                        return WCHAR("");;
                    }

                    memcpy(stringBuffer, (BYTE *)msgField+stringBufferOffset,(stringLength+1)*sizeof(WCHAR));
                    msg+=stringBuffer;
                }

                break;
            }
        }
    }

    if(metadata != NULL) metadata->Release();
    if(pFieldOffsets != NULL) delete [] pFieldOffsets;
    if(stringBuffer != NULL) delete stringBuffer;

    LOG(TRACE) << "CorProfiler::GetExceptionMessage: Exit";
    return msg;
}

//------------------------------------------------------------------------------------------------------------------------------------------------------
// IsCoreClrProcess
//------------------------------------------------------------------------------------------------------------------------------------------------------
bool CorProfiler::IsCoreClrProcess(pid_t pid, char** socketName)
{
    LOG(TRACE) << "CorProfiler::IsCoreClrProcess: Enter";

    bool bRet = false;
    *socketName = NULL;
    FILE *procFile = NULL;
    char lineBuf[4096];
    char* tmpFolder = NULL;

    // If $TMPDIR is set, use it as the path, otherwise we use /tmp
    // per https://github.com/dotnet/diagnostics/blob/master/documentation/design-docs/ipc-protocol.md
    tmpFolder = GetSocketPath("dotnet-diagnostic-", pid, 0);

    // Enumerate all open domain sockets exposed from the process. If one
    // exists by the following prefix, we assume its a .NET process:
    //    dotnet-diagnostic-{%d:PID}
    // The sockets are found in /proc/net/unix
    if(tmpFolder!=NULL)
    {
        procFile = fopen("/proc/net/unix", "r");
        if(procFile != NULL)
        {
            fgets(lineBuf, sizeof(lineBuf), procFile); // Skip first line with column headers.

            while(fgets(lineBuf, 4096, procFile) != NULL)
            {
                char* ptr = GetPath(lineBuf);
                if(ptr!=NULL)
                {
                    if(strncmp(ptr, tmpFolder, strlen(tmpFolder)) == 0)
                    {
                        // Found the correct socket
                        *socketName = new char[(sizeof(char)*strlen(ptr)+1)];
                        if(*socketName!=NULL)
                        {
                            memset(*socketName, 0, sizeof(char)*strlen(ptr)+1);
                            if(strncpy(*socketName, ptr, sizeof(char)*strlen(ptr)+1)!=NULL)
                            {
                                LOG(TRACE) << "CorProfiler::IsCoreClrProcess: CoreCLR diagnostics socket: " << (*socketName);
                                bRet = true;
                            }
                            break;
                        }
                    }
                }
            }

            fclose(procFile);
        }
        else
        {
            LOG(TRACE) << "CorProfiler::IsCoreClrProcess: Failed to open /proc/net/unix: " << errno;
        }
    }

    if(tmpFolder)
    {
        delete[] tmpFolder;
    }

    if(*socketName!=NULL && bRet==false)
    {
        delete[] *socketName;
        *socketName = NULL;
    }

    LOG(TRACE) << "CorProfiler::IsCoreClrProcess: Exit";
    return bRet;
}

//------------------------------------------------------------------------------------------------------------------------------------------------------
// CorProfiler::send_all
//
// Helper that waits until all data has been sent.
//------------------------------------------------------------------------------------------------------------------------------------------------------
int CorProfiler::send_all(int socket, void* buffer, size_t length)
{
    char *ptr = (char*) buffer;
    while (length > 0)
    {
        int i = send(socket, ptr, length, 0);
        if (i < 1)
        {
            return -1;
        }

        ptr += i;
        length -= i;
    }

    return 0;
}

//------------------------------------------------------------------------------------------------------------------------------------------------------
// CorProfiler::recv_all
//
// Helper that waits until all data has been read.
//------------------------------------------------------------------------------------------------------------------------------------------------------
int CorProfiler::recv_all(int socket, void* buffer, size_t length)
{
    char *ptr = (char*) buffer;
    while (length > 0)
    {
        int i = recv(socket, ptr, length, 0);
        if (i < 1)
        {
            return -1;
        }

        ptr += i;
        length -= i;
    }

    return 0;
}

//------------------------------------------------------------------------------------------------------------------------------------------------------
// CorProfiler::GenerateCoreClrDump
//------------------------------------------------------------------------------------------------------------------------------------------------------
bool CorProfiler::GenerateCoreClrDump(char* socketName, char* dumpFileName)
{
    LOG(TRACE) << "CorProfiler::GenerateCoreClrDump: Enter";

    bool bRet = false;
    struct sockaddr_un addr = {0};
    int fd = 0;
    WCHAR* dumpFileNameW = NULL;
    char* temp_buffer = NULL;

    if( (dumpFileNameW = GetUint16(dumpFileName))!=NULL)
    {
        if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
        {
            LOG(TRACE) << "CorProfiler::GenerateCoreClrDump: Failed to create socket for .NET dump generation.";
        }
        else
        {
            LOG(TRACE) << "CorProfiler::GenerateCoreClrDump: Success creating socket for .NET dump generation.";

            // Create socket to diagnostics server
            memset(&addr, 0, sizeof(struct sockaddr_un));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, socketName, sizeof(addr.sun_path)-1);

            if (connect(fd, (struct sockaddr*) (&addr), sizeof(struct sockaddr_un)) == -1)
            {
                LOG(TRACE) << "CorProfiler::GenerateCoreClrDump: Failed to connect to socket for .NET dump generation.";
            }
            else
            {
                LOG(TRACE) << "CorProfiler::GenerateCoreClrDump: Success connecting to socket for .NET dump generation.";

                unsigned int dumpFileNameLen = ((strlen(dumpFileName)+1));
                int payloadSize = sizeof(dumpFileNameLen);
                payloadSize += dumpFileNameLen*sizeof(wchar_t);
                unsigned int dumpType = CORECLR_DUMPTYPE_FULL;
                payloadSize += sizeof(dumpType);
                unsigned int diagnostics = CORECLR_DUMPLOGGING_OFF;
                payloadSize += sizeof(diagnostics);

                uint16_t totalPacketSize = sizeof(struct IpcHeader)+payloadSize;

                // First initialize header
                temp_buffer = new char[totalPacketSize];
                if(temp_buffer!=NULL)
                {
                    memset(temp_buffer, 0, totalPacketSize);
                    struct IpcHeader dumpHeader =
                    {
                        { {"DOTNET_IPC_V1"} },
                        (uint16_t)totalPacketSize,
                        (uint8_t)0x01,
                        (uint8_t)0x01,
                        (uint16_t)0x0000
                    };

                    char* temp_buffer_cur = temp_buffer;

                    memcpy(temp_buffer_cur, &dumpHeader, sizeof(struct IpcHeader));
                    temp_buffer_cur += sizeof(struct IpcHeader);

                    // Now we add the payload
                    memcpy(temp_buffer_cur, &dumpFileNameLen, sizeof(dumpFileNameLen));
                    temp_buffer_cur += sizeof(dumpFileNameLen);

                    memcpy(temp_buffer_cur, dumpFileNameW, dumpFileNameLen*sizeof(uint16_t));
                    temp_buffer_cur += dumpFileNameLen*sizeof(uint16_t);

                    // next, the dumpType
                    memcpy(temp_buffer_cur, &dumpType, sizeof(unsigned int));
                    temp_buffer_cur += sizeof(unsigned int);

                    // next, the diagnostics flag
                    memcpy(temp_buffer_cur, &diagnostics, sizeof(unsigned int));

                    if(send_all(fd, temp_buffer, totalPacketSize)==-1)
                    {
                        LOG(TRACE) << "CorProfiler::GenerateCoreClrDump: Failed sending packet to diagnostics server: " << errno;
                    }
                    else
                    {
                        LOG(TRACE) << "CorProfiler::GenerateCoreClrDump: Success sending packet to diagnostics server: ";

                        // Lets get the header first
                        struct IpcHeader retHeader;
                        if(recv_all(fd, &retHeader, sizeof(struct IpcHeader))==-1)
                        {
                            LOG(TRACE) << "CorProfiler::GenerateCoreClrDump: Failed receiving response header from diagnostics server: " << errno;
                        }
                        else
                        {
                            LOG(TRACE) << "CorProfiler::GenerateCoreClrDump: Success receiving response header from diagnostics server";
                            // Check the header to make sure its the right size
                            if(retHeader.Size != CORECLR_DIAG_IPCHEADER_SIZE)
                            {
                                LOG(TRACE) << "CorProfiler::GenerateCoreClrDump: Failed validating header size in response header from diagnostics server " << retHeader.Size << "!= 24]";
                            }
                            else
                            {
                                LOG(TRACE) << "CorProfiler::GenerateCoreClrDump: Success validating header size in response header from diagnostics server ";
                                // Next, get the payload which contains a single uint32 (hresult)
                                int32_t res = -1;
                                if(recv_all(fd, &res, sizeof(int32_t))==-1)
                                {
                                     LOG(TRACE) << "CorProfiler::GenerateCoreClrDump: Failed receiving result code from response payload from diagnostics server: " << errno;
                                }
                                else
                                {
                                     LOG(TRACE) << "CorProfiler::GenerateCoreClrDump: Success receiving result code from response payload from diagnostics server: " << errno;

                                    if(res==0)
                                    {
                                        LOG(TRACE) << "CorProfiler::GenerateCoreClrDump: Core dump generation success ";
                                        bRet = true;
                                    }
                                    else
                                    {
                                        LOG(TRACE) << "CorProfiler::GenerateCoreClrDump: Response error: " << res;
                                    }
                                }
                            }
                        }
                    }

                    delete[] temp_buffer;
                }

                close(fd);
            }
        }

        free(dumpFileNameW);
    }

    LOG(TRACE) << "CorProfiler::GenerateCoreClrDump: Exit";
    return bRet;
}

//------------------------------------------------------------------------------------------------------------------------------------------------------
// CorProfiler::GenerateCoreClrDump
//------------------------------------------------------------------------------------------------------------------------------------------------------
char* CorProfiler::GetSocketPath(char* prefix, pid_t pid, pid_t targetPid)
{
    LOG(TRACE) << "CorProfiler::GetSocketPath: Enter";

    char* prefixTmpFolder = NULL;
    char* t = NULL;

    // If $TMPDIR is set, use it as the path, otherwise we use /tmp
    prefixTmpFolder = getenv("TMPDIR");
    if(prefixTmpFolder==NULL)
    {
        prefixTmpFolder = "/tmp";
    }

    if(targetPid)
    {
        int len = snprintf(NULL, 0, "%s/%s%d-%d", prefixTmpFolder, prefix, pid, targetPid);
        t = new char[len+1];
        if(t==NULL)
        {
            return NULL;
        }
        snprintf(t, len+1, "%s/%s%d-%d", prefixTmpFolder, prefix, pid, targetPid);
    }
    else
    {
        int len = snprintf(NULL, 0, "%s/%s%d", prefixTmpFolder, prefix, pid);
        t = new char[len+1];
        if(t==NULL)
        {
            return NULL;
        }
        snprintf(t, len+1, "%s/%s%d", prefixTmpFolder, prefix, pid);
    }

    LOG(TRACE) << "CorProfiler::GetSocketPath: Exit";
    return t;
}

//------------------------------------------------------------------------------------------------------------------------------------------------------
// CorProfiler::GetPath
//------------------------------------------------------------------------------------------------------------------------------------------------------
char* CorProfiler::GetPath(char* lineBuf)
{
    char delim[] = " ";

    // example of /proc/net/unix line:
    // 0000000000000000: 00000003 00000000 00000000 0001 03 20287 @/tmp/.X11-unix/X0
    char *ptr = strtok(lineBuf, delim);

    // Move to last column which contains the name of the file (/socket)
    for(int i=0; i<7; i++)
    {
        ptr = strtok(NULL, delim);
    }

    if(ptr!=NULL)
    {
        ptr[strlen(ptr)-1]='\0';
    }

    return ptr;
}


// ========================================================================================================================
// ========================================================================================================================

HRESULT STDMETHODCALLTYPE CorProfiler::AppDomainCreationStarted(AppDomainID appDomainId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::AppDomainCreationFinished(AppDomainID appDomainId, HRESULT hrStatus)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::AppDomainShutdownStarted(AppDomainID appDomainId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::AppDomainShutdownFinished(AppDomainID appDomainId, HRESULT hrStatus)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::AssemblyLoadStarted(AssemblyID assemblyId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::AssemblyLoadFinished(AssemblyID assemblyId, HRESULT hrStatus)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::AssemblyUnloadStarted(AssemblyID assemblyId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::AssemblyUnloadFinished(AssemblyID assemblyId, HRESULT hrStatus)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ModuleLoadStarted(ModuleID moduleId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ModuleLoadFinished(ModuleID moduleId, HRESULT hrStatus)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ModuleUnloadStarted(ModuleID moduleId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ModuleUnloadFinished(ModuleID moduleId, HRESULT hrStatus)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ModuleAttachedToAssembly(ModuleID moduleId, AssemblyID AssemblyId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ClassLoadStarted(ClassID classId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ClassLoadFinished(ClassID classId, HRESULT hrStatus)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ClassUnloadStarted(ClassID classId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ClassUnloadFinished(ClassID classId, HRESULT hrStatus)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::FunctionUnloadStarted(FunctionID functionId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::JITCompilationStarted(FunctionID functionId, BOOL fIsSafeToBlock)
{
    return S_OK;}

HRESULT STDMETHODCALLTYPE CorProfiler::JITCompilationFinished(FunctionID functionId, HRESULT hrStatus, BOOL fIsSafeToBlock)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::JITCachedFunctionSearchStarted(FunctionID functionId, BOOL *pbUseCachedFunction)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::JITCachedFunctionSearchFinished(FunctionID functionId, COR_PRF_JIT_CACHE result)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::JITFunctionPitched(FunctionID functionId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::JITInlining(FunctionID callerId, FunctionID calleeId, BOOL *pfShouldInline)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ThreadCreated(ThreadID threadId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ThreadDestroyed(ThreadID threadId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ThreadAssignedToOSThread(ThreadID managedThreadId, DWORD osThreadId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::RemotingClientInvocationStarted()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::RemotingClientSendingMessage(GUID *pCookie, BOOL fIsAsync)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::RemotingClientReceivingReply(GUID *pCookie, BOOL fIsAsync)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::RemotingClientInvocationFinished()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::RemotingServerReceivingMessage(GUID *pCookie, BOOL fIsAsync)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::RemotingServerInvocationStarted()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::RemotingServerInvocationReturned()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::RemotingServerSendingReply(GUID *pCookie, BOOL fIsAsync)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::UnmanagedToManagedTransition(FunctionID functionId, COR_PRF_TRANSITION_REASON reason)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ManagedToUnmanagedTransition(FunctionID functionId, COR_PRF_TRANSITION_REASON reason)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::RuntimeSuspendStarted(COR_PRF_SUSPEND_REASON suspendReason)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::RuntimeSuspendFinished()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::RuntimeSuspendAborted()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::RuntimeResumeStarted()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::RuntimeResumeFinished()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::RuntimeThreadSuspended(ThreadID threadId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::RuntimeThreadResumed(ThreadID threadId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::MovedReferences(ULONG cMovedObjectIDRanges, ObjectID oldObjectIDRangeStart[], ObjectID newObjectIDRangeStart[], ULONG cObjectIDRangeLength[])
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ObjectAllocated(ObjectID objectId, ClassID classId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ObjectsAllocatedByClass(ULONG cClassCount, ClassID classIds[], ULONG cObjects[])
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ObjectReferences(ObjectID objectId, ClassID classId, ULONG cObjectRefs, ObjectID objectRefIds[])
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::RootReferences(ULONG cRootRefs, ObjectID rootRefIds[])
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ExceptionSearchFunctionEnter(FunctionID functionId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ExceptionSearchFunctionLeave()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ExceptionSearchFilterEnter(FunctionID functionId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ExceptionSearchFilterLeave()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ExceptionSearchCatcherFound(FunctionID functionId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ExceptionOSHandlerEnter(UINT_PTR __unused)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ExceptionOSHandlerLeave(UINT_PTR __unused)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ExceptionUnwindFunctionEnter(FunctionID functionId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ExceptionUnwindFunctionLeave()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ExceptionUnwindFinallyEnter(FunctionID functionId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ExceptionUnwindFinallyLeave()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ExceptionCatcherEnter(FunctionID functionId, ObjectID objectId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ExceptionCatcherLeave()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::COMClassicVTableCreated(ClassID wrappedClassId, REFGUID implementedIID, void *pVTable, ULONG cSlots)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::COMClassicVTableDestroyed(ClassID wrappedClassId, REFGUID implementedIID, void *pVTable)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ExceptionCLRCatcherFound()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ExceptionCLRCatcherExecute()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ThreadNameChanged(ThreadID threadId, ULONG cchName, WCHAR name[])
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::SurvivingReferences(ULONG cSurvivingObjectIDRanges, ObjectID objectIDRangeStart[], ULONG cObjectIDRangeLength[])
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::FinalizeableObjectQueued(DWORD finalizerFlags, ObjectID objectID)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::RootReferences2(ULONG cRootRefs, ObjectID rootRefIds[], COR_PRF_GC_ROOT_KIND rootKinds[], COR_PRF_GC_ROOT_FLAGS rootFlags[], UINT_PTR rootIds[])
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::HandleCreated(GCHandleID handleId, ObjectID initialObjectId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::HandleDestroyed(GCHandleID handleId)
{
    return S_OK;
}


HRESULT STDMETHODCALLTYPE CorProfiler::ProfilerAttachComplete()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ProfilerDetachSucceeded()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ReJITCompilationStarted(FunctionID functionId, ReJITID rejitId, BOOL fIsSafeToBlock)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::GetReJITParameters(ModuleID moduleId, mdMethodDef methodId, ICorProfilerFunctionControl *pFunctionControl)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ReJITCompilationFinished(FunctionID functionId, ReJITID rejitId, HRESULT hrStatus, BOOL fIsSafeToBlock)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ReJITError(ModuleID moduleId, mdMethodDef methodId, FunctionID functionId, HRESULT hrStatus)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::MovedReferences2(ULONG cMovedObjectIDRanges, ObjectID oldObjectIDRangeStart[], ObjectID newObjectIDRangeStart[], SIZE_T cObjectIDRangeLength[])
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::SurvivingReferences2(ULONG cSurvivingObjectIDRanges, ObjectID objectIDRangeStart[], SIZE_T cObjectIDRangeLength[])
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ConditionalWeakTableElementReferences(ULONG cRootRefs, ObjectID keyRefIds[], ObjectID valueRefIds[], GCHandleID rootIds[])
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::GetAssemblyReferences(const WCHAR *wszAssemblyPath, ICorProfilerAssemblyReferenceProvider *pAsmRefProvider)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ModuleInMemorySymbolsUpdated(ModuleID moduleId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::DynamicMethodJITCompilationStarted(FunctionID functionId, BOOL fIsSafeToBlock, LPCBYTE ilHeader, ULONG cbILHeader)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::DynamicMethodJITCompilationFinished(FunctionID functionId, HRESULT hrStatus, BOOL fIsSafeToBlock)
{
    return S_OK;
}