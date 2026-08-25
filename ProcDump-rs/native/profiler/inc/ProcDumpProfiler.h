// Copyright (c) .NET Foundation and contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#pragma once

#include <atomic>
#include <syslog.h>
#include <iostream>
#include <string>
#include <string>
#include <vector>
#include <sstream>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <linux/limits.h>

#include "cor.h"
#include "corprof.h"
#include "profilerstring.h"
#include "ProfilerCommon.h"
#include "easylogging++.h"

#define DETACH_TIMEOUT  30000
#define HEALTH_POLL_FREQ 5              // In secs

#define LOG_FILE    "/var/tmp/procdumpprofiler.log"
#define MAX_LOG_FILE_SIZE    "1000000"
#define DATE_LENGTH 26
#define PROFILER_STATUS_FAILURE 'F'
#define PROFILER_STATUS_HEALTH 'H'
#define PROFILER_STATUS_SUCCESS '1'

#define CORECLR_DUMPTYPE_FULL 4
#define CORECLR_DUMPLOGGING_OFF 0
#define CORECLR_DIAG_IPCHEADER_SIZE 24

// Magic version for the IpcHeader struct
struct MagicVersion
{
    uint8_t Magic[14];
};

// The header to be associated with every command and response
// to/from the diagnostics server
struct IpcHeader
{
    union
    {
        struct MagicVersion _magic;
        uint8_t  Magic[14];  // Magic Version number
    };

    uint16_t Size;       // The size of the incoming packet, size = header + payload size
    uint8_t  CommandSet; // The scope of the Command.
    uint8_t  CommandId;  // The command being sent
    uint16_t Reserved;   // reserved for future use
};

void* HealthThread(void* args);

//
// Simple mutex auto lock class (no copy constructor semantics or operator overloads)
//
class AutoMutex
{
private:
    pthread_mutex_t* mutex;
    bool locked;

public:
    AutoMutex(pthread_mutex_t* mut) : mutex(mut), locked(false)
    {
        if(pthread_mutex_lock(mutex)==0)
        {
            locked = true;
        }
    }

    ~AutoMutex()
    {
        if(locked==true)
        {
            pthread_mutex_unlock(mutex);
            locked = false;
        }
    }
};

class CorProfiler : public ICorProfilerCallback8
{
private:

    struct ExceptionMonitorEntry
    {
        std::string exception;
        int dumpsToCollect;
        int collectedDumps;
        ObjectID exceptionID;
    };

    std::atomic<int> refCount;
    ICorProfilerInfo* corProfilerInfo;
    ICorProfilerInfo8* corProfilerInfo8;
    std::vector<struct ExceptionMonitorEntry> exceptionMonitorList;
    std::vector<uint64_t> gcMemoryThresholdMonitorList;
    pthread_t healthThread;
    std::string processName;
    std::string fullDumpPath;
    pthread_mutex_t endDumpCondition;
    enum TriggerType triggerType;
    int currentThresholdIndex;
    int gcGeneration;
    bool gcGenStarted;

    String GetExceptionName(ObjectID objectId);
    String GetExceptionMessage(ObjectID objectId);
    bool ParseClientData(char* clientData);
    WCHAR* GetUint16(char* buffer);
    std::string GetDumpName(uint16_t dumpCount, std::string name);
    std::string GetProcessName();
    bool GenerateCoreClrDump(char* socketName, char* dumpFileName);
    bool IsCoreClrProcess(pid_t pid, char** socketName);
    char* GetPath(char* lineBuf);
    void CleanupProfiler();
    void SendCatastrophicFailureStatus();
    int send_all(int socket, void* buffer, size_t length);
    int recv_all(int socket, void* buffer, size_t length);
    bool WildcardSearch(WCHAR*, WCHAR*);
    u_int64_t GetGCHeapSize(int generation);
    bool WriteDumpHelper(std::string dumpName);
    bool IsHighPerfBasicGC();

public:
    CorProfiler();
    virtual ~CorProfiler();

    char* GetSocketPath(char* prefix, pid_t pid, pid_t targetPid);
    ICorProfilerInfo3* corProfilerInfo3;
    pid_t procDumpPid;

    int SendDumpCompletedStatus(std::string dump, char success);
    void UnloadProfiler();

