// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License

//--------------------------------------------------------------------
//
// General purpose helpers
//
//--------------------------------------------------------------------
#include "Includes.h"

// These are the start and end addresses to the embedded profiler binary
extern char _binary_ProcDumpProfiler_so_end[];
extern char _binary_ProcDumpProfiler_so_start[];

//--------------------------------------------------------------------
//
// ExtractProfiler
//
// The profiler so is embedded into the ProcDump binary. This function
// extracts the profiler so and places it into
// PROCDUMP_DIR "/" PROFILER_FILE_NAME
//
//--------------------------------------------------------------------
int ExtractProfiler()
{
    auto_free_fd int destfd = -1;

    // Try to delete the profiler lib in case it was left over...
    unlink(PROCDUMP_DIR "/" PROFILER_FILE_NAME);

    destfd = creat(PROCDUMP_DIR "/" PROFILER_FILE_NAME, S_IRWXU|S_IROTH);
    if (destfd < 0)
    {
        return -1;
    }

    size_t written = 0;
    ssize_t writeRet;
    size_t size = _binary_ProcDumpProfiler_so_end - _binary_ProcDumpProfiler_so_start;

    while (written < size)
    {
        writeRet = write(destfd, _binary_ProcDumpProfiler_so_start + written, size - written);
        if (writeRet < 0)
        {
            return 1;
        }
        written += writeRet;
    }

    return 0;
}

//--------------------------------------------------------------------
//
// LoadProfiler
//
// This function sends a command to the diagnostics pipe of the target
// process instructing the runtime to load the profiler.
//
//--------------------------------------------------------------------
int LoadProfiler(pid_t pid, char* clientData)
{
    struct sockaddr_un addr = {0};
    uint32_t attachTimeout = 5000;
    struct CLSID profilerGuid = {0};
    unsigned int clientDataSize = 0;

    auto_free_fd int fd = -1;
    auto_free char* socketName = NULL;
    auto_free uint16_t* profilerPathW = NULL;
    auto_free void* temp_buffer = NULL;

    if(!IsCoreClrProcess(pid, &socketName))
    {
        Trace("LoadProfiler: Unable to find .NET diagnostics endpoint for targeted process.");
        return -1;
    }

    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
    {
        Trace("LoadProfiler: Failed to create socket for .NET dump generation.");
        return -1;
    }

    // Create socket to diagnostics server
    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socketName, sizeof(addr.sun_path)-1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(struct sockaddr_un)) == -1)
    {
        Trace("LoadProfiler: Failed to connect to socket for .NET profiler load.");
        return -1;
    }

    // Calculate header and payload size

    // profile attach timeout
    unsigned int payloadSize = sizeof(attachTimeout);

    // profiler guid
    StringToGuid(const_cast<char*>(PROFILER_GUID), &profilerGuid);
    payloadSize += sizeof(profilerGuid);

    //profiler path
    profilerPathW = GetUint16(const_cast<char*>(PROCDUMP_DIR "/" PROFILER_FILE_NAME));
    if(profilerPathW==NULL)
    {
        Trace("LoadProfiler: Failed to GetUint16.");
        return -1;
    }

    unsigned int profilerPathLen = (strlen(PROCDUMP_DIR "/" PROFILER_FILE_NAME)+1);

    payloadSize += sizeof(profilerPathLen);
    payloadSize += profilerPathLen*sizeof(uint16_t);

    // client data
    if(clientData)
    {
        clientDataSize = strlen(clientData) + 1;
    }
    else
    {
        clientDataSize = 0;
    }

    payloadSize += sizeof(clientDataSize);
    payloadSize += clientDataSize*sizeof(unsigned char);

    Trace("LoadProfiler: client data: %s", clientData);

    uint16_t totalPacketSize = sizeof(struct IpcHeader)+payloadSize;

    // First initialize header
    temp_buffer = malloc(totalPacketSize);
    if(temp_buffer==NULL)
    {
        Trace("LoadProfiler: Failed to allocate memory for packet.")
        return -1;
    }

    memset(temp_buffer, 0, totalPacketSize);
    struct IpcHeader dumpHeader =
    {
        { {"DOTNET_IPC_V1"} },
        (uint16_t)totalPacketSize,
        (uint8_t)0x03,
        (uint8_t)0x01,
        (uint16_t)0x0000
    };

    char* temp_buffer_cur = (char*) temp_buffer;

    memcpy(temp_buffer_cur, &dumpHeader, sizeof(struct IpcHeader));
    temp_buffer_cur += sizeof(struct IpcHeader);

    memcpy(temp_buffer_cur, &attachTimeout, sizeof(attachTimeout));
    temp_buffer_cur += sizeof(attachTimeout);

    memcpy(temp_buffer_cur, &profilerGuid, sizeof(profilerGuid));
    temp_buffer_cur += sizeof(profilerGuid);

    memcpy(temp_buffer_cur, &profilerPathLen, sizeof(profilerPathLen));
    temp_buffer_cur += sizeof(profilerPathLen);

    memcpy(temp_buffer_cur, profilerPathW, profilerPathLen*sizeof(uint16_t));
    temp_buffer_cur += profilerPathLen*sizeof(uint16_t);

    memcpy(temp_buffer_cur, &clientDataSize, sizeof(unsigned int));
    temp_buffer_cur += sizeof(unsigned int);

    if(clientDataSize>0)
    {
        memcpy(temp_buffer_cur, clientData, clientDataSize);
        temp_buffer_cur += clientDataSize;
    }

    // Send the payload
    if(send_all(fd, temp_buffer, totalPacketSize)==-1)
    {
        Trace("LoadProfiler: Failed sending packet to diagnostics server [%d]", errno);
        return -1;
    }

    // Get response
    struct IpcHeader retHeader;
    if(recv_all(fd, &retHeader, sizeof(struct IpcHeader))==-1)
    {
        Trace("LoadProfiler: Failed receiving response header from diagnostics server [%d]", errno);
        return -1;
    }

    // Check the header to make sure its the right size
    if(retHeader.Size != CORECLR_DIAG_IPCHEADER_SIZE)
    {
        Trace("LoadProfiler: Failed validating header size in response header from diagnostics server [%d != 24]", retHeader.Size);
        return -1;
    }

    int32_t res = -1;
    if(recv_all(fd, &res, sizeof(int32_t))==-1)
    {
        Trace("LoadProfiler: Failed receiving result code from response payload from diagnostics server [%d]", errno);
        return -1;
    }
    else
    {
        if(res!=0)
        {
            if(res==0x8013136A)
            {
                // The profiler is already loaded in the target process
                Trace("LoadProfiler: Target process is already being monitored.");
                Log(error, "Target process is already being monitored.");
            }
            else
            {
                Trace("LoadProfiler: Error returned from diagnostics server [0x%x]", res);
                Log(error, "Error returned from diagnostics server [0x%x]", res);
            }

            return -1;
        }
    }

