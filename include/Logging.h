// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License

//--------------------------------------------------------------------
//
// A simple logging library for log generation and debugging
//
//--------------------------------------------------------------------

#ifndef LOGGING_H
#define LOGGING_H

#include <syslog.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#define INTERNAL_ERROR "Internal Error has occurred. If problem continues to occur run procdump with -d flag to trace issue"

// double-macro-stringify to expand __FILE__ and __LINE__ properly when they are injected in files
#define S1(x) #x
#define S2(x) S1(x)
#define LOCATION "in "__FILE__ ", at line " S2(__LINE__)

enum LogLevel{
    debug,
    info,   // standard output
    warn,
    crit,
    error
};

void Log(enum LogLevel logLevel, const char *message, ...);


void DiagTrace(const char* message, ...);

/*
 * Summary: Used similarly to printf, but requires a format string for all input.  
 *          This macro appends line number and file information at the end of the format string and va_args.
 * Params:
 * - format: printf style format string literal
 * - var_args: variable number of format args
 * Example: Trace("%s", strerror(errno)) // %s format specifier required.
 */
#define Trace(format, ...) \
    DiagTrace(format " %s", ##__VA_ARGS__, LOCATION);

#endif // LOGGING_H