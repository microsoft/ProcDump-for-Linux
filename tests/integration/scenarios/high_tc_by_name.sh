#!/bin/bash
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
OS=$(uname -s)
if [ "$OS" = "Darwin" ]; then
    runProcDumpAndValidate=$DIR/../runProcDumpAndValidate.sh;
else
    runProcDumpAndValidate=$(readlink -m "$DIR/../runProcDumpAndValidate.sh");    
fi

source $runProcDumpAndValidate

TESTPROGNAME="ProcDumpTestApplication"
TESTPROGMODE="tc"

# These are all the ProcDump switches preceeding the PID
PREFIX="-tc 50"

# ProcDump should wait for the process by name (-w) instead of by PID
PROCDUMPWAITBYNAME="true"

# This are all the ProcDump switches after the PID
POSTFIX=""

# Indicates whether the test should result in a dump or not
SHOULDDUMP=true

# The dump target
DUMPTARGET=""

runProcDumpAndValidate