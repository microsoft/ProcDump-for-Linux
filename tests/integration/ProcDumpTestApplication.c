#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <limits.h>
#include <sys/mman.h>
#include <time.h>
#if defined(__linux__) && !defined(__APPLE__)
#include <sys/prctl.h>
#endif

#define FILE_DESC_COUNT	500
#define THREAD_COUNT	100


void* dFunc(int type)
{
        if(type == 0)
        {
                char* alloc = malloc(10000);
                for(int i=0; i<10000; i++)
                {
                        alloc[i] = 'a';
                }
                mlock(alloc, 10000);
                return alloc;
        }
        else if (type == 1)
        {
                char* callocAlloc = calloc(1, 10000);
                mlock(callocAlloc, 10000);
                return callocAlloc;
        }
        else if (type == 2)
        {
                void* lastAlloc = malloc(10000);
                void* newAlloc = realloc(lastAlloc, 20000);
                for(int i=0; i<20000; i++)
                {
                        ((char*)newAlloc)[i] = 'a';
                }                
                mlock(newAlloc, 20000);
                return newAlloc;
        }
        else if (type == 3)
        {
#ifdef __linux__                
                void* lastAlloc = malloc(10000);
                void* newAlloc = reallocarray(lastAlloc, 10, 20000);
                return newAlloc;
#endif                
                return NULL;
        }
        else
        {
                return NULL;
        }
}

void* c(int type)
{
        return dFunc(type);
}

void* b(int type)
{
        return c(type);
}

void* a(int type)
{
        return b(type);
}

void* ThreadProc(void *input)
{
    sleep(UINT_MAX);
    return NULL;
};

// CPU stress function - consumes specified percentage of CPU
void stress_cpu(int target_cpu_percentage) {
    struct timespec work_time, sleep_time;
    long work_usec, sleep_usec;
    
    // Calculate work and sleep times for desired CPU percentage
    // Use 10ms cycle time for good responsiveness
    work_usec = (10000 * target_cpu_percentage) / 100;
    sleep_usec = 10000 - work_usec;
    
    work_time.tv_sec = work_usec / 1000000;
    work_time.tv_nsec = (work_usec % 1000000) * 1000;
    
    sleep_time.tv_sec = sleep_usec / 1000000;
    sleep_time.tv_nsec = (sleep_usec % 1000000) * 1000;
    
    printf("CPU stress: targeting %d%% load\n", target_cpu_percentage);
    
    while (1) {
        struct timespec start, now;
        clock_gettime(CLOCK_MONOTONIC, &start);
        
        // Busy work period
        while (1) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed = (now.tv_sec - start.tv_sec) * 1000000 + 
                          (now.tv_nsec - start.tv_nsec) / 1000;
            if (elapsed >= work_usec) break;
            
            // Some actual work to prevent optimization
            volatile double x = 1.0;
            for (int i = 0; i < 100; i++) {
                x = x * 1.1;
            }
        }
        
        // Sleep period
        if (sleep_usec > 0) {
            nanosleep(&sleep_time, NULL);
        }
    }
}

// Memory stress function - allocates specified amount of memory
void stress_memory(size_t target_bytes) {
    void **memory_blocks = NULL;
    int num_blocks = 0;
    size_t total_allocated = 0;
    const size_t block_size = 4096; // 4KB blocks

    size_t blocks_needed = (target_bytes + block_size - 1) / block_size;
    
    printf("Memory stress: allocating %zu bytes (%zu blocks)\n", target_bytes, blocks_needed);
    
    memory_blocks = malloc(blocks_needed * sizeof(void*));
    if (!memory_blocks) {
        perror("Failed to allocate memory tracking array");
        exit(1);
    }
    
    // Allocate memory blocks
    for (size_t i = 0; i < blocks_needed; i++) {
        memory_blocks[i] = malloc(block_size);
        if (!memory_blocks[i]) {
            fprintf(stderr, "Failed to allocate block %zu (allocated %zu bytes so far)\n",
                    i, total_allocated);
            break;
        }
        
        // Touch the memory to ensure it's actually allocated
        memset(memory_blocks[i], (int)(i & 0xFF), block_size);
        
        num_blocks++;
        total_allocated += block_size;
        
        // Small delay to make allocation more realistic
        if (i % 100 == 0) {
            usleep(1000); // 1ms delay every 100 blocks
        }
    }
    
    printf("Successfully allocated %zu bytes in %d blocks\n", total_allocated, num_blocks);
}

