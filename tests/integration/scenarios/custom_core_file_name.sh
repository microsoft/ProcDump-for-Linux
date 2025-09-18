#!/bin/bash
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
PROCDUMPPATH="$DIR/../../../procdump";
TESTPROGPATH="$DIR/../../../ProcDumpTestApplication";

customFileName="custom_dump_file"
dumpDir=$(mktemp -d -t dump_XXXXXX)
dumpTarget="$dumpDir/$customFileName"

$TESTPROGPATH "sleep" &
target_pid=$!

# starts ProcDump to dump using timer trigger and custom dump file name
echo [`date +"%T.%3N"`] "$PROCDUMPPATH $target_pid $dumpTarget"
$PROCDUMPPATH $target_pid $dumpTarget

kill -9 $target_pid

# asserts that a dump file with the custom name was created
foundFile=$(find "$dumpDir" -maxdepth 1 -name "$customFileName*" -print -quit)
if [[ -n $foundFile ]]; then
    echo $foundFile
    exit 0
else
    exit 1;
fi