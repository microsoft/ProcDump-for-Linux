// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License

#include "Handle.h"

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

static const int SETUP_TIMEOUT_MILLISECONDS = 5000;
static const int FINITE_WAIT_MILLISECONDS = 30000;

struct WaitArguments
{
    struct Handle** handles;
    int milliseconds;
    int result;
};

static void* WaitForEvents(void* context)
{
    struct WaitArguments* arguments = static_cast<struct WaitArguments*>(context);
    arguments->result = WaitForMultipleObjects(2, arguments->handles, false, arguments->milliseconds);
    return NULL;
}

static int GetWaiterCount(struct Event* event)
{
    int rc = pthread_mutex_lock(&event->mutex);
    if(rc != 0)
    {
        fprintf(stderr, "pthread_mutex_lock failed: %d\n", rc);
        return -1;
    }

    int waiters = event->nWaiters;
    rc = pthread_mutex_unlock(&event->mutex);
    if(rc != 0)
    {
        fprintf(stderr, "pthread_mutex_unlock failed: %d\n", rc);
        return -1;
    }

    return waiters;
}

static bool WaitForWaiterCount(struct Event* event, int expected)
{
    for(int elapsed = 0; elapsed < SETUP_TIMEOUT_MILLISECONDS; elapsed++)
    {
        int waiters = GetWaiterCount(event);
        if(waiters == expected)
        {
            return true;
        }
        if(waiters < 0)
        {
            return false;
        }

        usleep(1000);
    }

    return false;
}

static bool RunCleanupCase(const char* name, int milliseconds)
{
    struct Handle losingEvent = {};
    losingEvent.type = EVENT;
    InitNamedEvent(&losingEvent.event, true, false, const_cast<char*>("LosingEvent"));

    struct Handle winningEvent = {};
    winningEvent.type = EVENT;
    InitNamedEvent(&winningEvent.event, true, false, const_cast<char*>("WinningEvent"));

    struct Handle* handles[] = { &losingEvent, &winningEvent };
    struct WaitArguments arguments = { handles, milliseconds, -1 };

    pthread_t waitingThread;
    int rc = pthread_create(&waitingThread, NULL, WaitForEvents, &arguments);
    if(rc != 0)
    {
        fprintf(stderr, "%s: pthread_create failed: %d\n", name, rc);
        DestroyEvent(&winningEvent.event);
        DestroyEvent(&losingEvent.event);
        return false;
    }

    bool bothWorkersWaiting = WaitForWaiterCount(&losingEvent.event, 1) &&
                              WaitForWaiterCount(&winningEvent.event, 1);
    if(!bothWorkersWaiting)
    {
        fprintf(stderr, "%s: waiter threads did not start within %d ms\n",
                name, SETUP_TIMEOUT_MILLISECONDS);
        SetEvent(&losingEvent.event);
        SetEvent(&winningEvent.event);
    }
    else
    {
        SetEvent(&winningEvent.event);
    }

    rc = pthread_join(waitingThread, NULL);
    if(rc != 0)
    {
        fprintf(stderr, "%s: pthread_join failed: %d\n", name, rc);
        return false;
    }

    int losingWaiters = GetWaiterCount(&losingEvent.event);
    bool passed = bothWorkersWaiting &&
                  arguments.result == WAIT_OBJECT_0 + 1 &&
                  losingWaiters == 0;

    if(arguments.result != WAIT_OBJECT_0 + 1)
    {
        fprintf(stderr, "%s: expected wait result %d, got %d\n",
                name, WAIT_OBJECT_0 + 1, arguments.result);
    }
    if(losingWaiters != 0)
    {
        fprintf(stderr,
                "%s: WaitForMultipleObjects returned with %d waiter(s) still using the losing event\n",
                name, losingWaiters);
    }

    if(losingWaiters > 0)
    {
        SetEvent(&losingEvent.event);
        if(!WaitForWaiterCount(&losingEvent.event, 0))
        {
            fprintf(stderr, "%s: losing waiter did not exit during test cleanup\n", name);
            return false;
        }
    }

    DestroyEvent(&winningEvent.event);
    DestroyEvent(&losingEvent.event);

    if(passed)
    {
        printf("%s: passed\n", name);
    }

    return passed;
}

int main()
{
    bool finiteWaitPassed = RunCleanupCase("finite wait", FINITE_WAIT_MILLISECONDS);
    bool infiniteWaitPassed = RunCleanupCase("infinite wait", INFINITE_WAIT);

    return finiteWaitPassed && infiniteWaitPassed ? 0 : 1;
}