    HRESULT STDMETHODCALLTYPE Initialize(IUnknown* pICorProfilerInfoUnk) override;
    HRESULT STDMETHODCALLTYPE Shutdown() override;
    HRESULT STDMETHODCALLTYPE AppDomainCreationStarted(AppDomainID appDomainId) override;
    HRESULT STDMETHODCALLTYPE AppDomainCreationFinished(AppDomainID appDomainId, HRESULT hrStatus) override;
    HRESULT STDMETHODCALLTYPE AppDomainShutdownStarted(AppDomainID appDomainId) override;
    HRESULT STDMETHODCALLTYPE AppDomainShutdownFinished(AppDomainID appDomainId, HRESULT hrStatus) override;
    HRESULT STDMETHODCALLTYPE AssemblyLoadStarted(AssemblyID assemblyId) override;
    HRESULT STDMETHODCALLTYPE AssemblyLoadFinished(AssemblyID assemblyId, HRESULT hrStatus) override;
    HRESULT STDMETHODCALLTYPE AssemblyUnloadStarted(AssemblyID assemblyId) override;
    HRESULT STDMETHODCALLTYPE AssemblyUnloadFinished(AssemblyID assemblyId, HRESULT hrStatus) override;
    HRESULT STDMETHODCALLTYPE ModuleLoadStarted(ModuleID moduleId) override;
    HRESULT STDMETHODCALLTYPE ModuleLoadFinished(ModuleID moduleId, HRESULT hrStatus) override;
    HRESULT STDMETHODCALLTYPE ModuleUnloadStarted(ModuleID moduleId) override;
    HRESULT STDMETHODCALLTYPE ModuleUnloadFinished(ModuleID moduleId, HRESULT hrStatus) override;
    HRESULT STDMETHODCALLTYPE ModuleAttachedToAssembly(ModuleID moduleId, AssemblyID AssemblyId) override;
    HRESULT STDMETHODCALLTYPE ClassLoadStarted(ClassID classId) override;
    HRESULT STDMETHODCALLTYPE ClassLoadFinished(ClassID classId, HRESULT hrStatus) override;
    HRESULT STDMETHODCALLTYPE ClassUnloadStarted(ClassID classId) override;
    HRESULT STDMETHODCALLTYPE ClassUnloadFinished(ClassID classId, HRESULT hrStatus) override;
    HRESULT STDMETHODCALLTYPE FunctionUnloadStarted(FunctionID functionId) override;
    HRESULT STDMETHODCALLTYPE JITCompilationStarted(FunctionID functionId, BOOL fIsSafeToBlock) override;
    HRESULT STDMETHODCALLTYPE JITCompilationFinished(FunctionID functionId, HRESULT hrStatus, BOOL fIsSafeToBlock) override;
    HRESULT STDMETHODCALLTYPE JITCachedFunctionSearchStarted(FunctionID functionId, BOOL* pbUseCachedFunction) override;
    HRESULT STDMETHODCALLTYPE JITCachedFunctionSearchFinished(FunctionID functionId, COR_PRF_JIT_CACHE result) override;
    HRESULT STDMETHODCALLTYPE JITFunctionPitched(FunctionID functionId) override;
    HRESULT STDMETHODCALLTYPE JITInlining(FunctionID callerId, FunctionID calleeId, BOOL* pfShouldInline) override;
    HRESULT STDMETHODCALLTYPE ThreadCreated(ThreadID threadId) override;
    HRESULT STDMETHODCALLTYPE ThreadDestroyed(ThreadID threadId) override;
    HRESULT STDMETHODCALLTYPE ThreadAssignedToOSThread(ThreadID managedThreadId, DWORD osThreadId) override;
    HRESULT STDMETHODCALLTYPE RemotingClientInvocationStarted() override;
    HRESULT STDMETHODCALLTYPE RemotingClientSendingMessage(GUID* pCookie, BOOL fIsAsync) override;
    HRESULT STDMETHODCALLTYPE RemotingClientReceivingReply(GUID* pCookie, BOOL fIsAsync) override;
    HRESULT STDMETHODCALLTYPE RemotingClientInvocationFinished() override;
    HRESULT STDMETHODCALLTYPE RemotingServerReceivingMessage(GUID* pCookie, BOOL fIsAsync) override;
    HRESULT STDMETHODCALLTYPE RemotingServerInvocationStarted() override;
    HRESULT STDMETHODCALLTYPE RemotingServerInvocationReturned() override;
    HRESULT STDMETHODCALLTYPE RemotingServerSendingReply(GUID* pCookie, BOOL fIsAsync) override;
    HRESULT STDMETHODCALLTYPE UnmanagedToManagedTransition(FunctionID functionId, COR_PRF_TRANSITION_REASON reason) override;
    HRESULT STDMETHODCALLTYPE ManagedToUnmanagedTransition(FunctionID functionId, COR_PRF_TRANSITION_REASON reason) override;
    HRESULT STDMETHODCALLTYPE RuntimeSuspendStarted(COR_PRF_SUSPEND_REASON suspendReason) override;
    HRESULT STDMETHODCALLTYPE RuntimeSuspendFinished() override;
    HRESULT STDMETHODCALLTYPE RuntimeSuspendAborted() override;
    HRESULT STDMETHODCALLTYPE RuntimeResumeStarted() override;
    HRESULT STDMETHODCALLTYPE RuntimeResumeFinished() override;
    HRESULT STDMETHODCALLTYPE RuntimeThreadSuspended(ThreadID threadId) override;
    HRESULT STDMETHODCALLTYPE RuntimeThreadResumed(ThreadID threadId) override;
    HRESULT STDMETHODCALLTYPE MovedReferences(ULONG cMovedObjectIDRanges, ObjectID oldObjectIDRangeStart[], ObjectID newObjectIDRangeStart[], ULONG cObjectIDRangeLength[]) override;
    HRESULT STDMETHODCALLTYPE ObjectAllocated(ObjectID objectId, ClassID classId) override;
    HRESULT STDMETHODCALLTYPE ObjectsAllocatedByClass(ULONG cClassCount, ClassID classIds[], ULONG cObjects[]) override;
    HRESULT STDMETHODCALLTYPE ObjectReferences(ObjectID objectId, ClassID classId, ULONG cObjectRefs, ObjectID objectRefIds[]) override;
    HRESULT STDMETHODCALLTYPE RootReferences(ULONG cRootRefs, ObjectID rootRefIds[]) override;
    HRESULT STDMETHODCALLTYPE ExceptionThrown(ObjectID thrownObjectId) override;
    HRESULT STDMETHODCALLTYPE ExceptionSearchFunctionEnter(FunctionID functionId) override;
    HRESULT STDMETHODCALLTYPE ExceptionSearchFunctionLeave() override;
    HRESULT STDMETHODCALLTYPE ExceptionSearchFilterEnter(FunctionID functionId) override;
    HRESULT STDMETHODCALLTYPE ExceptionSearchFilterLeave() override;
    HRESULT STDMETHODCALLTYPE ExceptionSearchCatcherFound(FunctionID functionId) override;
    HRESULT STDMETHODCALLTYPE ExceptionOSHandlerEnter(UINT_PTR __unused) override;
    HRESULT STDMETHODCALLTYPE ExceptionOSHandlerLeave(UINT_PTR __unused) override;
    HRESULT STDMETHODCALLTYPE ExceptionUnwindFunctionEnter(FunctionID functionId) override;
    HRESULT STDMETHODCALLTYPE ExceptionUnwindFunctionLeave() override;
    HRESULT STDMETHODCALLTYPE ExceptionUnwindFinallyEnter(FunctionID functionId) override;
    HRESULT STDMETHODCALLTYPE ExceptionUnwindFinallyLeave() override;
    HRESULT STDMETHODCALLTYPE ExceptionCatcherEnter(FunctionID functionId, ObjectID objectId) override;
    HRESULT STDMETHODCALLTYPE ExceptionCatcherLeave() override;
    HRESULT STDMETHODCALLTYPE COMClassicVTableCreated(ClassID wrappedClassId, REFGUID implementedIID, void* pVTable, ULONG cSlots) override;
    HRESULT STDMETHODCALLTYPE COMClassicVTableDestroyed(ClassID wrappedClassId, REFGUID implementedIID, void* pVTable) override;
    HRESULT STDMETHODCALLTYPE ExceptionCLRCatcherFound() override;
    HRESULT STDMETHODCALLTYPE ExceptionCLRCatcherExecute() override;
    HRESULT STDMETHODCALLTYPE ThreadNameChanged(ThreadID threadId, ULONG cchName, WCHAR name[]) override;
    HRESULT STDMETHODCALLTYPE GarbageCollectionStarted(int cGenerations, BOOL generationCollected[], COR_PRF_GC_REASON reason) override;
    HRESULT STDMETHODCALLTYPE SurvivingReferences(ULONG cSurvivingObjectIDRanges, ObjectID objectIDRangeStart[], ULONG cObjectIDRangeLength[]) override;
    HRESULT STDMETHODCALLTYPE GarbageCollectionFinished() override;
    HRESULT STDMETHODCALLTYPE FinalizeableObjectQueued(DWORD finalizerFlags, ObjectID objectID) override;
    HRESULT STDMETHODCALLTYPE RootReferences2(ULONG cRootRefs, ObjectID rootRefIds[], COR_PRF_GC_ROOT_KIND rootKinds[], COR_PRF_GC_ROOT_FLAGS rootFlags[], UINT_PTR rootIds[]) override;
    HRESULT STDMETHODCALLTYPE HandleCreated(GCHandleID handleId, ObjectID initialObjectId) override;
    HRESULT STDMETHODCALLTYPE HandleDestroyed(GCHandleID handleId) override;
    HRESULT STDMETHODCALLTYPE InitializeForAttach(IUnknown* pCorProfilerInfoUnk, void* pvClientData, UINT cbClientData) override;
    HRESULT STDMETHODCALLTYPE ProfilerAttachComplete() override;
    HRESULT STDMETHODCALLTYPE ProfilerDetachSucceeded() override;
    HRESULT STDMETHODCALLTYPE ReJITCompilationStarted(FunctionID functionId, ReJITID rejitId, BOOL fIsSafeToBlock) override;
    HRESULT STDMETHODCALLTYPE GetReJITParameters(ModuleID moduleId, mdMethodDef methodId, ICorProfilerFunctionControl* pFunctionControl) override;
    HRESULT STDMETHODCALLTYPE ReJITCompilationFinished(FunctionID functionId, ReJITID rejitId, HRESULT hrStatus, BOOL fIsSafeToBlock) override;
    HRESULT STDMETHODCALLTYPE ReJITError(ModuleID moduleId, mdMethodDef methodId, FunctionID functionId, HRESULT hrStatus) override;
    HRESULT STDMETHODCALLTYPE MovedReferences2(ULONG cMovedObjectIDRanges, ObjectID oldObjectIDRangeStart[], ObjectID newObjectIDRangeStart[], SIZE_T cObjectIDRangeLength[]) override;
    HRESULT STDMETHODCALLTYPE SurvivingReferences2(ULONG cSurvivingObjectIDRanges, ObjectID objectIDRangeStart[], SIZE_T cObjectIDRangeLength[]) override;
    HRESULT STDMETHODCALLTYPE ConditionalWeakTableElementReferences(ULONG cRootRefs, ObjectID keyRefIds[], ObjectID valueRefIds[], GCHandleID rootIds[]) override;
    HRESULT STDMETHODCALLTYPE GetAssemblyReferences(const WCHAR* wszAssemblyPath, ICorProfilerAssemblyReferenceProvider* pAsmRefProvider) override;
    HRESULT STDMETHODCALLTYPE ModuleInMemorySymbolsUpdated(ModuleID moduleId) override;

