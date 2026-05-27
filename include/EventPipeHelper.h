// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License

//--------------------------------------------------------------------
//
// EventPipe helper for .NET performance counter monitoring
//
//--------------------------------------------------------------------

#ifndef EVENTPIPEHELPER_H
#define EVENTPIPEHELPER_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <pthread.h>

// Forward declaration
struct ProcDumpConfiguration;

//
// Represents a single counter value received from EventPipe
//
struct EventPipeCounterValue
{
    char providerName[256];
    char counterName[256];
    double value;           // Mean for snapshot counters, Increment for rate counters
    char counterType[32];   // "Mean", "Sum", "Histogram", "Gauge", etc.
    char quantiles[512];    // For Histogram: raw quantile string "0.5=1.23;0.95=2.34;0.99=5.67"
};

//
// Callback invoked for each counter value received from the EventPipe stream.
// Return false to stop the session.
//
typedef bool (*EventPipeCounterCallback)(struct EventPipeCounterValue* counterValue, void* context);

//
// Start an EventPipe session to monitor counters and invoke the callback
// for each counter value received. This function blocks until the session
// is stopped (via StopEventPipeSession) or an error occurs.
//
// socketName: path to the .NET diagnostics Unix domain socket
// providers: array of provider names to subscribe to
// providerCount: number of providers
// intervalSeconds: polling interval for EventCounterIntervalSec
// callback: function called for each counter value
// context: user context passed to callback
// sessionId: [out] the session ID for stopping
//
// Returns: 0 on success, -1 on error
//
int StartEventPipeCounterSession(
    const char* socketName,
    const char** providers,
    int providerCount,
    int intervalSeconds,
    EventPipeCounterCallback callback,
    void* context,
    uint64_t* sessionId,
    int* sessionFd,
    pthread_mutex_t* sessionFdMutex);

//
// Stop an EventPipe session. Must be called from a different thread
// than the one blocked in StartEventPipeCounterSession.
//
// socketName: path to the diagnostics socket (new connection)
// sessionId: the session ID returned from StartEventPipeCounterSession
//
// Returns: 0 on success, -1 on error
//
int StopEventPipeSession(const char* socketName, uint64_t sessionId);

#endif // EVENTPIPEHELPER_H
