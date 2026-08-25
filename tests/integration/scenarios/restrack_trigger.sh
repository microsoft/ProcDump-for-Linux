#!/bin/bash
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
PROCDUMPPATH="$DIR/../../../procdump";
TESTPROGPATH="$DIR/../../../ProcDumpTestApplication";

dumpDir=$(mktemp -d -t dump_XXXXXX)
targetLog=$(mktemp -t restrack_target_XXXXXX)

# The mem mode waits before allocating through a -> b -> c -> dFunc, giving
# ProcDump time to attach before producing deterministic outstanding allocations.
$TESTPROGPATH mem > "$targetLog" 2>&1 &
target_pid=$!
trap 'kill -9 "$target_pid" 2>/dev/null; rm -f "$targetLog"' EXIT

echo [`date +"%T.%3N"`] "$PROCDUMPPATH -restrack $target_pid $dumpDir"
{
    for _ in $(seq 1 30); do
        if grep -q "Restrack allocations complete" "$targetLog"; then
            break
        fi
        sleep 1
    done
    echo 't'
} | $PROCDUMPPATH "-restrack" "-sr" "1" $target_pid $dumpDir

foundFile=$(find "$dumpDir" -maxdepth 1 -name "ProcDumpTestApplication_manual_*.restrack" -print -quit)
if [[ -z $foundFile ]]; then
    echo "TEST FAILED: No restrack report generated"
    exit 1
fi

if ! grep -q '^+++ Leaked Allocation' "$foundFile"; then
    echo "TEST FAILED: Restrack report has no allocation groups"
    exit 1
fi

for frame in dFunc c b a; do
    if ! grep -Eq "^[[:space:]]*\[0x[0-9a-f]+\] ${frame}\+0x[0-9a-f]+" "$foundFile"; then
        echo "TEST FAILED: Restrack report is missing symbolized frame '$frame'"
        exit 1
    fi
done

echo "$foundFile"
