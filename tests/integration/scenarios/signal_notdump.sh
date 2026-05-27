#!/bin/bash
# Test: -sig trigger does NOT fire when a non-matching signal is delivered
# Monitors for SIGUSR2 (12), sends SIGUSR1 (10), expects NO dump
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
PROCDUMPPATH="$DIR/../../../procdump";
TESTPROGPATH="$DIR/../../../ProcDumpTestApplication";

dumpDir=$(mktemp -d -t dump_XXXXXX)

# Launch target in sleep mode
$TESTPROGPATH sleep &
target_pid=$!
sleep 1

# Monitor for SIGUSR2 only
echo "[`date +"%T.%3N"`] $PROCDUMPPATH -log stdout -sig 12 -n 1 $target_pid $dumpDir"
$PROCDUMPPATH -log stdout -sig 12 -n 1 $target_pid $dumpDir &
pd_pid=$!
sleep 2

# Send SIGUSR1 (not monitored) — should NOT trigger a dump
echo "[`date +"%T.%3N"`] Sending SIGUSR1 (non-matching) to $target_pid"
kill -10 $target_pid
sleep 5

# Clean up
kill -9 $pd_pid 2>/dev/null
kill -9 $target_pid 2>/dev/null

# Verify NO dump was created
foundFile=$(find "$dumpDir" -maxdepth 1 -name "ProcDumpTestApplication_*" ! -name "*.restrack" -print -quit)
if [[ -z $foundFile ]]; then
    echo "TEST PASSED: No dump generated for non-matching signal"
    exit 0
else
    echo "TEST FAILED: Dump was generated for non-matching signal: $foundFile"
    exit 1
fi
