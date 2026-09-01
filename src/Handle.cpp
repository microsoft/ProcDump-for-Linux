// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License

//--------------------------------------------------------------------
//
// Generalization of Events and Semaphores (Critical Sections)
//
//--------------------------------------------------------------------
#include "Includes.h"

//--------------------------------------------------------------------
//
// WaitForSingleObject - Blocks the current thread until
//      either the event triggers, semaphore > 0,
//       or the wait time has passed
//
// Parameters:
//      -Handle -> the event/semaphore to wait for
//      -Milliseconds -> the time to wait (in milliseconds).
//          '-1' will mean infinite, and 0 will be instant check
//
// Return - An integer indicating state of wait
//      0 -> successful wait, and trigger fired
//      ETIMEDOUT -> the wait timed out (based on sepcified milliseconds)
//      other non-zero -> check errno.h
//
//--------------------------------------------------------------------
int WaitForSingleObject(struct Handle *Handle, int Milliseconds)
{
    struct timespec ts;
    int rc = 0;

    // Get current time and add wait time
    if (Milliseconds != INFINITE_WAIT)
    { // We aren't waiting infinitely
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += Milliseconds / 1000;              // ms->sec
        ts.tv_nsec += (Milliseconds % 1000) * 1000000; // remaining ms->ns
        if (ts.tv_nsec >= 1000000000)
        {
            ts.tv_sec += ts.tv_nsec / 1000000000;
            ts.tv_nsec %= 1000000000;
        }
    }

    switch (Handle->type) {
    case EVENT:
        if ((rc = pthread_mutex_lock(&(Handle->event.mutex))) == 0)
        {
            Handle->event.nWaiters++;
            while (!Handle->event.bTriggered && rc == 0)
            {
                rc = (Milliseconds == INFINITE_WAIT) ? // either wait
                    pthread_cond_wait(&(Handle->event.cond), &(Handle->event.mutex)) :  // infinitely
                    pthread_cond_timedwait(&(Handle->event.cond), &(Handle->event.mutex), &ts); // or till specified time passes
            }
            Handle->event.nWaiters--;

            // Check if we should reset
            if (Handle->event.nWaiters == 0 && !Handle->event.bManualReset)
            {
                Handle->event.bTriggered = false;
            }
            pthread_mutex_unlock(&(Handle->event.mutex));
        }


        break;

    case SEMAPHORE:
        if(Milliseconds == INFINITE_WAIT)
        {
            do
            {
                rc = sem_wait(Handle->semaphore);
            } while(rc == -1 && errno == EINTR);

            if(rc != 0)
            {
                rc = errno;
            }
        }
        else
        {
#ifdef __linux__
            do
            {
                rc = sem_timedwait(Handle->semaphore, &ts);
            } while(rc == -1 && errno == EINTR);

            if(rc != 0)
            {
                rc = errno;
            }
#elif __APPLE__
            struct timespec now, sleep_time;    
            while(1)
            {
                if (sem_trywait(Handle->semaphore) == 0) 
                {
                    return 0; // Successfully acquired the semaphore
                }                

                clock_gettime(CLOCK_REALTIME, &now);

                // Check if the timeout has expired
                if ((now.tv_sec > ts.tv_sec) ||
                    (now.tv_sec == ts.tv_sec && now.tv_nsec >= ts.tv_nsec)) 
                {
                    rc = ETIMEDOUT;
                    break;
                }

                // Calculate the time to sleep
                sleep_time.tv_sec = 0;
                sleep_time.tv_nsec = 1000000; // 1 millisecond
                nanosleep(&sleep_time, NULL);
            }
#endif
        }

        break;

    default:
        rc = -1;
        break;
    }

    return rc;
}

// Helper functions/infrastructure for WaitForMultipleObjects
struct thread_result {
    int retVal;
    int threadIndex;
};

struct coordinator {
    pthread_cond_t condEventTriggered;
    pthread_mutex_t mutexEventTriggered;
    struct thread_result *results;
    int numberTriggered; // behind mutex
    int stopIssued; // when != 0, proceed to cleanup
    struct Handle evtStartWaiting;
};

struct thread_args {
    struct Handle *handle;
    struct coordinator *coordinator;
    int retVal;
    int threadIndex;
};

static const int MULTIPLE_WAIT_POLL_INTERVAL_MILLISECONDS = 1000;

static bool IsStopIssued(struct coordinator *coordinator)
{
    pthread_mutex_lock(&coordinator->mutexEventTriggered);
    bool stopIssued = coordinator->stopIssued != 0;
    pthread_mutex_unlock(&coordinator->mutexEventTriggered);
    return stopIssued;
}

void *WaiterThread(void *thread_args)
{
    int rc;
    struct thread_args *input = (struct thread_args *)thread_args;

    // Wait for go signal
    if ((rc = WaitForSingleObject(&(input->coordinator->evtStartWaiting), 2000)) != WAIT_OBJECT_0) {
        // we messed up and the thread can't start...
    }

    // Poll in bounded intervals so a losing waiter can observe the stop request.
    do {
        if (IsStopIssued(input->coordinator)) {
            rc = ETIMEDOUT;
            break;
        }

        rc = WaitForSingleObject(input->handle, MULTIPLE_WAIT_POLL_INTERVAL_MILLISECONDS);
    } while (rc == ETIMEDOUT);


    pthread_mutex_lock(&input->coordinator->mutexEventTriggered);
    struct thread_result result = { .retVal = rc, .threadIndex = input->threadIndex };
    input->coordinator->results[input->coordinator->numberTriggered++] = result;
    pthread_mutex_unlock(&input->coordinator->mutexEventTriggered);
    pthread_cond_signal(&input->coordinator->condEventTriggered);

    free(input);
    return NULL;
}

