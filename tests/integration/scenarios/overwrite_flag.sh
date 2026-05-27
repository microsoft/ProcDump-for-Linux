#!/bin/bash
# Test: -o (overwrite) flag allows overwriting existing dump files
# Without -o, a second dump to the same filename is skipped; with -o it succeeds
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
PROCDUMPPATH="$DIR/../../../procdump";
TESTPROGPATH="$DIR/../../../ProcDumpTestApplication";

dumpDir=$(mktemp -d -t dump_XXXXXX)
dumpFile="$dumpDir/overwrite_test"

# Launch target in sleep mode
$TESTPROGPATH sleep &
target_pid=$!
sleep 1

# First dump (timer-based, immediate) — creates overwrite_test.<PID>
echo "[`date +"%T.%3N"`] First dump (should succeed)"
$PROCDUMPPATH -n 1 $target_pid $dumpFile

actual_file="$dumpFile.$target_pid"
if [[ ! -f $actual_file ]]; then
    echo "TEST FAILED: First dump was not created"
    kill -9 $target_pid 2>/dev/null
    exit 1
fi
firstSize=$(stat -c%s "$actual_file" 2>/dev/null)
echo "First dump: $actual_file ($firstSize bytes)"

# Second dump WITHOUT -o (should fail — file already exists)
echo "[`date +"%T.%3N"`] Second dump without -o (should be skipped)"
output=$($PROCDUMPPATH -n 1 $target_pid $dumpFile 2>&1)
echo "$output"

if echo "$output" | grep -q "already exists"; then
    echo "GOOD: dump was correctly skipped without -o"
else
    echo "TEST FAILED: Expected 'already exists' message but dump was not blocked"
    kill -9 $target_pid 2>/dev/null
    exit 1
fi

# Third dump WITH -o (should overwrite successfully)
echo "[`date +"%T.%3N"`] Third dump with -o (should overwrite)"
output=$($PROCDUMPPATH -o -n 1 $target_pid $dumpFile 2>&1)
echo "$output"

kill -9 $target_pid 2>/dev/null

thirdSize=$(stat -c%s "$actual_file" 2>/dev/null)
if echo "$output" | grep -q "Core dump 0 generated"; then
    echo "TEST PASSED: Overwrite flag worked (first=$firstSize, after_o=$thirdSize)"
    exit 0
else
    echo "TEST FAILED: Dump with -o did not generate"
    exit 1
fi
