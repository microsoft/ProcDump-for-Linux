#!/bin/bash
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
PROCDUMPPATH="$DIR/../../../procdump";


GCORE=$(which gcore)
if [ -z "$GCORE" ]; then
    echo "TEST FAILED: gcore is not installed"
    exit 1
fi
cleanup() {
    if [ -f /tmp/gcore ] && [ ! -e "$GCORE" ]; then
        mv /tmp/gcore "$GCORE"
    fi
}
trap cleanup EXIT INT TERM

echo [`date +"%T.%3N"`] Moving gcore from $GCORE to /tmp/gcore
mv $GCORE /tmp/gcore

echo [`date +"%T.%3N"`] Starting ProcDump
output=$($PROCDUMPPATH -usegcore "123")

echo [`date +"%T.%3N"`] Restoring gcore from tmp/gcore to $GCORE
mv /tmp/gcore $GCORE


expected="failed to locate gcore binary"
if [[ "$output" == *"$expected"* ]]; then
    exit 0
else
    exit 1
fi