#if (__GNUC__ >= 13)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"
#endif
    return 0;
}
#if (__GNUC__ >= 13)
#pragma GCC diagnostic pop
#endif

//--------------------------------------------------------------------
//
// InjectProfiler
//
// Injects the profiler into the target process by:
//
// 1. Extracting the profiler binary from the procdump binary and placing in
//    tmp location.
// 2. Send a command to the target process diagnostics pipe with the location
//    of the profiler (passing in any relevant data such as exception filter and
//    dump count). The profiler opens an IPC pipe that ProcDump can send
//    cancellation requests to.
//
//--------------------------------------------------------------------
int InjectProfiler(pid_t pid, char* clientData)
{
    Trace("InjectProfiler: Starting injection of profiler into target process %d", pid);

    int ret = ExtractProfiler();
    if(ret != 0)
    {
        Log(error, "Failed to extract profiler. Please make sure you are running elevated.");
        Trace("InjectProfiler: failed to extract profiler.");
        return ret;
    }

    ret = LoadProfiler(pid, clientData);
    if(ret != 0)
    {
        Log(error, "Failed to load profiler. Please make sure you are running elevated and targetting a .NET process.");
        Trace("InjectProfiler: failed to load profiler into target process.");
        return ret;
    }

    Trace("InjectProfiler: Successfully injected profiler into target process %d", pid);

    return 0;
}

//--------------------------------------------------------------------
//
// GetEncodedExceptionFilter
//
// Create the exception filter string which goes like this:
//    <fullpathtodumplocation>;<pidofprocdump>;<exception>:<numdumps>;<exception>:<numdumps>,...
// Where Exception is:
//    Name_of_Exception:num_dumps or
//    <any> which means that the user did not specify an exception filter and we just dump on
//          any exception that happens to throw.
// The exception filter passed in from the command line using -f:
//    <Name_ofException>,<Name_ofException>,...
//
//--------------------------------------------------------------------
char* GetEncodedExceptionFilter(char* exceptionFilterCmdLine, unsigned int numDumps)
{
    unsigned int totalExceptionNameLen = 0;
    unsigned int numberOfDumpsLen = 0;
    unsigned int numExceptions = 0;
    char* exceptionFilter = NULL;
    char* exceptionFilterCur = NULL;
    char tmp[10];
    size_t len = 0;

    // If no exceptions were specified using -f we should dump on any exception (hence we add <any>)
    char* cpy = exceptionFilterCmdLine ? strdup(exceptionFilterCmdLine) : strdup("*");

    numberOfDumpsLen = sprintf(tmp, "%d", numDumps);

    char* token = strtok(cpy, ",");
    while(token!=NULL)
    {
        totalExceptionNameLen += strlen(token);
        numExceptions++;
        token = strtok(NULL, ",");
    }

    free(cpy);

    cpy = exceptionFilterCmdLine ? strdup(exceptionFilterCmdLine) : strdup("*");

    totalExceptionNameLen++; // NULL terminator

    exceptionFilter = (char*) malloc(totalExceptionNameLen+numExceptions*(numberOfDumpsLen+2+2)); // +1 for : seperator +1 for ; seperator +2 for 2 '*' wildcard
    if(exceptionFilter==NULL)
    {
        free(cpy);
        return NULL;
    }

    exceptionFilterCur = exceptionFilter;

    token = strtok(cpy, ",");
    while(token!=NULL)
    {
        len = strlen(token);
        if(token[0] != '*' && token[len-1] != '*')
        {
            exceptionFilterCur += sprintf(exceptionFilterCur, "*%s*:%d;", token, numDumps);
        }
        else if(token[0] != '*')
        {
            exceptionFilterCur += sprintf(exceptionFilterCur, "*%s:%d;", token, numDumps);
        }
        else if(token[len-1] != '*')
        {
            exceptionFilterCur += sprintf(exceptionFilterCur, "%s*:%d;", token, numDumps);
        }
        else
        {
            exceptionFilterCur += sprintf(exceptionFilterCur, "%s:%d;", token, numDumps);
        }
        token = strtok(NULL, ",");
    }

    free(cpy);
    return exceptionFilter;
}