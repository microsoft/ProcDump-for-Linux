#!/bin/bash
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
PROCDUMPPATH=$(readlink -m "$DIR/$1");
TESTWEBAPIPATH=$(readlink -m "$DIR/../TestWebApi");
HELPERS=$(readlink -m "$DIR/../helpers.sh");

source $HELPERS

pushd .
cd $TESTWEBAPIPATH
rm -rf *TestWebApi_*Exception*
dotnet run --urls=http://localhost:5032&

# waiting TestWebApi ready to service
waitforurl http://localhost:5032/throwinvalidoperation
if [ $? -eq -1 ]; then
    pkill -9 TestWebApi
    popds
    exit 1
fi

sudo $PROCDUMPPATH -log -e TestWebApi&

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

sudo kill -2 ${PROCDUMPCHILDPID}
popd

sleep 1s
if [ -S $SOCKETPATH ];
then
    rm $SOCKETPATH
fi

#check to make sure profiler so is unloaded
PROF="$(cat /proc/${TESTCHILDPID}/maps | awk '{print $6}' | grep '\procdumpprofiler.so' | uniq)"
pkill -9 TestWebApi
pkill -9 procdump
if [[ "$PROF" == "procdumpprofiler.so" ]]; then
    exit 1
else
    exit 0
fi