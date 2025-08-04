#!/bin/bash
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
PROCDUMPPATH=$(readlink -m "$DIR/$1");
TESTWEBAPIPATH=$(readlink -m "$DIR/../TestWebApi");
HELPERS=$(readlink -m "$DIR/../helpers.sh");

source $HELPERS

pushd .
cd $TESTWEBAPIPATH
rm -rf *TestWebApi_*commit*
dotnet restore --configfile ../../../../nuget.config
dotnet run --urls=http://localhost:5032&

# waiting TestWebApi ready to service
waitforurl http://localhost:5032/throwinvalidoperation
if [ $? -eq -1 ]; then
    pkill -9 TestWebApi
    popds
    exit 1
fi

sudo $PROCDUMPPATH -log stdout -m 10,20,30 -w TestWebApi&

# waiting for procdump child process
PROCDUMPCHILDPID=-1
waitforprocdump PROCDUMPCHILDPID
if [ $PROCDUMPCHILDPID -eq -1 ]; then
    pkill -9 TestWebApi
    pkill -9 procdump
    popd
    exit 1
fi

COUNT=( $(ls *TestWebApi_*commit* 2>/dev/null | wc -l) )
i=0
while [ $COUNT -ne 3 ]
do
    ((i=i+1))
    if [[ "$i" -gt 60 ]]; then
        pkill -9 TestWebApi
        pkill -9 procdump
        popd
        exit 1
        return
    fi
    sleep 1s
    COUNT=( $(ls *TestWebApi_*commit* 2>/dev/null | wc -l) )
done

sudo pkill -9 procdump
pkill -9 TestWebApi

rm -rf *TestWebApi_*commit*
popd
if [[ "$COUNT" -eq 3 ]]; then

    exit 0
else
    exit 1
fi
