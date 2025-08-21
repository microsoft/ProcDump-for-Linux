#!/bin/bash
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
PROCDUMPPATH="$DIR/../../../procdump";

dumpDir=$(mktemp -d -t dump_XXXXXX)

# uses 'cat' as target for restrack
cat /dev/urandom > /dev/null &
target_pid=$!

# sends 'y' when the message 'Press any key to trigger a Restrack snapshot' appears
echo [`date +"%T.%3N"`] "$PROCDUMPPATH -restrack $target_pid $dumpDir"
echo 't' | $PROCDUMPPATH "-restrack" $target_pid $dumpDir

# asserts that restrack file was generated
foundFile=$(find "$dumpDir" -maxdepth 1 -name "cat_manual_*.restrack" -print -quit)
if [[ -n $foundFile ]]; then
    echo $foundFile
    exit 0
else
    exit 1;
fi
