#!/bin/bash
# Test realloc and failed mmap/munmap bookkeeping in the eBPF resource tracker.
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROCDUMPPATH="$DIR/../../../procdump"
TESTPROGPATH="$DIR/../../../ProcDumpTestApplication"

dumpDir=$(mktemp -d -t dump_XXXXXX)
targetLog=$(mktemp -t restrack_edges_XXXXXX)
"$TESTPROGPATH" restrack_edges >"$targetLog" 2>&1 &
target_pid=$!

{
    for _ in $(seq 1 30); do
        if grep -q "Restrack edge allocations complete" "$targetLog"; then
            break
        fi
        sleep 1
    done
    echo t
} | "$PROCDUMPPATH" -restrack -sr 1 "$target_pid" "$dumpDir"
status=$?
report=$(find "$dumpDir" -maxdepth 1 -name '*.restrack' -print -quit)
kill -9 "$target_pid" 2>/dev/null

if [ "$status" -ne 0 ] || [ ! -f "$report" ]; then
    echo "TEST FAILED: restrack report was not generated"
    exit 1
fi
if ! grep -q 'allocation size: 0x4e20' "$report"; then
    echo "TEST FAILED: realloc result size was not tracked"
    exit 1
fi
if grep -q 'allocation size: 0x2710' "$report"; then
    echo "TEST FAILED: realloc old allocation remained tracked"
    exit 1
fi
if grep -qi 'ffffffffffffffff' "$report"; then
    echo "TEST FAILED: MAP_FAILED was recorded as an allocation"
    exit 1
fi
exit 0