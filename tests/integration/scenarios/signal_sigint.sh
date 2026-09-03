#!/bin/bash
# Test: Ctrl+C cancels signal monitoring even when the target receives no signal.
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROCDUMPPATH="$DIR/../../../procdump"
TESTPROGPATH="$DIR/../../../ProcDumpTestApplication"

dumpDir=$(mktemp -d -t dump_XXXXXX)
"$TESTPROGPATH" signal &
target_pid=$!
sleep 1

"$PROCDUMPPATH" -sig 10 "$target_pid" "$dumpDir" &
pd_pid=$!
sleep 2
kill -INT "$pd_pid"

for _ in $(seq 1 50); do
    if ! kill -0 "$pd_pid" 2>/dev/null; then
        wait "$pd_pid"
        status=$?
        kill -9 "$target_pid" 2>/dev/null
        [ "$status" -eq 0 ] && exit 0
        echo "TEST FAILED: procdump exited with $status"
        exit 1
    fi
    sleep 0.1
done

kill -9 "$pd_pid" "$target_pid" 2>/dev/null
echo "TEST FAILED: signal monitor did not exit after SIGINT"
exit 1