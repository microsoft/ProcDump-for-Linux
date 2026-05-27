#!/bin/bash
# Test: -pcl trigger fires when a .NET EventCounter drops BELOW threshold
# Sets counter to 100 initially, monitors with -pcl threshold 50,
# then drops counter to 10, expects dump
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

# Set the custom counter to a value ABOVE threshold first
wget -q -O /dev/null http://localhost:5032/setgauge/100

# Start procdump monitoring for the custom counter < 50 (below)
sudo $PROCDUMPPATH -log stdout -pcl TestWebApi.PerfCounter:test-gauge 50 -n 1 $TESTCHILDPID &

# Give procdump time to connect and start EventPipe session
sleep 5

# Now drop the counter BELOW threshold to trigger a dump
wget -q -O /dev/null http://localhost:5032/setgauge/10

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
