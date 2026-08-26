#!/bin/bash
# Test: CLI -mc applies the requested coredump_filter and restores the target.
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROCDUMPPATH="$DIR/../../../procdump"
TESTPROGPATH="$DIR/../../../ProcDumpTestApplication"

dumpDir=$(mktemp -d -t dump_XXXXXX)
"$TESTPROGPATH" sleep &
target_pid=$!
sleep 1
before=$(cat "/proc/$target_pid/coredump_filter")

"$PROCDUMPPATH" -mc 0x7f "$target_pid" "$dumpDir"
status=$?
after=$(cat "/proc/$target_pid/coredump_filter")
count=$(find "$dumpDir" -maxdepth 1 -type f | wc -l)
kill -9 "$target_pid" 2>/dev/null

if [ "$status" -eq 0 ] && [ "$before" = "$after" ] && [ "$count" -eq 1 ]; then
    exit 0
fi

echo "TEST FAILED: status=$status mask=$before->$after dump_count=$count"
exit 1