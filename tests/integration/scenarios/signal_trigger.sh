#!/bin/bash
# Test: -sig trigger fires when a matching signal is delivered
# Monitors for SIGUSR1 (10) and SIGUSR2 (12), sends SIGUSR1, expects dump
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
PROCDUMPPATH="$DIR/../../../procdump";
TESTPROGPATH="$DIR/../../../ProcDumpTestApplication";

dumpDir=$(mktemp -d -t dump_XXXXXX)

# Launch target in sleep mode (it will just sit idle)
$TESTPROGPATH sleep &
target_pid=$!
sleep 1

# Monitor for SIGUSR1 (10) and SIGUSR2 (12), expect 1 dump
echo "[`date +"%T.%3N"`] $PROCDUMPPATH -log stdout -sig 10,12 -n 1 $target_pid $dumpDir"
$PROCDUMPPATH -log stdout -sig 10,12 -n 1 $target_pid $dumpDir &
pd_pid=$!
sleep 2

# Send SIGUSR1 to the target process
echo "[`date +"%T.%3N"`] Sending SIGUSR1 to $target_pid"
kill -10 $target_pid
sleep 5

# Clean up
kill -9 $pd_pid 2>/dev/null
kill -9 $target_pid 2>/dev/null

# Verify a dump was created
foundFile=$(find "$dumpDir" -maxdepth 1 -name "ProcDumpTestApplication_*" ! -name "*.restrack" -print -quit)
if [[ -n $foundFile ]]; then
    echo "$foundFile"
    exit 0
else
    echo "TEST FAILED: No dump was generated for signal trigger"
    exit 1
fi
