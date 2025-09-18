#!/bin/bash
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
runProcDumpAndValidate=$(readlink -m "$DIR/../runProcDumpAndValidate.sh");
source $runProcDumpAndValidate

TESTPROGNAME="ProcDumpTestApplication"
TESTPROGMODE="burn"

# This are all the ProcDump switches preceeding the target
PREFIX="-c 50"

# ProcDump should wait for the process by name (-w) instead of by PID
PROCDUMPWAITBYNAME="true"

# This are all the ProcDump switches after the target
POSTFIX=""

# Indicates whether the test should result in a dump or not
SHOULDDUMP=true

# The dump target
DUMPTARGET=""

runProcDumpAndValidate