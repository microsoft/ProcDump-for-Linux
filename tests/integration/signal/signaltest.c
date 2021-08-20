// 
// Used to test the signal triggering (and forwarding) of procdump.
//
// 1. Run this test app (it registers for the first 23 signals). 
// 2. Run procdump against this pid
// 3. use kill to send whichever signal you are interested in triggering procdump (or not trigger)
// 4. Make sure in all cases (except for signals that can't be intercepted) that this program outputs "Caught signal X"
//    where X is the signal you sent. If the output does not show that signal being handled, it means the signal forwarding 
//    in procdump is not working properly and needs to be investigated.
//
#include <stdio.h>
#include <stdlib.h>
#include <wait.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <unistd.h>


void sig_handler(int signum)
{
    if(signum==SIGINT)
    {
        exit(-1);
    }
    
    // We shouldnt be using printf in a signal handler but in this simple test case its fine
    printf("Caught signal: %d\n", signum);
}

void main(int argc, char** argv)
{
    for(int i=0; i<24; i++)
    {
        signal(i, sig_handler);
    }

    while(1)
    {
        pause();
    }
}