    HRESULT STDMETHODCALLTYPE DynamicMethodJITCompilationStarted(FunctionID functionId, BOOL fIsSafeToBlock, LPCBYTE ilHeader, ULONG cbILHeader) override;
    HRESULT STDMETHODCALLTYPE DynamicMethodJITCompilationFinished(FunctionID functionId, HRESULT hrStatus, BOOL fIsSafeToBlock) override;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override
    {
        if (riid == __uuidof(ICorProfilerCallback8) ||
            riid == __uuidof(ICorProfilerCallback7) ||
            riid == __uuidof(ICorProfilerCallback6) ||
            riid == __uuidof(ICorProfilerCallback5) ||
            riid == __uuidof(ICorProfilerCallback4) ||
            riid == __uuidof(ICorProfilerCallback3) ||
            riid == __uuidof(ICorProfilerCallback2) ||
            riid == __uuidof(ICorProfilerCallback)  ||
            riid == IID_IUnknown)
        {
            *ppvObject = this;
            this->AddRef();
            return S_OK;
        }

        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef(void) override
    {
        return std::atomic_fetch_add(&this->refCount, 1) + 1;
    }

    ULONG STDMETHODCALLTYPE Release(void) override
    {
        int count = std::atomic_fetch_sub(&this->refCount, 1) - 1;

        if (count <= 0)
        {
            delete this;
        }

        return count;
    }
};