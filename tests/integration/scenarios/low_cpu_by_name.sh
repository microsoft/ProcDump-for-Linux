#!/bin/bash
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
runProcDumpAndValidate=$(readlink -m "$DIR/../runProcDumpAndValidate.sh");
TESTPROGNAME="ProcDumpTestApplication"
TESTPROGMODE="sleep"
source $runProcDumpAndValidate

# These are all the ProcDump switches preceeding the PID
PREFIX="-cl 20"

# This are all the ProcDump switches after the PID
POSTFIX=""

# Indicates whether the test should result in a dump or not
SHOULDDUMP=true

# The dump target
DUMPTARGET=""

runProcDumpAndValidate
