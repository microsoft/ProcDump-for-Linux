// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License

//--------------------------------------------------------------------
//
// Profiler Common
//
//--------------------------------------------------------------------
#ifndef PROFILERCOMMON_H
#define PROFILERCOMMON_H

#define CUMULATIVE_GC_SIZE  2008
#define MAX_GC_GEN  2

enum TriggerType
{
    Processor,
    Commit,
    Timer,
    Signal,
    ThreadCount,
    FileDescriptorCount,
    Exception,
    GCThreshold,
    GCGeneration,
    Restrack,
    Manual,
};

#endif // PROFILERCOMMON_H