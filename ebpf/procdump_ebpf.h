/*
    ProcDump for Linux

    Copyright (c) Microsoft Corporation

    All rights reserved.

    MIT License

    Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the ""Software""), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED *AS IS*, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#ifndef __PROCDUMP_EBPF_H__
#define __PROCDUMP_EBPF_H__

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/usdt.bpf.h>

#define USER_STACKID_FLAGS (0 | BPF_F_FAST_STACK_CMP | BPF_F_USER_STACK)
#define ARGS_HASH_SIZE 10240

#define BPF_PRINTK( format, ... ) \
    if(isLoggingEnabled == true) \
    { \
        char fmt[] = format; \
        bpf_trace_printk(fmt, sizeof(fmt), ##__VA_ARGS__ ); \
    }

//
// This is a hashmap to hold resource arguments (such as size) between alloc and free calls.
// It's shared by all cpus because alloc and free could be on different cpus.
struct argsStruct
{
    unsigned long size;
};

struct
{
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, ARGS_HASH_SIZE);
    __type(key, int);
    __type(value, struct ResourceInformation);
} argsHashMap SEC(".maps");


//
// Since stack space is minimal, we use this to store an event on the heap
//
struct
{
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 512);
	__type(key, int);
	__type(value, struct ResourceInformation);
} heapStorage SEC(".maps");

//
// The ring buffer we use to communicate with user space
//
struct
{
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 10 * 1024 * 1024 /* 10 MB */);
} ringBuffer SEC(".maps");

#endif // __PROCDUMP_EBPF_H__
