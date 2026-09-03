/*
    ProcDump for Linux

    Copyright (c) Microsoft Corporation

    All rights reserved.

    MIT License

    Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the ""Software""), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED *AS IS*, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "procdump_ebpf.h"
#include "procdump_ebpf_common.h"

pid_t target_PID;
uint dev, inode;
int sampleRate;
bool isLoggingEnabled;

char LICENSE[] SEC("license") = "Dual BSD/GPL";

// ------------------------------------------------------------------------------------------
// GetFilterPidTgid
//
// Returns the PID and TID of the current process.
// ------------------------------------------------------------------------------------------
__attribute__((always_inline))
static inline bool GetFilterPidTgid(struct bpf_pidns_info* pidns)
{
    if(bpf_get_ns_current_pid_tgid(dev, inode, pidns, sizeof(*pidns)))
    {
        return false;
    }

    //
    // Only trace PIDs that matched the target PID and if we should sample this event.
    //
    if (pidns->tgid != target_PID)
    {
        return false;
    }

    return true;
}


// ------------------------------------------------------------------------------------------
// CheckSampleRate
//
// Returns true if we should sample this event.
// ------------------------------------------------------------------------------------------
__attribute__((always_inline))
static inline bool CheckSampleRate()
{
    int key = 0;
    int* currentSampleCount;
    if(sampleRate <= 1)
    {
        return true;
    }
    currentSampleCount = bpf_map_lookup_elem(&sampleCounts, &key);
    if(currentSampleCount == NULL)
    {
        return false;
    }
    (*currentSampleCount)++;
    if(*currentSampleCount >= sampleRate)
    {
        *currentSampleCount = 0;
        return true;
    }
    return false;
}

__attribute__((always_inline))
static inline void DiscardEvent(struct bpf_pidns_info* pidns)
{
    bpf_map_delete_elem(&argsHashMap, &pidns->pid);
}

__attribute__((always_inline))
static inline void RecordLostEvent()
{
    int key = 0;
    __u64* lost = bpf_map_lookup_elem(&eventStats, &key);
    if(lost != NULL)
    {
        __sync_fetch_and_add(lost, 1);
    }
}

__attribute__((always_inline))
static inline bool CheckedArraySize(unsigned long size, unsigned long count, unsigned long* total)
{
    unsigned long low = (size & 0xffffffffUL) * count;
    unsigned long high = (size >> 32) * count;
    if(high > 0xffffffffUL)
    {
        return false;
    }
    unsigned long shifted = high << 32;
    *total = low + shifted;
    return *total >= low;
}

// ------------------------------------------------------------------------------------------
// ZeroMemory
//
// We could just simply use memset but string.h is not working correctly due to needing
// the 32bit version of stubs.h (stubs-32.h). For some reason targeting bpf undefines __x86_64__.
// We can work around this on most architectures (they all have 32bit version) but not on
// mariner which does not offer it (for now).
// ------------------------------------------------------------------------------------------
__attribute__((always_inline))
static inline void ZeroMemory(char* ptr, unsigned int size)
{
    for(int i = 0; i < size; i++)
    {
        ptr[i] = 0;
    }
}

// ------------------------------------------------------------------------------------------
// SendEvent
//
// Sends the allocation/free event
// ------------------------------------------------------------------------------------------
__attribute__((always_inline))
static inline int SendEvent(void* alloc, bool freeOp, struct bpf_pidns_info* pidns)
{
    struct ResourceInformation* event = NULL;
    int ret = 1;

    //
    // Only trace PIDs that matched the target PID and have non NULL allocations
    //
    //
    // Get event
    //
    event = (struct ResourceInformation*) bpf_map_lookup_elem(&argsHashMap, &pidns->pid);
    if (event == NULL)
    {
        BPF_PRINTK("   [SendEvent] Failed: Getting event (allocation address: 0x%lx, target PID: %d)", alloc, target_PID);
        return 1;
    }

    if (freeOp == false && alloc == NULL)
    {
        if(event->previousAddress != 0 && event->allocSize == 0)
        {
            event->resourceType = RESTRACK_FREE;
            event->allocAddress = event->previousAddress;
            event->previousAddress = 0;
            event->callStackLen = 0;
            bpf_ringbuf_output(&ringBuffer, event, sizeof(*event), 0);
        }
        DiscardEvent(pidns);
        return 0;
    }

    //
    // Set the allocation address if it's an allocation operation
    //
    if(freeOp == false)
    {
        event->allocAddress = (unsigned long) alloc;
        BPF_PRINTK("   [SendEvent] Allocation size:0x%lx", event->allocSize);
    }

    {BPF_PRINTK("   [SendEvent] Allocation :0x%lx", event->allocAddress);}

    //
    // Send the event to the ring buffer (user land)
    //
    if((ret = bpf_ringbuf_output(&ringBuffer, event, sizeof(*event), 0)) != 0)
    {
        BPF_PRINTK("   [SendEvent] Failed: Getting event (type: %d, allocation address: 0x%lx, target PID: %d)", event->resourceType, event->allocAddress, target_PID);
        RecordLostEvent();
        DiscardEvent(pidns);
        return ret;
    }


    if(freeOp == false && event->previousAddress != 0 &&
       event->previousAddress != (unsigned long)alloc)
    {
        unsigned long newAddress = event->allocAddress;
        event->resourceType = RESTRACK_FREE;
        event->allocAddress = event->previousAddress;
        event->previousAddress = 0;
        event->allocSize = 0;
        event->callStackLen = 0;
        if(bpf_ringbuf_output(&ringBuffer, event, sizeof(*event), 0) != 0)
        {
            RecordLostEvent();
        }
        event->allocAddress = newAddress;
    }

    {BPF_PRINTK("   [SendEvent] Deleting event for %ld", pidns->pid);}
    if((ret = bpf_map_delete_elem(&argsHashMap, &pidns->pid)) != 0)
    {
        BPF_PRINTK("   [SendEvent] Failed: Deleting event (type: %d, allocation address: 0x%lx, target PID: %d)", event->resourceType, event->allocAddress, target_PID);
        return ret;
    }

    BPF_PRINTK("   [SendEvent] Success: (type: %d, allocation address: 0x%lx, target PID: %d)", event->resourceType, event->allocAddress, target_PID);
    return 0;
}

// ------------------------------------------------------------------------------------------
// ResourceFreeHelper
//
// Helper for all the intercepted free functions.
// ------------------------------------------------------------------------------------------
__attribute__((always_inline))
static inline int ResourceFreeHelper(void* alloc, struct bpf_pidns_info* pidns)
{
    struct ResourceInformation* event = NULL;
    uint32_t map_id = 0;

    //
    // Get heap element from the map
    //
    event = bpf_map_lookup_elem(&heapStorage, &map_id);
    if (event == NULL)
    {
        BPF_PRINTK("   [ResourceFreeHelper] Failed: Getting event (allocation: 0x%lx, target PID: %d)", alloc, target_PID);
        return 1;
    }

    ZeroMemory((char*) event, sizeof(struct ResourceInformation));

    //
    // Setup the event we want to transfer to usermode.
    //
    event->pid = target_PID;
    event->resourceType = RESTRACK_FREE;
    event->allocAddress = (unsigned long) alloc;

    //
    // Update the arguments hashmap with the entry. We'll fetch the entry when
    // we free the allocation and update other fields (such as allocation ptr) and then
    // send to user mode.
    //
    if (bpf_map_update_elem(&argsHashMap, &pidns->pid, event, BPF_ANY) != 0)
    {
        BPF_PRINTK("   [ResourceFreeHelper] Failed: Updating event (allocation: 0x%lx, target PID: %d)", alloc, target_PID);
        return 1;
    }

    BPF_PRINTK("   [ResourceFreeHelper] Success: (allocation: 0x%lx, target PID: %d)", alloc, target_PID);
    return 0;
}


// ------------------------------------------------------------------------------------------
// ResourceAllocHelper
//
// Helper for all the intercepted allocation functions.
// ------------------------------------------------------------------------------------------
__attribute__((always_inline))
static inline int ResourceAllocHelper(unsigned long size, struct pt_regs *ctx, struct bpf_pidns_info* pidns)
{
    struct ResourceInformation* event = NULL;
    uint32_t map_id = 0;

    //
    // Only trace if we should sample this event.
    //
    if (CheckSampleRate() == false)
    {
        return 0;
    }

    //
    // Get heap element from the map
    //
    event = bpf_map_lookup_elem(&heapStorage, &map_id);
    if (event != NULL)
    {
        ZeroMemory((char*) event, sizeof(struct ResourceInformation));
    }
    else
    {
        BPF_PRINTK("   [ResourceAllocHelper] Failed: Getting event (allocation size: 0x%lx, target PID: %d)", size, target_PID);
        return 1;
    }

    //
    // Setup the event we want to transfer to usermode.
    //
    event->allocSize = size;
    event->pid = target_PID;
    event->resourceType = RESTRACK_ALLOC;
    event->allocAddress = 0;
    event->callStackLen = bpf_get_stack(ctx, event->stackTrace, sizeof(event->stackTrace), BPF_F_USER_STACK) / sizeof(__u64);

    //
    // Update the arguments hashmap with the entry. We'll fetch the entry when
    // we exit the malloc and update other fields (such as allocation ptr) and then
    // send to user mode.
    //
    if (bpf_map_update_elem(&argsHashMap, &pidns->pid, event, BPF_ANY) != 0)
    {
        BPF_PRINTK("   [ResourceAllocHelper] Failed: Updating event (allocation size: 0x%lx, target PID: %d)", size, target_PID);
        return 1;
    }

    BPF_PRINTK("   [ResourceAllocHelper] Success: (allocation size: 0x%lx, target PID: %d)", size, target_PID);
    return 0;
}

// ------------------------------------------------------------------------------------------
// sys_mmap_enter
// ------------------------------------------------------------------------------------------
SEC("uprobe/libc.so.6:mmap")
int sys_mmap_enter(struct pt_regs *ctx)
{
    struct bpf_pidns_info pidns = {};
    if(GetFilterPidTgid(&pidns) == false)
    {
        return 0;
    }

    {BPF_PRINTK("[***** sys_mmap_enter, pid: %ld, tgid: %ld, size: %ld]", pidns.pid, pidns.tgid, (unsigned long) PT_REGS_PARM2(ctx));}
    ResourceAllocHelper((unsigned long) PT_REGS_PARM2(ctx), ctx, &pidns);
    return 0;
}

// ------------------------------------------------------------------------------------------
// sys_mmap_exit
// ------------------------------------------------------------------------------------------
SEC("uretprobe/libc.so.6:mmap")
int sys_mmap_exit(struct pt_regs *ctx)
{
    struct bpf_pidns_info pidns = {};
    if(GetFilterPidTgid(&pidns) == false)
    {
        return 0;
    }

    {BPF_PRINTK("[***** sys_mmap_exit, pid: %ld, tgid: %ld]", pidns.pid, pidns.tgid);}
    long result = (long)PT_REGS_RC(ctx);
    if(result == -1)
    {
        DiscardEvent(&pidns);
    }
    else
    {
        SendEvent((void*)result, false, &pidns);
    }
    return 0;
}

// ------------------------------------------------------------------------------------------
// sys_munmap_enter
// ------------------------------------------------------------------------------------------
SEC("uprobe/libc.so.6:munmap")
int sys_munmap_enter(struct pt_regs *ctx)
{
    struct bpf_pidns_info pidns = {};
    if(GetFilterPidTgid(&pidns) == false)
    {
        return 0;
    }

    {BPF_PRINTK("[***** sys_munmap_enter, pid: %ld, tgid: %ld]", pidns.pid, pidns.tgid);}
    ResourceFreeHelper((void*) PT_REGS_PARM1(ctx), &pidns);
    return 0;
}

// ------------------------------------------------------------------------------------------
// sys_munmap_exit
// ------------------------------------------------------------------------------------------
SEC("uretprobe/libc.so.6:munmap")
int sys_munmap_exit(struct pt_regs *ctx)
{
    struct bpf_pidns_info pidns = {};
    if(GetFilterPidTgid(&pidns) == false)
    {
        return 0;
    }

    {BPF_PRINTK("[***** sys_munmap_exit, pid: %ld, tgid: %ld]", pidns.pid, pidns.tgid);}
    if((long)PT_REGS_RC(ctx) == 0)
    {
        SendEvent(NULL, true, &pidns);
    }
    else
    {
        DiscardEvent(&pidns);
    }
    return 0;
}

// ------------------------------------------------------------------------------------------
// uprobe_malloc
// ------------------------------------------------------------------------------------------
SEC("uprobe/libc.so.6:malloc")
int BPF_KPROBE(uprobe_malloc, unsigned long size)
{
    struct bpf_pidns_info pidns = {};
    if(GetFilterPidTgid(&pidns) == false)
    {
        return 0;
    }

    {BPF_PRINTK("[***** malloc_enter, pid:%ld, tgid: %ld, size: %ld]", pidns.pid, pidns.tgid, size);}
    ResourceAllocHelper(size, ctx, &pidns);
    return 0;
}

// ------------------------------------------------------------------------------------------
// uretprobe_malloc
// ------------------------------------------------------------------------------------------
SEC("uretprobe/libc.so.6:malloc")
int BPF_KRETPROBE(uretprobe_malloc, void* ret)
{
    struct bpf_pidns_info pidns = {};
    if(GetFilterPidTgid(&pidns) == false)
    {
        return 0;
    }

    {BPF_PRINTK("[***** malloc_exit, pid: %ld, tgid: %ld]", pidns.pid, pidns.tgid);}
    SendEvent(ret, false, &pidns);
    return 0;
}

// ------------------------------------------------------------------------------------------
// uprobe_free
// ------------------------------------------------------------------------------------------
SEC("uprobe/libc.so.6:free")
int BPF_KPROBE(uprobe_free, void* alloc)
{
    struct bpf_pidns_info pidns = {};
    if(GetFilterPidTgid(&pidns) == false)
    {
        return 0;
    }

    {BPF_PRINTK("[***** free_enter, pid: %ld, tgid: %ld]", pidns.pid, pidns.tgid);}
    ResourceFreeHelper(alloc, &pidns);
    return 0;
}


// ------------------------------------------------------------------------------------------
// uretprobe_free
// ------------------------------------------------------------------------------------------
SEC("uretprobe/libc.so.6:free")
int BPF_KRETPROBE(uretprobe_free, void* ret)
{
    struct bpf_pidns_info pidns = {};
    if(GetFilterPidTgid(&pidns) == false)
    {
        return 0;
    }

    {BPF_PRINTK("[***** free_exit, pid: %ld, tgid: %ld]", pidns.pid, pidns.tgid);}
    SendEvent(NULL, true, &pidns);
    return 0;
}

// ------------------------------------------------------------------------------------------
// uprobe_cmalloc
//
// This is the entry point for the calloc uprobe. It's called when calloc is called.
// ------------------------------------------------------------------------------------------
SEC("uprobe/libc.so.6:calloc")
int BPF_KPROBE(uprobe_calloc, int count, unsigned long size)
{
    struct bpf_pidns_info pidns = {};
    if(GetFilterPidTgid(&pidns) == false)
    {
        return 0;
    }

    {BPF_PRINTK("[***** calloc_enter, pid: %ld, tgid: %ld, size: %ld]", pidns.pid, pidns.tgid, size*count);}
    unsigned long totalSize = 0;
    if(count < 0 || CheckedArraySize(size, (unsigned long)count, &totalSize) == false)
    {
        return 0;
    }
    ResourceAllocHelper(totalSize, ctx, &pidns);
    return 0;
}

// ------------------------------------------------------------------------------------------
// uretprobe_cmalloc
//
// This is the entry point for the calloc exit uprobe. It's called when calloc is exiting.
// ------------------------------------------------------------------------------------------
SEC("uretprobe/libc.so.6:calloc")
int BPF_KRETPROBE(uretprobe_calloc, void* ret)
{
    struct bpf_pidns_info pidns = {};
    if(GetFilterPidTgid(&pidns) == false)
    {
        return 0;
    }

    {BPF_PRINTK("[***** calloc_exit, pid: %ld, tgid: %ld]", pidns.pid, pidns.tgid);}
    SendEvent(ret, false, &pidns);
    return 0;
}

// ------------------------------------------------------------------------------------------
// uprobe_realloc
//
// This is the entry point for the realloc uprobe. It's called when realloc is called.
// ------------------------------------------------------------------------------------------
SEC("uprobe/libc.so.6:realloc")
int BPF_KPROBE(uprobe_realloc, void* ptr, unsigned long size)
{
    struct bpf_pidns_info pidns = {};
    if(GetFilterPidTgid(&pidns) == false)
    {
        return 0;
    }

    {BPF_PRINTK("[***** realloc_enter, pid:%ld, tgid: %ld, size:%ld]", pidns.pid, pidns.tgid, size);}
    ResourceAllocHelper(size, ctx, &pidns);
    struct ResourceInformation* event = bpf_map_lookup_elem(&argsHashMap, &pidns.pid);
    if(event != NULL)
    {
        event->previousAddress = (unsigned long)ptr;
    }
    return 0;
}

// ------------------------------------------------------------------------------------------
// uretprobe_remalloc
//
// This is the entry point for the realloc exit uprobe. It's called when realloc is exiting.
// ------------------------------------------------------------------------------------------
SEC("uretprobe/libc.so.6:realloc")
int BPF_KRETPROBE(uretprobe_realloc, void* ret)
{
    struct bpf_pidns_info pidns = {};
    if(GetFilterPidTgid(&pidns) == false)
    {
        return 0;
    }

    {BPF_PRINTK("[***** realloc_exit, pid: %ld, tgid: %ld]", pidns.pid, pidns.tgid);}
    SendEvent(ret, false, &pidns);
    return 0;
}


// ------------------------------------------------------------------------------------------
// uprobe_realloc
//
// This is the entry point for the reallocarray uprobe. It's called when reallocarray is called.
// ------------------------------------------------------------------------------------------
SEC("uprobe/libc.so.6:reallocarray")
int BPF_KPROBE(uprobe_reallocarray, void* ptr, long count, unsigned long size)
{
    struct bpf_pidns_info pidns = {};
    if(GetFilterPidTgid(&pidns) == false)
    {
        return 0;
    }

    {BPF_PRINTK("[***** reallocarray_enter, pid: %ld, tgid: %ld, size: %ld]", pidns.pid, pidns.tgid, size*count);}
    unsigned long totalSize = 0;
    if(count < 0 || CheckedArraySize(size, (unsigned long)count, &totalSize) == false)
    {
        return 0;
    }
    ResourceAllocHelper(totalSize, ctx, &pidns);
    struct ResourceInformation* event = bpf_map_lookup_elem(&argsHashMap, &pidns.pid);
    if(event != NULL)
    {
        event->previousAddress = (unsigned long)ptr;
    }
    return 0;
}

// ------------------------------------------------------------------------------------------
// uretprobe_reallocarray
//
// This is the entry point for the reallocarray exit uprobe. It's called when reallocarray is exiting.
// ------------------------------------------------------------------------------------------
SEC("uretprobe/libc.so.6:reallocarray")
int BPF_KRETPROBE(uretprobe_reallocarray, void* ret)
{
    struct bpf_pidns_info pidns = {};
    if(GetFilterPidTgid(&pidns) == false)
    {
        return 0;
    }

    {BPF_PRINTK("[***** reallocarray_exit, pid: %ld, tgid: %ld]", pidns.pid, pidns.tgid);}
    SendEvent(ret, false, &pidns);
    return 0;
}