#!/bin/bash
#
# Library API: basic on-demand dump of a live .NET process (TestWebApi)
# via pdWriteDump. Verifies the library can dump a managed/.NET target,
# not just native processes.
#
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
DRIVERPATH=$(readlink -m "$DIR/../../../ProcDumpLibTestDriver");
TESTWEBAPIPATH=$(readlink -m "$DIR/../TestWebApi");
HELPERS=$(readlink -m "$DIR/../helpers.sh");

source $HELPERS

if [ ! -x "$DRIVERPATH" ]; then
    echo "[libtest] ERROR: driver not found or not executable: $DRIVERPATH"
    exit 1
fi

# Clean up dumps from a prior run
rm -rf /tmp/libdumpnet_*
dumpDir=$(mktemp -d -t libdumpnet_XXXXXX)

pushd .
cd $TESTWEBAPIPATH
rm -rf *TestWebApi_*Exception*
dotnet run --urls=http://localhost:5033 &

# Wait until TestWebApi is ready to service requests
waitforurl http://localhost:5033/throwinvalidoperation
if [ $? -eq -1 ]; then
    pkill -9 TestWebApi
    popd
    exit 1
fi

TESTCHILDPID=$(ps -o pid= -C "TestWebApi" | tr -d ' ')
echo "TestWebApi PID: $TESTCHILDPID"
if [ -z "$TESTCHILDPID" ]; then
    echo "[libtest] FAIL: could not determine TestWebApi pid"
    pkill -9 TestWebApi
    popd
    exit 1
fi

dumpPath="$dumpDir/core"
echo [`date +"%T.%3N"`] Running: "$DRIVERPATH" "$TESTCHILDPID" "$dumpPath" default 1
sudo "$DRIVERPATH" "$TESTCHILDPID" "$dumpPath" default 1
rc=$?
echo "[libtest] driver exit code: $rc"

popd
pkill -9 TestWebApi

result=0

# 1. The driver must report success.
if [ "$rc" -ne 0 ]; then
    echo "[libtest] FAIL: expected success but driver returned $rc"
    result=1
else
    echo "[libtest] PASS: driver returned success"
fi

# 2. A non-trivial dump file must have been produced. For .NET targets the
#    runtime's createdump writes to the literal path, while the native engine
#    appends ".<pid>"; accept either by locating the file in the dump dir.
producedDump=$(find "$dumpDir" -mindepth 1 -maxdepth 1 -type f -print -quit)
if [ -n "$producedDump" ] && [ -f "$producedDump" ]; then
    dumpSize=$(stat -c%s "$producedDump")
    echo "[libtest] dump produced: $producedDump (${dumpSize} bytes)"
    if [ "$dumpSize" -gt 1024 ]; then
        echo "[libtest] PASS: dump file is non-trivial"
    else
        echo "[libtest] FAIL: dump file is too small (${dumpSize} bytes)"
        result=1
    fi
else
    echo "[libtest] FAIL: no dump file found in $dumpDir"
    result=1
fi

exit $result
