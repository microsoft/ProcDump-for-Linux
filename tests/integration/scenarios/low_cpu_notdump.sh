#!/bin/bash
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
runProcDumpAndValidate=$(readlink -m "$DIR/../runProcDumpAndValidate.sh");
source $runProcDumpAndValidate

TESTPROGNAME="ProcDumpTestApplication"
TESTPROGMODE="burn"

# burn needs extra warm-up so lifetime-average CPU exceeds the -cl threshold
STABILIZATION_SLEEP=5

# These are all the ProcDump switches preceeding the PID
PREFIX="-cl 10"

# This are all the ProcDump switches after the PID
POSTFIX=""

# Indicates whether the test should result in a dump or not
SHOULDDUMP=false

# The dump target
DUMPTARGET=""

runProcDumpAndValidate