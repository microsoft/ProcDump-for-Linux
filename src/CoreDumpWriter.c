// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License

//--------------------------------------------------------------------
//
// Core dump orchestrator
//
//--------------------------------------------------------------------


#include "CoreDumpWriter.h"

char *sanitize(char *processName);

static const char *CoreDumpTypeStrings[] = { "commit", "cpu", "thread", "filedesc", "signal", "time", "manual" };

bool GenerateCoreClrDump(char* socketName, char* dumpFileName);
bool IsCoreClrProcess(struct CoreDumpWriter *self, char** socketName);
char* GetPath(char* lineBuf);
uint16_t* GetUint16(char* buffer);

int WriteCoreDumpInternal(struct CoreDumpWriter *self, char* socketName);
FILE *popen2(const char *command, const char *type, pid_t *pid);

extern const struct IpcHeader GenericSuccessHeader;

//--------------------------------------------------------------------
//
// NewCoreDumpWriter - Helper function for newing a struct CoreDumpWriter
//
// Returns: struct CoreDumpWriter *
//
//--------------------------------------------------------------------
struct CoreDumpWriter *NewCoreDumpWriter(enum ECoreDumpType type, struct ProcDumpConfiguration *config)
{
    struct CoreDumpWriter *writer = (struct CoreDumpWriter *)malloc(sizeof(struct CoreDumpWriter));
    if (writer == NULL) {
        Log(error, INTERNAL_ERROR);
        Trace("NewCoreDumpWriter: failed to allocate memory.");
        exit(-1);
    }

    writer->Config = config;
    writer->Type = type;

    return writer;
}


//--------------------------------------------------------------------
//
// GetPath - Parses out the path from a full line read from 
//           /proc/net/unix. Example line:
//
//           0000000000000000: 00000003 00000000 00000000 0001 03 20287 @/tmp/.X11-unix/X0
//
// Returns: path   - point to path if it exists, NULL otherwise. 
//
//--------------------------------------------------------------------
char* GetPath(char* lineBuf)
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
bool IsCoreClrProcess(struct CoreDumpWriter *self, char** socketName)
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
        snprintf(tmpFolder, 4096, "/tmp/dotnet-diagnostic-%d", self->Config->ProcessId);
    }
    else
    {
        strncpy(tmpFolder, prefixTmpFolder, 4096);
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
// WaitForQuit - Wait for Quit Event or just timeout
//
//      Timed wait with awareness of quit event  
//
// Returns: 0   - Success
//          -1  - Failure
//          1   - Quit/Limit reached
//
//--------------------------------------------------------------------
int WriteCoreDump(struct CoreDumpWriter *self)
{
    int rc = 0;

    // Enter critical section (block till we decrement semaphore)
    rc = WaitForQuitOrEvent(self->Config, &self->Config->semAvailableDumpSlots, INFINITE_WAIT);
    if(rc == 0){
        Log(error, INTERNAL_ERROR);
        Trace("WriteCoreDump: failed WaitForQuitOrEvent.");
        exit(-1);
    }
    if(pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL) != 0){
        Log(error, INTERNAL_ERROR);
        Trace("WriteCoreDump: failed pthread_setcanceltype.");
        exit(-1);
    }
    switch (rc) {
        case WAIT_OBJECT_0: // QUIT!  Time for cleanup, no dump
            break;
        case WAIT_OBJECT_0+1: // We got a dump slot!
            { 
                char* socketName = NULL;
                IsCoreClrProcess(self, &socketName);
                if ((rc = WriteCoreDumpInternal(self, socketName)) == 0) {
                    // We're done here, unlock (increment) the sem
                    if(sem_post(&self->Config->semAvailableDumpSlots.semaphore) == -1){
                        Log(error, INTERNAL_ERROR);
                        Trace("WriteCoreDump: failed sem_post.");
                        if(socketName) free(socketName);
                        exit(-1);
                    }
                }
                if(socketName) free(socketName);
            }
            break;
        case WAIT_ABANDONED: // We've hit the dump limit, clean up
            break;
        default:
            Trace("WriteCoreDump: Error in default case");
            break;
    }
    if(pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL) != 0){
        Log(error, INTERNAL_ERROR);
        Trace("WriteCoreDump: failed pthread_setcanceltype.");
        exit(-1);
    }

    return rc;
}

