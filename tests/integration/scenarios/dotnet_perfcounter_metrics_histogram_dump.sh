#!/bin/bash
# Test: -pc trigger fires using System.Diagnostics.Metrics Histogram
# Monitors http.server.request.duration (p50) >= 0.5 seconds via the Metrics API
# Sends slow requests (1s each) to push the histogram p50 above 0.5s, expects dump
echo "ProcDump path "$1
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
PROCDUMPPATH=$(readlink -m "$DIR/$1");
TESTWEBAPIPATH=$(readlink -m "$DIR/../TestWebApi");
HELPERS=$(readlink -m "$DIR/../helpers.sh");

source $HELPERS

pushd .
cd $TESTWEBAPIPATH
rm -rf *TestWebApi_*perfcounter*
pkill -9 TestWebApi
pkill -9 procdump
dotnet restore --configfile ../../../../nuget.config
dotnet run --urls=http://localhost:5032&

# waiting TestWebApi ready to service
waitforurl http://localhost:5032/throwinvalidoperation
if [ $? -eq -1 ]; then
    pkill -9 TestWebApi
    popd
    exit 1
fi

TESTCHILDPID=$(ps -o pid= -C "TestWebApi" | tr -d ' ')

# Start procdump monitoring for ASP.NET request duration histogram p50 >= 0.5s
# Microsoft.AspNetCore.Hosting is the meter name
# http.server.request.duration is the instrument name (Histogram, in seconds)
# [p50] selects the 50th percentile (median) — can also use [p95], [p99], etc.
sudo $PROCDUMPPATH -log stdout -pc Microsoft.AspNetCore.Hosting:http.server.request.duration[p50] 0.5 -n 1 $TESTCHILDPID &

# Give procdump time to connect and start EventPipe session
sleep 5

# Send several slow requests (1s each) to push the histogram above 0.5s
WGET_PIDS=""
for i in 1 2 3; do
    wget -q -O /dev/null http://localhost:5032/slowrequest/1000 &
    WGET_PIDS="$WGET_PIDS $!"
done
for pid in $WGET_PIDS; do wait $pid 2>/dev/null; done

# Wait for dump to appear
waitforndumps 1 "*TestWebApi_*perfcounter*" "COUNT"

sudo pkill -9 procdump
pkill -9 TestWebApi

if [[ "$COUNT" -ge 1 ]]; then
    echo "TEST PASSED: Metrics histogram triggered dump"
    rm -rf *TestWebApi_*perfcounter*
    popd
    exit 0
else
    echo "TEST FAILED: No dump was generated"
    rm -rf *TestWebApi_*perfcounter*
    popd
    exit 1
fi
