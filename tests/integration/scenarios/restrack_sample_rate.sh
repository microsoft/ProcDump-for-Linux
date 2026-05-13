#!/bin/bash
# Test: -sr (sample rate) parameter works with -restrack
# Verifies that restrack with -sr 1 produces a valid .restrack file
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
PROCDUMPPATH="$DIR/../../../procdump";

dumpDir=$(mktemp -d -t dump_XXXXXX)

# uses 'cat' as target for restrack (same as restrack_trigger.sh)
cat /dev/urandom > /dev/null &
target_pid=$!

# -restrack with -sr 1 (sample every allocation)
echo "[`date +"%T.%3N"`] $PROCDUMPPATH -restrack -sr 1 $target_pid $dumpDir"
echo 't' | $PROCDUMPPATH "-restrack" "-sr" "1" $target_pid $dumpDir

# asserts that restrack file was generated
# We just verify the file exists and -sr was accepted — actual content
# depends on the target's allocation patterns which vary per run
foundFile=$(find "$dumpDir" -maxdepth 1 -name "cat_manual_*.restrack" -print -quit)
if [[ -n $foundFile ]]; then
    fileSize=$(stat -c%s "$foundFile")
    echo "$foundFile ($fileSize bytes)"
    exit 0
else
    echo "TEST FAILED: No restrack file generated with -sr"
    exit 1
fi
