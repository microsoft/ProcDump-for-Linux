#!/bin/bash
echo "ProcDump path "$1
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
PROCDUMPPATH=$(readlink -m "$DIR/$1");
TESTWEBAPIPATH=$(readlink -m "$DIR/../TestWebApi");
HELPERS=$(readlink -m "$DIR/../helpers.sh");

source $HELPERS

pushd .
cd $TESTWEBAPIPATH
rm -rf *TestWebApi_*Exception*
pkill -9 TestWebApi
pkill -9 procdump
dotnet restore --configfile ../../../../nuget.config
dotnet run --urls=http://localhost:5032&

# waiting TestWebApi ready to service
waitforurl http://localhost:5032/throwinvalidoperation
if [ $? -eq -1 ]; then
    pkill -9 TestWebApi
    popds
    exit 1
fi

sudo $PROCDUMPPATH -log stdout -e -w TestWebApi&

# waiting for procdump child process
PROCDUMPCHILDPID=-1
waitforprocdump PROCDUMPCHILDPID
if [ $PROCDUMPCHILDPID -eq -1 ]; then
    pkill -9 TestWebApi
    pkill -9 procdump
    popd
    exit 1
fi

TESTCHILDPID=$(ps -o pid= -C "TestWebApi" | tr -d ' ')

#make sure procdump ready to capture before throw exception by checking if socket created
SOCKETPATH=-1
waitforprocdumpsocket $PROCDUMPCHILDPID $TESTCHILDPID SOCKETPATH
if [ $SOCKETPATH -eq -1 ]; then
    pkill -9 TestWebApi
    pkill -9 procdump
    popd
    exit 1
fi
echo "SOCKETPATH: "$SOCKETPATH

# wait for profile to be loaded
for i in {1..15}; do
    PROF="$(cat /proc/${TESTCHILDPID}/maps | awk '{print $6}' | grep '\procdumpprofiler.so' | uniq)"
    if [[ "$PROF" == *"procdumpprofiler.so" ]]; then
        echo "[script] Profiler was loaded..."
        break
    fi
    echo "[script] Waiting for profiler to be loaded..."
    sleep 1
done

wget http://localhost:5032/throwinvalidoperation

# wait for dump to be created
for i in {1..15}; do
    COUNT=( $(ls *TestWebApi_*Exception* | wc -l) )
    if [[ "$COUNT" -eq 1 ]]; then
        echo "[script] Dump was written..."
        break
    fi
    echo "[script] Waiting for dump to be written..."
    sleep 1
done

sudo pkill -9 procdump

if [ -S $SOCKETPATH ];
then
    rm $SOCKETPATH
fi

if [[ "$COUNT" -eq 1 ]]; then
    rm -rf *TestWebApi_*Exception*
    popd

    #check to make sure profiler so is unloaded
    PROF="$(cat /proc/${TESTCHILDPID}/maps | awk '{print $6}' | grep '\procdumpprofiler.so' | uniq)"
    pkill -9 TestWebApi
    if [[ "$PROF" == "procdumpprofiler.so" ]]; then
        exit 1
    else
        exit 0
    fi
else
    pkill -9 TestWebApi
    popd
    exit 1
fi
