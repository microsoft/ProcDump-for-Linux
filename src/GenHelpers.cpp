// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License

//--------------------------------------------------------------------
//
// General purpose helpers
//
//--------------------------------------------------------------------
#include "Includes.h"
#ifdef __linux__
#include <syscall.h>
#endif

//--------------------------------------------------------------------
//
// GetSeparatedValues -
// Returns a list of values separated by the specified separator.
//
//--------------------------------------------------------------------
int* GetSeparatedValues(char* src, char* separator, int* numValues)
{
    int* ret = NULL;
    int i = 0;

    if(src == NULL || numValues == NULL)
    {
        return NULL;
    }

    char* dup = strdup(src);        // Duplicate to avoid changing the original using strtok
    if(dup == NULL)
    {
        return NULL;
    }

    char* token = strtok((char*)dup, separator);
    while (token != NULL)
    {
        i++;
        token = strtok(NULL, separator);
    }

    free(dup);

    if(i > 0)
    {
        ret = (int*) malloc(i*sizeof(int));
        if(ret)
        {
            i = 0;
            dup = strdup(src);
            if(dup == NULL)
            {
                free(ret);
                ret = NULL;
                return NULL;
            }

            token = strtok((char*)dup, separator);
            while (token != NULL)
            {
                if(!ConvertToInt(token, &ret[i]))
                {
                    free(ret);
                    ret = NULL;
                    return NULL;
                }

                i++;
                token = strtok(NULL, separator);
            }

            free(dup);
        }
    }

    *numValues = i;
    return ret;
}


//--------------------------------------------------------------------
//
// ConvertToInt - Helper to convert from a char* to int
//
//--------------------------------------------------------------------
bool ConvertToInt(const char* src, int* conv)
{
    char *end;

    long l = strtol(src, &end, 10);
    if (*end != '\0')
        return false;

    *conv = l;
    return true;
}

//--------------------------------------------------------------------
//
// ConvertToIntHex - Helper to convert from a char* (hex) to int
//
//--------------------------------------------------------------------
bool ConvertToIntHex(const char* src, int* conv)
{
    int temp = 0;

    for (size_t i=0; src[i] != '\0'; i++)
    {
        if ((src[i] >= '0') && (src[i] <= '9'))
        {
            // Shift left by 0x10 (16) and add the digit using an ASCII delta
            temp *= 0x10;
            temp += src[i] - '0';
        }
        else if ((src[i] >= 'A') && (src[i] <= 'F'))
        {
            // Shift left by 0x10 (16) and add the digit using an ASCII delta
            temp *= 0x10;
            temp += 10 + (src[i] - 'A');
        }
        else if ((src[i] >= 'a') && (src[i] <= 'f'))
        {
            // Shift left by 0x10 (16) and add the digit using an ASCII delta
            temp *= 0x10;
            temp += 10 + (src[i] - 'a');
        }
        else
        {
            return false;
        }
    }

    *conv = temp;
    return true;
}

//--------------------------------------------------------------------
//
// CheckKernelVersion - Check to see if current kernel is greater than
// specified.
//
//--------------------------------------------------------------------
bool CheckKernelVersion(int major, int minor)
{
    struct utsname kernelInfo = {};
    if(uname(&kernelInfo) == 0)
    {
        int version, patch = 0;
        if(sscanf(kernelInfo.release,"%d.%d",&version,&patch) != 2)
        {
            return false;
        }

        if(version > major)
        {
            return true;
        }
        else if(version == major && patch >= minor)
        {
            return true;
        }
    }

    return false;
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
// GetUint16 - Quick and dirty conversion from char to uint16_t
//
// Returns: uint16_t*   - if successfully converted, NULL otherwise.
//                        Caller must free upon success
//
//--------------------------------------------------------------------
uint16_t* GetUint16(char* buffer)
{
    int len;
    uint16_t* dumpFileNameW = NULL;

    if(buffer!=NULL)
    {
        len = strlen(buffer) + 1;
        dumpFileNameW = (uint16_t*) malloc((len)*sizeof(uint16_t));
        if(dumpFileNameW==NULL)
        {
            return NULL;
        }

        for(int i=0; i<len; i++)
        {
            dumpFileNameW[i] = (uint16_t) buffer[i];
        }
    }

    return dumpFileNameW;
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
    int i = 7;
    char delim[] = " ";

    // example of /proc/net/unix line:
    // 0000000000000000: 00000003 00000000 00000000 0001 03 20287 @/tmp/.X11-unix/X0
    char *ptr = strtok(lineBuf, delim);

    // Move to last column which contains the name of the file (/socket)
    while (i--)
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
            dup2(pipefd[1], STDERR_FILENO); // redirect stderr to write end of pipe
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
#if (__GNUC__ >= 13)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"
#endif
}
#if (__GNUC__ >= 13)
#pragma GCC diagnostic pop
#endif


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
    if(sanitizedProcessName==NULL)
    {
        return NULL;
    }

    for (int i = 0; i < strlen(sanitizedProcessName); i++)
    {
        if (!isalnum(sanitizedProcessName[i]))
        {
            sanitizedProcessName[i] = '_';
        }
    }
    return sanitizedProcessName;
}

