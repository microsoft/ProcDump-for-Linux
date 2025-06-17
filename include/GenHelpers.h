// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License

//--------------------------------------------------------------------
//
// General purpose helpers
//
//--------------------------------------------------------------------

#ifndef GENHELPERS_H
#define GENHELPERS_H

#ifdef __linux__
#include <linux/version.h>
#elif __APPLE_
#endif

#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <sys/utsname.h>
#include <errno.h>
#include <stdio.h>
#include <sys/types.h>
#include <dirent.h>

//
// Minimum kernel version for ProcDump to run
//
#define MIN_KERNEL_VERSION 3
#define MIN_KERNEL_PATCH 5

//
// Minimum kernel version for ProcDump restrack to run
//
#define MIN_RESTRACK_KERNEL_VERSION 4
#define MIN_RESTRACK_KERNEL_PATCH 18


//-------------------------------------------------------------------------------------
// Auto clean up of memory using free (void)
//-------------------------------------------------------------------------------------
static inline void cleanup_void(void* val)
{
    void **ppVal = (void**)val;
    free(*ppVal);
}

//-------------------------------------------------------------------------------------
// Auto clean up of file descriptors using close
//-------------------------------------------------------------------------------------
#if (__GNUC__ >= 13)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"
#endif
static inline void cleanup_fd(int* val)
{
    if (*val)
    {
        close(*val);
    }
    else
    {
        // Make static analyzer happy, otherwise it thinks we leak when *val == 0
    }
}
#if (__GNUC__ >= 13)
#pragma GCC diagnostic pop
#endif

//-------------------------------------------------------------------------------------
// Auto clean up of dir using closedir
//-------------------------------------------------------------------------------------
static inline void cleanup_dir(DIR** val)
{
    if(*val)
    {
        closedir(*val);
    }
}

//-------------------------------------------------------------------------------------
// Auto clean up of FILE using fclose
//-------------------------------------------------------------------------------------
static inline void cleanup_file(FILE** val)
{
    if(*val)
    {
        fclose(*val);
    }
}

//-------------------------------------------------------------------------------------
// Auto cancel pthread
//-------------------------------------------------------------------------------------
static inline void cancel_pthread(unsigned long* val)
{
    if(*val!=-1)
    {
        #ifdef __linux__
        pthread_cancel(*val);
        #endif
    }
}

#define auto_free __attribute__ ((__cleanup__(cleanup_void)))
#define auto_free_fd __attribute__ ((__cleanup__(cleanup_fd)))
#define auto_free_dir __attribute__ ((__cleanup__(cleanup_dir)))
#define auto_free_file __attribute__ ((__cleanup__(cleanup_file)))
#define auto_cancel_thread __attribute__ ((__cleanup__(cancel_pthread)))

int* GetSeparatedValues(char* src, char* separator, int* numValues);
bool ConvertToInt(const char* src, int* conv);
bool ConvertToIntHex(const char* src, int* conv);
bool IsValidNumberArg(const char *arg);
bool CheckKernelVersion(int major, int minor);
uint16_t* GetUint16(char* buffer);
char* GetPath(char* lineBuf);
FILE *popen2(const char *command, const char *type, pid_t *pid);
char *sanitize(char *processName);
int StringToGuid(char* szGuid, struct CLSID* pGuid);
int GetHex(char* szStr, int size, void* pResult);
bool createDir(const char *dir, mode_t perms);
bool isBinaryOnPath(const char *binary);
char* GetSocketPath(char* prefix, pid_t pid, pid_t targetPid);
int send_all(int socket, void *buffer, size_t length);
int recv_all(int socket, void* buffer, size_t length);
pid_t gettid() noexcept;
unsigned long GetCoreDumpFilter(int pid);
bool SetCoreDumpFilter(int pid, unsigned long filter);

#endif // GENHELPERS_H

