#!/bin/bash
# Test: -pgid monitors all processes in a process group
# Creates a process in its own group, triggers memory dump
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
PROCDUMPPATH="$DIR/../../../procdump";
TESTPROGPATH="$DIR/../../../ProcDumpTestApplication";

dumpDir=$(mktemp -d -t dump_XXXXXX)

# Launch target process in its own process group using setsid
# Uses memory mode (90MB) — more reliable than CPU for group monitoring
setsid $TESTPROGPATH mem 90M &
target_pid=$!
sleep 2

# With setsid, the process becomes its own session and group leader (PGID == PID)
pgid=$target_pid

echo "[`date +"%T.%3N"`] Target PID=$target_pid  PGID=$pgid"

# Monitor the process group for high memory (>= 50 MB)
echo "[`date +"%T.%3N"`] $PROCDUMPPATH -log stdout -m 50 -n 1 -pgid $pgid $dumpDir"
$PROCDUMPPATH -log stdout -m 50 -n 1 -pgid $pgid $dumpDir &
pd_pid=$!

# Wait for dump to appear (up to 30s)
for i in $(seq 1 30); do
    foundFile=$(find "$dumpDir" -maxdepth 1 -name "ProcDumpTestApplication_*" ! -name "*.restrack" -print -quit)
    if [[ -n $foundFile ]]; then
        break
    fi
    sleep 1
done

# Clean up
kill -9 $pd_pid 2>/dev/null
kill -9 $target_pid 2>/dev/null

if [[ -n $foundFile ]]; then
    echo "$foundFile"
    exit 0
else
    echo "TEST FAILED: No dump generated for -pgid process group"
    exit 1
fi
