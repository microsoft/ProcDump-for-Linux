#!/bin/bash
# Test: -pc trigger fires when ASP.NET current-requests (built-in counter) exceeds threshold
# Sends slow requests (3s each) to keep concurrent requests above 1, expects dump
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

# Start procdump monitoring for ASP.NET current-requests >= 1
sudo $PROCDUMPPATH -log stdout -pc Microsoft.AspNetCore.Hosting:current-requests 1 -n 1 $TESTCHILDPID &

# Give procdump time to connect and start EventPipe session
sleep 5

# Send several slow requests (3s each) to keep current-requests above the threshold
WGET_PIDS=""
for i in 1 2 3; do
    wget -q -O /dev/null http://localhost:5032/slowrequest/3000 &
    WGET_PIDS="$WGET_PIDS $!"
done
for pid in $WGET_PIDS; do wait $pid 2>/dev/null; done

# Wait for dump to appear
waitforndumps 1 "*TestWebApi_*perfcounter*" "COUNT"

sudo pkill -9 procdump
pkill -9 TestWebApi

if [[ "$COUNT" -ge 1 ]]; then
    rm -rf *TestWebApi_*perfcounter*
    popd
    exit 0
else
    rm -rf *TestWebApi_*perfcounter*
    popd
    exit 1
fi