//--------------------------------------------------------------------
//
// GetUint16 - Quick and dirty conversion from char to uint16_t 
//
// Returns: uint16_t*   - if successfully converted, NULL otherwise.
//                        Caller must free upon success
//
//--------------------------------------------------------------------
uint16_t* GetUint16(char* buffer)
{
    uint16_t* dumpFileNameW = NULL;

    if(buffer!=NULL)
    {
        dumpFileNameW = malloc((strlen(buffer)+1)*sizeof(uint16_t));
        for(int i=0; i<(strlen(buffer)+1); i++)
        {
            dumpFileNameW[i] = (uint16_t) buffer[i];
        }
    }

    return dumpFileNameW;
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

/// CRITICAL SECTION
/// Should only ever have <max number of dump slots> running concurrently
/// The default value of which is 1 (hard coded) and is set in
/// ProcDumpConfiguration.semAvailableDumpSlots
/// Returns 1 if we trigger quit in the crit section, 0 otherwise
int WriteCoreDumpInternal(struct CoreDumpWriter *self, char* socketName)
{
    char date[DATE_LENGTH];
    char command[BUFFER_LENGTH];
    char ** outputBuffer;
    char lineBuffer[BUFFER_LENGTH];
    char gcorePrefixName[BUFFER_LENGTH];
    char coreDumpFileName[BUFFER_LENGTH];
    int  lineLength;
    int  i;
    int  rc = 0;
    time_t rawTime;

    pid_t gcorePid;
    struct tm* timerInfo = NULL;
    FILE *commandPipe = NULL;

    const char *desc = CoreDumpTypeStrings[self->Type];
    char *name = sanitize(self->Config->ProcessName);
    pid_t pid = self->Config->ProcessId;

    // allocate output buffer
    outputBuffer = (char**)malloc(sizeof(char*) * MAX_LINES);
    if(outputBuffer == NULL){
        Log(error, INTERNAL_ERROR);
        Trace("WriteCoreDumpInternal: failed gcore output buffer allocation");
        exit(-1);
    }

    // get time for current dump generated
    rawTime = time(NULL);
    if((timerInfo = localtime(&rawTime)) == NULL){
        Log(error, INTERNAL_ERROR);
        Trace("WriteCoreDumpInternal: failed localtime");
        exit(-1);
    }
    strftime(date, 26, "%Y-%m-%d_%H:%M:%S", timerInfo);

    // assemble the full file name (including path) for core dumps
    if(self->Config->CoreDumpName != NULL) {
        if(snprintf(gcorePrefixName, BUFFER_LENGTH, "%s/%s_%d",
                    self->Config->CoreDumpPath,
                    self->Config->CoreDumpName,
                    self->Config->NumberOfDumpsCollected) < 0) {
            Log(error, INTERNAL_ERROR);
            Trace("WriteCoreDumpInternal: failed sprintf custom output file name");
            exit(-1);
        }
    } else {
        if(snprintf(gcorePrefixName, BUFFER_LENGTH, "%s/%s_%s_%s",
                    self->Config->CoreDumpPath, name, desc, date) < 0) {
            Log(error, INTERNAL_ERROR);
            Trace("WriteCoreDumpInternal: failed sprintf default output file name");
            exit(-1);
        }
    }

    // assemble the command
    if(snprintf(command, BUFFER_LENGTH, "gcore -o %s %d 2>&1", gcorePrefixName, pid) < 0){
        Log(error, INTERNAL_ERROR);
        Trace("WriteCoreDumpInternal: failed sprintf gcore command");
        exit(-1);
    }

    // assemble filename
    if(snprintf(coreDumpFileName, BUFFER_LENGTH, "%s.%d", gcorePrefixName, pid) < 0){
        Log(error, INTERNAL_ERROR);
        Trace("WriteCoreDumpInternal: failed sprintf core file name");
        exit(-1);
    }

    // check if we're allowed to write into the target directory
    if(access(self->Config->CoreDumpPath, W_OK) < 0) {
        Log(error, INTERNAL_ERROR);
        Trace("WriteCoreDumpInternal: no write permission to core dump target file %s",
              coreDumpFileName);
        exit(-1);
    }

    if(socketName!=NULL)
    {
        // If we have a socket name, we're dumping a .NET Core 3+ process....
        if(GenerateCoreClrDump(socketName, coreDumpFileName)==false)
        {
            Log(error, "An error occurred while generating the core dump for .NET 3.x+ process");
        }
        else
        {
            // log out sucessful core dump generated
            Log(info, "Core dump %d generated: %s", self->Config->NumberOfDumpsCollected, coreDumpFileName);

            self->Config->NumberOfDumpsCollected++; // safe to increment in crit section
            if (self->Config->NumberOfDumpsCollected >= self->Config->NumberOfDumpsToCollect) {
                SetEvent(&self->Config->evtQuit.event); // shut it down, we're done here
                rc = 1;
            }
        }

        free(outputBuffer);
    }
    else
    {
        // Oterwise, we use gcore dump generation   TODO: We might consider adding a forcegcore flag in cases where
        // someone wants to use gcore even for .NET Core 3.x+ processes.
        commandPipe = popen2(command, "r", &gcorePid);
        self->Config->gcorePid = gcorePid;
        
        if(commandPipe == NULL){
            Log(error, "An error occurred while generating the core dump");
            Trace("WriteCoreDumpInternal: Failed to open pipe to gcore");
            exit(1);
        }
        
        // read all output from gcore command
        for(i = 0; i < MAX_LINES && fgets(lineBuffer, sizeof(lineBuffer), commandPipe) != NULL; i++) {
            lineLength = strlen(lineBuffer);                                // get # of characters read

            outputBuffer[i] = (char*)malloc(sizeof(char) * lineLength);
            if(outputBuffer[i] != NULL) {
                strncpy(outputBuffer[i], lineBuffer, lineLength - 1);           // trim newline off
                outputBuffer[i][lineLength-1] = '\0';                           // append null character
            }
            else {
                Log(error, INTERNAL_ERROR);
                Trace("WriteCoreDumpInternal: failed to allocate gcore error message buffer");
                exit(-1);
            }
        }
        
        // close pipe reading from gcore
        self->Config->gcorePid = NO_PID;                // reset gcore pid so that signal handler knows we aren't dumping
        pclose(commandPipe);

        // check if gcore was able to generate the dump
        if(strstr(outputBuffer[i-1], "gcore: failed") != NULL){
            Log(error, "An error occurred while generating the core dump");
                    
            // log gcore message
            for(int j = 0; j < i; j++){
                if(outputBuffer[j] != NULL){
                    Log(error, "GCORE - %s", outputBuffer[j]);
                }
            }

            exit(1);
        }

        for(int j = 0; j < i; j++) {
            free(outputBuffer[j]);
        }
        free(outputBuffer);

        // On WSL2 there is a delay between the core dump being written to disk and able to succesfully access it in the below check
        sleep(1);

        // validate that core dump file was generated
        if(access(coreDumpFileName, F_OK) != -1) {
            if(self->Config->nQuit){
                // if we are in a quit state from interrupt delete partially generated core dump file
                int ret = unlink(coreDumpFileName);
                if (ret < 0 && errno != ENOENT) {
                    Trace("WriteCoreDumpInternal: Failed to remove partial core dump");
                    exit(-1);
                }
            }
            else{
                // log out sucessful core dump generated
                Log(info, "Core dump %d generated: %s", self->Config->NumberOfDumpsCollected, coreDumpFileName);

                self->Config->NumberOfDumpsCollected++; // safe to increment in crit section
                if (self->Config->NumberOfDumpsCollected >= self->Config->NumberOfDumpsToCollect) {
                    SetEvent(&self->Config->evtQuit.event); // shut it down, we're done here
                    rc = 1;
                }
            }
        }
    }

    free(name);

    return rc;
}

//--------------------------------------------------------------------
//
// popen2 - alternate popen that surfaces the pid of the spawned process
//
// Parameters: command (const char *) - the string containing the command to execute in the child thread
//             type (const char *) - either "r" for read or "w" for write
//             pid (pidt_t *) - out variable containing the pid of the spawned process
//
// Returns: FILE* pointing to the r or w of the pip between this thread and the spawned
//
//--------------------------------------------------------------------
FILE *popen2(const char *command, const char *type, pid_t *pid)
{
    // per man page: "...opens a process by creating a pipe, forking, and invoking the shell..."
    int pipefd[2]; // 0 -> read, 1 -> write
    pid_t childPid;

    if ((pipe(pipefd)) == -1) {
        Log(error, INTERNAL_ERROR);
        Trace("popen2: unable to open pipe");
        exit(-1);
    }

    // fork and then ensure we have the correct end of the pipe open
    if ((childPid = fork()) == -1) {
        Log(error, INTERNAL_ERROR);
        Trace("popen2: unable to fork process");
        exit(-1);
    }

    if (childPid == 0) {
        // Child
        setpgid(0,0); // give the child and descendants their own pgid so we can terminate gcore separately

        if (type[0] == 'r') {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO); // redirect stdout to write end of pipe
        } else {
            close(pipefd[1]);
            dup2(pipefd[0], STDIN_FILENO); // redirect pipe read to stdin
        }

        execl("/bin/bash", "bash", "-c", command, (char *)NULL); // won't return
        return NULL; // will never be hit; just for static analyzers
    } else {
        // parent
        setpgid(childPid, childPid); // give the child and descendants their own pgid so we can terminate gcore separately
        *pid = childPid;

        if (type[0] == 'r') {
            close(pipefd[1]);
            return fdopen(pipefd[0], "r");
        } else {
            close(pipefd[0]);
            return fdopen(pipefd[1], "w");
        }

    }
}

//--------------------------------------------------------------------
//
// sanitize - Helper function for removing all non-alphanumeric characters from process name
//
// Returns: char *
//
//--------------------------------------------------------------------
// remove all non alphanumeric characters from process name and replace with '_'
char *sanitize(char * processName)
{
    if(processName == NULL){
        Log(error, "NULL process name.\n");
        exit(-1);
    }

    char *sanitizedProcessName = strdup(processName);
    for (int i = 0; i < strlen(sanitizedProcessName); i++)
    {
        if (!isalnum(sanitizedProcessName[i]))
        {
            sanitizedProcessName[i] = '_';
        }
    }
    return sanitizedProcessName;
}