//--------------------------------------------------------------------
//
// WaitForMultipleObjects - Blocks the current thread and waits for multiple Events
//
// Parameters:
//      -Count -> The number of Events
//      -Events -> An array of pointers to Events
//      -WaitAll -> Should we wait for all the events or whatever comes back first
//      -Milliseconds -> the number of milliseconds to wait, -1 is infinite
//
// Return - An integer indicating state of wait:
//      WAIT_OBJECT_0 to (WAIT_OBJECT_0 + Count-1) -> successful wait, and trigger fired
//              if WaitAll is TRUE: indicates all objects signaled
//              if WaitAll is FALSE: returns the index of the event that satisfied the wait *first*
//      ETIMEDOUT -> the wait timed out (based on sepcified milliseconds)
//      other non-zero -> check errno.h
//
//--------------------------------------------------------------------
int WaitForMultipleObjects(int Count, struct Handle **Handles, bool WaitAll, int Milliseconds)
{
    struct coordinator *coordinator;
    struct thread_result *results;

    pthread_t *threads;
    struct thread_args **thread_args;

    struct timespec ts;

    int t = 0;
    int rc = -1;
    int retVal = -1;

    threads = (pthread_t *)malloc(sizeof(pthread_t) * Count);
    if(threads==NULL)
    {
        Log(error, INTERNAL_ERROR);
        Trace("ERROR: Failed to malloc in %s\n",__FILE__);
        exit(-1);
    }

    thread_args = (struct thread_args **)malloc(sizeof(struct thread_args *) * Count);
    if(thread_args==NULL)
    {
        Log(error, INTERNAL_ERROR);
        Trace("ERROR: Failed to malloc in %s\n",__FILE__);
        exit(-1);
    }

    coordinator = (struct coordinator *)malloc(sizeof(struct coordinator));
    if (coordinator == NULL) {
        Log(error, INTERNAL_ERROR);
        Trace("ERROR: Failed to malloc in %s\n",__FILE__);
        exit(-1);
    }

    coordinator->numberTriggered = 0;
    coordinator->stopIssued = 0;

    coordinator->evtStartWaiting.type = EVENT;
    InitNamedEvent(&(coordinator->evtStartWaiting.event), true, false, const_cast<char*> ("StartWaiting"));
    pthread_cond_init(&coordinator->condEventTriggered, NULL);
    pthread_mutex_init(&coordinator->mutexEventTriggered, NULL);

    results = coordinator->results = (struct thread_result *)malloc(sizeof(struct thread_result) * Count);

    // Get current time and add wait time
    if (Milliseconds != -1) { // We aren't waiting infinitely
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += Milliseconds / 1000;              // ms->sec
        ts.tv_nsec += (Milliseconds % 1000) * 1000000;  // remaining ms->ns
    }

    // Create our threads
    pthread_mutex_lock(&coordinator->mutexEventTriggered);
    for (t = 0; t < Count; t++) {
        thread_args[t] = (struct thread_args *)malloc(sizeof(struct thread_args));
        if (thread_args[t] == NULL) {
            printf("ERROR: Failed to alloc in %s\n",__FILE__);
            exit(-1);
        }
        thread_args[t]->handle = Handles[t];
        thread_args[t]->threadIndex = t;
        thread_args[t]->coordinator = coordinator;
        rc = pthread_create(&threads[t], NULL, WaiterThread, (void *)thread_args[t]);
        if (rc) {
            Log(error, INTERNAL_ERROR);
            Trace("ERROR: pthread_create failed in %s with error %d\n",__FILE__,rc);
            exit(-1);
        }
    }

    SetEvent(&(coordinator->evtStartWaiting.event));

    // listen to our threads in no particular order
    while (((WaitAll && coordinator->numberTriggered < Count) ||
           (!WaitAll && coordinator->numberTriggered == 0)) &&
           rc == 0) {
        if (Milliseconds == INFINITE_WAIT) {
            if ((rc = pthread_cond_wait(&coordinator->condEventTriggered, &coordinator->mutexEventTriggered)) != 0) {
                break; // we either errored or timed out, go cleanup
            }
        } else {
            if ((rc = pthread_cond_timedwait(&coordinator->condEventTriggered, &coordinator->mutexEventTriggered, &ts)) != 0) {
                break; // we either errored or timed out, go cleanup
            }
        }
        // A handle fired.  Check if we need to kep listening or head to return
    }


    coordinator->stopIssued = 1;
    pthread_mutex_unlock(&coordinator->mutexEventTriggered);

    // Wait until no worker can access the caller-owned handles or coordinator.
    for (t = 0; t < Count; t++) {
        int joinRc = pthread_join(threads[t], NULL);
        if (joinRc != 0) {
            Log(error, INTERNAL_ERROR);
            Trace("ERROR: pthread_join failed in %s with error %d\n",__FILE__,joinRc);
            exit(-1);
        }
    }

    // rc will be non-zero if we timed/errored out
    // retVal will be <wait code> + threadIndex that fired first (e.g., WAIT_OBJECT_0 + 1, WAIT_ABANDONED + 2)
    if (rc) {
        retVal = rc;
    } else {
        retVal = (WaitAll) ? rc : results[0].retVal + results[0].threadIndex;
    }

    DestroyEvent(&(coordinator->evtStartWaiting.event));
    pthread_cond_destroy(&coordinator->condEventTriggered);
    pthread_mutex_destroy(&coordinator->mutexEventTriggered);
    free(results);
    free(coordinator);
    free(threads);
    free(thread_args);

    return retVal;
}