//--------------------------------------------------------------------
//
// StringToGuid
//
// Convert string representation of GUID to a GUID
//
//--------------------------------------------------------------------
int StringToGuid(char* szGuid, struct CLSID* pGuid)
{
    int i = 0;

    // Verify the surrounding syntax.
    if (strlen(szGuid) != 38 || szGuid[0] != '{' || szGuid[9] != '-' ||
        szGuid[14] != '-' || szGuid[19] != '-' || szGuid[24] != '-' || szGuid[37] != '}')
    {
        return -1;
    }

    // Parse the first 3 fields.
    if (GetHex(szGuid + 1, 4, &pGuid->Data1))
        return -1;
    if (GetHex(szGuid + 10, 2, &pGuid->Data2))
        return -1;
    if (GetHex(szGuid + 15, 2, &pGuid->Data3))
        return -1;

    // Get the last two fields (which are byte arrays).
    for (i = 0; i < 2; ++i)
    {
        if (GetHex(szGuid + 20 + (i * 2), 1, &pGuid->Data4[i]))
        {
            return -1;
        }
    }
    for (i=0; i < 6; ++i)
    {
        if (GetHex(szGuid + 25 + (i * 2), 1, &pGuid->Data4[i+2]))
        {
            return -1;
        }
    }
    return 0;
}

//--------------------------------------------------------------------
//
// GetHex
//
// Gets hex value of specified string
//
//--------------------------------------------------------------------
int GetHex(char* szStr, int size, void* pResult)
{
    int         count = size * 2;       // # of bytes to take from string.
    unsigned int Result = 0;           // Result value.
    char          ch;

    while (count-- && (ch = *szStr++) != '\0')
    {
        switch (ch)
        {
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
            Result = 16 * Result + (ch - '0');
            break;

            case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
            Result = 16 * Result + 10 + (ch - 'A');
            break;

            case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
            Result = 16 * Result + 10 + (ch - 'a');
            break;

            default:
            return -1;
        }
    }

    // Set the output.
    switch (size)
    {
        case 1:
        *((unsigned char*) pResult) = (unsigned char) Result;
        break;

        case 2:
        *((short*) pResult) = (short) Result;
        break;

        case 4:
        *((int*) pResult) = Result;
        break;

        default:
        break;
    }

    return 0;
}

//--------------------------------------------------------------------
//
// createDir
//
// Create specified directory with specified permissions.
//
//--------------------------------------------------------------------
bool createDir(const char *dir, mode_t perms)
{
    if (dir == NULL) {
        fprintf(stderr, "createDir invalid params\n");
        return false;
    }

    struct stat st;

    if (stat(dir, &st) < 0) {
        if (mkdir(dir, perms) < 0) {
            return false;
        }
    } else {
        if (!S_ISDIR(st.st_mode)) {
            return false;
        }
        chmod(dir, perms);
    }
    return true;
}

//--------------------------------------------------------------------
//
// isBinaryOnPath
//
// Checks whether a binary can be found in any of the directories from the PATH.
//
//--------------------------------------------------------------------
bool isBinaryOnPath(const char *binary){
    char binbuf[4096];
    struct stat statbuf;

    char *PATH = getenv("PATH");
    if(PATH == NULL)
    {
        return false;
    }

    char *path = strdup(PATH);
    if(path == NULL)
    {
        Log(error, INTERNAL_ERROR);
        Trace("isBinaryOnPath: failed to strdup PATH");
        return false;
    }

    char *directory = strtok(path, ":");
    while (directory != NULL)
    {
        snprintf(binbuf, sizeof(binbuf), "%s/%s", directory, binary);
        if (stat(binbuf, &statbuf) == 0 && S_ISREG(statbuf.st_mode))
        {
            free(path);
            return true;
        }
        
        directory = strtok(NULL, ":");
    }

    free(path);
    return false;
}