int main(int argc, char *argv[])
{
#if defined(__linux__) && !defined(__APPLE__)
    // When the Linux kernel's Yama security module is active with ptrace_scope = 1,
    // it restricts ptrace access to only allow processes to trace their direct children or
    // processes that have explicitly granted permission.
    // The prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY) call in the test application explicitly allows
    // any process to attach to it using ptrace, bypassing the default Yama security restrictions.
    // Without this explicit permission, ProcDump would be unable to attach to the test process
    // and the monitoring functionality would fail with permission denied errors.
    if (prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY) < 0)
    {
        printf("error setting PR_SET_PTRACER");
        exit(1);
    }
#endif

    if (argc > 1)
    {
        //
        // To avoid timing differences, each test below should sleep indefinately once done.
        // The process will be killed by the test harness once procdump has finished monitoring
        //
        if (strcmp("sleep", argv[1]) == 0)
        {
            sleep(UINT_MAX);
        }
        else if (strcmp("burn", argv[1]) == 0)
        {
            while(1);
        }
        else if (strcmp("fc", argv[1]) == 0)
        {
          FILE* fd[FILE_DESC_COUNT];
          for(int i=0; i<FILE_DESC_COUNT; i++)
          {
              fd[i] = fopen(argv[0], "r");
          }
          memset(fd, 0, FILE_DESC_COUNT*sizeof(FILE*));
          sleep(UINT_MAX);
        }
        else if (strcmp("tc", argv[1]) == 0)
        {
          pthread_t threads[THREAD_COUNT];
          for(int i=0; i<THREAD_COUNT; i++)
          {
              pthread_create(&threads[i], NULL, ThreadProc, NULL);
          }
          sleep(UINT_MAX);
        }
        else if (strcmp("mem", argv[1]) == 0)
        {
            if (argc < 3)
            {
                // if no extra argument, allocate memory using different allocation methods (malloc, calloc, realloc, reallocarray)
                sleep(10);
                for(int i=0; i<1000; i++)
                {
                    a(0);
                    a(1);
                    a(2);
                    a(3);
                }
            }
            else
            {
                char *endptr;
                double value = strtod(argv[2], &endptr);

                if (value < 0) {
                    fprintf(stderr, "Invalid memory size: %s\n", argv[2]);
                    exit(1);
                }

                size_t multiplier = 1;
                if (endptr && *endptr) {
                    switch (*endptr) {
                        case 'K': case 'k': multiplier = 1024; break;
                        case 'M': case 'm': multiplier = 1024 * 1024; break;
                        case 'G': case 'g': multiplier = 1024 * 1024 * 1024; break;
                        default:
                            fprintf(stderr, "Invalid memory size suffix: %c\n", *endptr);
                            exit(1);
                    }
                }
                
                stress_memory((size_t)(value * multiplier));
            }

            sleep(UINT_MAX);
        }
        else if (strcmp("cpu", argv[1]) == 0)
        {
            if (argc < 3)
            {
                printf("cpu option requires a percentage argument\n");
                exit(1);
            }

            int target_cpu_percentage = atoi(argv[2]);
            if (target_cpu_percentage <= 0 || target_cpu_percentage > 100) {
                fprintf(stderr, "CPU load must be between 1-100%%\n");
                exit(1);
            }
            
            stress_cpu(target_cpu_percentage);
        }
        else
        {
            fprintf(stderr, "Unknown argument: %s\n", argv[1]);
            exit(1);
        }
    }
    else
    {
        printf("No arguments specified.\n");
        exit(1);
    }
}