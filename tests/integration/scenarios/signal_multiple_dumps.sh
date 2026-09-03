#!/bin/bash
# Test: signal monitoring reattaches until the configured dump count is reached.
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROCDUMPPATH="$DIR/../../../procdump"
TESTPROGPATH="$DIR/../../../ProcDumpTestApplication"

dumpDir=$(mktemp -d -t dump_XXXXXX)
"$TESTPROGPATH" signal &
target_pid=$!
sleep 1

"$PROCDUMPPATH" -log stdout -sig 10 -n 2 "$target_pid" "$dumpDir" &
pd_pid=$!
sleep 2

kill -10 "$target_pid"
for _ in $(seq 1 30); do
    if [ "$(find "$dumpDir" -maxdepth 1 -type f | wc -l)" -ge 1 ]; then
        break
    fi
    sleep 1
done

kill -10 "$target_pid"
for _ in $(seq 1 30); do
    if ! kill -0 "$pd_pid" 2>/dev/null; then
        break
    fi
    sleep 1
done

wait "$pd_pid"
status=$?
count=$(find "$dumpDir" -maxdepth 1 -type f -name 'ProcDumpTestApplication_signal_*' | wc -l)
kill -9 "$target_pid" 2>/dev/null

if [ "$status" -eq 0 ] && [ "$count" -eq 2 ]; then
    exit 0
fi

echo "TEST FAILED: expected 2 signal dumps, found $count (procdump status $status)"
exit 1