//--------------------------------------------------------------------
//
// GetSocketPath
//
//--------------------------------------------------------------------
char* GetSocketPath(char* prefix, pid_t pid, pid_t targetPid)
{
    char* prefixTmpFolder = NULL;
    char* t = NULL;

    // If $TMPDIR is set, use it as the path, otherwise we use /tmp
    prefixTmpFolder = getenv("TMPDIR");
    if(prefixTmpFolder==NULL)
    {
        if(targetPid)
        {
            int len = snprintf(NULL, 0, "/tmp/%s%d-%d", prefix, pid, targetPid);
            t = (char*) malloc(len+1);
            if(t==NULL)
            {
                return NULL;
            }

            snprintf(t, len+1, "/tmp/%s%d-%d", prefix, pid, targetPid);
        }
        else
        {
            int len = snprintf(NULL, 0, "/tmp/%s%d", prefix, pid);
            t = (char*) malloc(len+1);
            if(t==NULL)
            {
                return NULL;
            }

            snprintf(t, len+1, "/tmp/%s%d", prefix, pid);
        }
    }
    else
    {
        if(targetPid)
        {
            int len = snprintf(NULL, 0, "%s/%s%d-%d", prefixTmpFolder, prefix, pid, targetPid);
            t = (char*) malloc(len+1);
            if(t==NULL)
            {
                return NULL;
            }

            snprintf(t, len+1, "%s/%s%d-%d", prefixTmpFolder, prefix, pid, targetPid);
        }
        else
        {
            int len = snprintf(NULL, 0, "%s/%s%d", prefixTmpFolder, prefix, pid);
            t = (char*) malloc(len+1);
            if(t==NULL)
            {
                return NULL;
            }

            snprintf(t, len+1, "%s/%s%d", prefixTmpFolder, prefix, pid);
        }
    }

    return t;
}

//--------------------------------------------------------------------
//
// send_all
//
// Keeps sending data on socket until all data has been sent.
//
//--------------------------------------------------------------------
int send_all(int socket, void* buffer, size_t length)
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

//--------------------------------------------------------------------
//
// recv_all
//
// Keeps reading data on socket until all data has been read.
//
//--------------------------------------------------------------------
int recv_all(int socket, void* buffer, size_t length)
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

//--------------------------------------------------------------------
//
// gettid
//
// Returns the current thread ID. Useful to add in trace statements
// when threads are created to match to a debug session.
//
// Note: SYS_gettid is not POSIX compliant.
//
//--------------------------------------------------------------------
pid_t gettid() noexcept
{
#ifdef SYS_gettid
    return syscall(SYS_gettid);
#endif

    return 0;
}

//--------------------------------------------------------------------
//
// GetCoreDumpFilter
//
// Returns the core dump filter for the specified process id.
//--------------------------------------------------------------------
unsigned long GetCoreDumpFilter(int pid)
{
    unsigned long filter = -1;

    char filepath[PATH_MAX];
    snprintf(filepath, sizeof(filepath), "/proc/%d/coredump_filter", pid);

    FILE* file = fopen(filepath, "r");
    if (file != NULL)
    {
        int itemsRead = fscanf(file, "%lx", &filter);
        if (itemsRead != 1)
        {
            filter = -1;
        }
    }

    fclose(file);
    return filter;
}

//--------------------------------------------------------------------
//
// SetCoreDumpFilter
//
// Sets the core dump filter for the specified process id.
//--------------------------------------------------------------------
bool SetCoreDumpFilter(int pid, unsigned long filter)
{
    bool ret = false;
    char filepath[PATH_MAX];
    snprintf(filepath, sizeof(filepath), "/proc/%d/coredump_filter", pid);

    FILE *file = fopen(filepath, "w");
    if (file != NULL)
    {
        if(fprintf(file, "%ld", filter) > 0)
        {
            ret = true;
        }
    }

    fclose(file);
    return ret;
}
