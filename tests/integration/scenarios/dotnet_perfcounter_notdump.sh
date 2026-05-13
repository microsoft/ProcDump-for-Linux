#!/bin/bash
# Test: -pc trigger does NOT fire when counter stays below threshold
# Sets counter to 10, monitors with -pc threshold 50, expects NO dump
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

# Set the custom counter to a value below threshold
wget -q -O /dev/null http://localhost:5032/setgauge/10

# Start procdump monitoring for the custom counter >= 50
sudo $PROCDUMPPATH -log stdout -pc TestWebApi.PerfCounter:test-gauge 50 -n 1 $TESTCHILDPID &

# Let monitoring run for a reasonable time without crossing threshold
sleep 10

# Check that no dump was created
COUNT=$(ls -1 *TestWebApi_*perfcounter* 2>/dev/null | wc -l)

sudo pkill -9 procdump
pkill -9 TestWebApi

if [[ "$COUNT" -eq 0 ]]; then
    rm -rf *TestWebApi_*perfcounter*
    popd
    exit 0
else
    rm -rf *TestWebApi_*perfcounter*
    popd
    exit 1
fi
