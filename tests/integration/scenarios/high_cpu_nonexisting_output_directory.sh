#!/bin/bash
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
runProcDumpAndValidate=$(readlink -m "$DIR/../runProcDumpAndValidate.sh");
source $runProcDumpAndValidate

stressPercentage=90
procDumpType="-C"
procDumpTrigger=80
shouldDump=false
customDumpFileName="missing_subdir/custom_dump_file"

runProcDumpAndValidate $stressPercentage $procDumpType $procDumpTrigger $shouldDump "CPU" $customDumpFileName
