// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License

//--------------------------------------------------------------------
//
// .NET helpers
//
//--------------------------------------------------------------------
#include "Includes.h"

//--------------------------------------------------------------------
//
// IsCoreClrProcess - Checks to see whether the process is a .NET Core
// process by checking the availability of a diagnostics server exposed
// as a Unix domain socket. If the pipe is available, we assume its a
// .NET Core process
//
// Returns: true   - if the process is a .NET Core process,[out] socketName
//                   will contain the full socket name. Caller owns the
//                   memory allocated for the socketName
//          false  - if the process is NOT a .NET Core process,[out] socketName
//                   will be NULL.
//
//--------------------------------------------------------------------
bool IsCoreClrProcess(pid_t pid, char** socketName)
{
    bool bRet = false;
    *socketName = NULL;
    FILE *procFile = NULL;
    char lineBuf[4096];
    char tmpFolder[4096];
    char* prefixTmpFolder = NULL;

    // If $TMPDIR is set, use it as the path, otherwise we use /tmp
    // per https://github.com/dotnet/diagnostics/blob/master/documentation/design-docs/ipc-protocol.md
    prefixTmpFolder = getenv("TMPDIR");
    if(prefixTmpFolder==NULL)
    {
        snprintf(tmpFolder, 4096, "/tmp/dotnet-diagnostic-%d", pid);
    }
    else
    {
        snprintf(tmpFolder, FILENAME_MAX, "%s/procdump-cancel-%d", prefixTmpFolder, getpid());
    }

    // Enumerate all open domain sockets exposed from the process. If one
    // exists by the following prefix, we assume its a .NET Core process:
    //    dotnet-diagnostic-{%d:PID}
    // The sockets are found in /proc/net/unix
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
                    // Found the correct socket...copy the name to the out param
                    *socketName = malloc(sizeof(char)*strlen(ptr)+1);
                    if(*socketName!=NULL)
                    {
                        memset(*socketName, 0, sizeof(char)*strlen(ptr)+1);
                        if(strncpy(*socketName, ptr, sizeof(char)*strlen(ptr)+1)!=NULL)
                        {
                            Trace("CoreCLR diagnostics socket: %s", socketName);
                            bRet = true;
                        }
                        break;
                    }
                }
            }
        }
    }
    else
    {
        Trace("Failed to open /proc/net/unix [%d].", errno);
    }

    if(procFile!=NULL)
    {
        fclose(procFile);
        procFile = NULL;
    }

    if(*socketName!=NULL && bRet==false)
    {
        free(*socketName);
        *socketName = NULL;
    }

    return bRet;
}

//--------------------------------------------------------------------
//
// GenerateCoreClrDump - Generates the .NET core dump using the
// diagnostics server.
//
// Returns: true   - if core dump was generated
//          false  - otherwise
//
//--------------------------------------------------------------------
bool GenerateCoreClrDump(char* socketName, char* dumpFileName)
{
    bool bRet = false;
    struct sockaddr_un addr = {0};
    int fd = 0;
    uint16_t* dumpFileNameW = NULL;
    void* temp_buffer = NULL;

    if( (dumpFileNameW = GetUint16(dumpFileName))!=NULL)
    {
        if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
        {
            Trace("Failed to create socket for .NET Core dump generation.");
        }
        else
        {
            // Create socket to diagnostics server
            memset(&addr, 0, sizeof(struct sockaddr_un));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, socketName, sizeof(addr.sun_path)-1);

            if (connect(fd, (struct sockaddr*)&addr, sizeof(struct sockaddr_un)) == -1)
            {
                Trace("Failed to connect to socket for .NET Core dump generation.");
            }
            else
            {
                unsigned int dumpFileNameLen = ((strlen(dumpFileName)+1));
                int payloadSize = sizeof(dumpFileNameLen);
                payloadSize += dumpFileNameLen*sizeof(wchar_t);
                unsigned int dumpType = CORECLR_DUMPTYPE_FULL;
                payloadSize += sizeof(dumpType);
                unsigned int diagnostics = CORECLR_DUMPLOGGING_OFF;
                payloadSize += sizeof(diagnostics);

                uint16_t totalPacketSize = sizeof(struct IpcHeader)+payloadSize;

                // First initialize header
                temp_buffer = malloc(totalPacketSize);
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

                    void* temp_buffer_cur = temp_buffer;

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

                    if(send(fd, temp_buffer, totalPacketSize, 0)==-1)
                    {
                        Trace("Failed sending packet to diagnostics server [%d]", errno);
                    }
                    else
                    {
                        // Lets get the header first
                        struct IpcHeader retHeader;
                        if(recv(fd, &retHeader, sizeof(struct IpcHeader), 0)==-1)
                        {
                            Trace("Failed receiving response header from diagnostics server [%d]", errno);
                        }
                        else
                        {
                            // Check the header to make sure its the right size
                            if(retHeader.Size != CORECLR_DIAG_IPCHEADER_SIZE)
                            {
                                Trace("Failed validating header size in response header from diagnostics server [%d != 24]", retHeader.Size);
                            }
                            else
                            {
                                // Next, get the payload which contains a single uint32 (hresult)
                                int32_t res = -1;
                                if(recv(fd, &res, sizeof(int32_t), 0)==-1)
                                {
                                    Trace("Failed receiving result code from response payload from diagnostics server [%d]", errno);
                                }
                                else
                                {
                                    if(res==0)
                                    {
                                        bRet = true;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }


    if(temp_buffer!=NULL)
    {
        free(temp_buffer);
        temp_buffer = NULL;
    }

    if(fd!=0)
    {
        close(fd);
        fd = 0;
    }

    if(dumpFileNameW!=NULL)
    {
        free(dumpFileNameW);
        dumpFileNameW = NULL;
    }

    return bRet;
}
