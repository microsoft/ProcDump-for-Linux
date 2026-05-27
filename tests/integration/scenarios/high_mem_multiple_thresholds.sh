#!/bin/bash
# Test: -m with comma-separated thresholds triggers multiple dumps at different levels
# Uses -m 20,40,60 with a process allocating 90MB, expects 3 dumps
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
PROCDUMPPATH="$DIR/../../../procdump";
TESTPROGPATH="$DIR/../../../ProcDumpTestApplication";

dumpDir=$(mktemp -d -t dump_XXXXXX)

# Launch memory stress process — allocates 90MB
$TESTPROGPATH mem 90M &
target_pid=$!
sleep 1

# Monitor with 3 memory thresholds: 20MB, 40MB, 60MB
# The process allocates 90MB, so all 3 thresholds should be crossed
# NOTE: -n is NOT allowed with multiple thresholds (count is implicit from threshold count)
echo "[`date +"%T.%3N"`] $PROCDUMPPATH -log stdout -m 20,40,60 $target_pid $dumpDir"
$PROCDUMPPATH -log stdout -m 20,40,60 $target_pid $dumpDir &
pd_pid=$!

# Wait for dumps to appear (up to 30s)
for i in $(seq 1 30); do
    count=$(find "$dumpDir" -maxdepth 1 -name "ProcDumpTestApplication_*" ! -name "*.restrack" | wc -l)
    if [[ "$count" -ge 3 ]]; then
        break
    fi
    sleep 1
done

# Clean up
kill -9 $pd_pid 2>/dev/null
kill -9 $target_pid 2>/dev/null

# Verify at least 3 dumps were created
count=$(find "$dumpDir" -maxdepth 1 -name "ProcDumpTestApplication_*" ! -name "*.restrack" | wc -l)
echo "Dumps created: $count"

if [[ "$count" -ge 3 ]]; then
    echo "TEST PASSED: Multiple memory thresholds triggered $count dumps"
    exit 0
else
    echo "TEST FAILED: Expected 3 dumps, got $count"
    exit 1
fi
