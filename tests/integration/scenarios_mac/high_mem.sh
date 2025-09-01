#!/bin/bash
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
runProcDumpAndValidate=$DIR/../runProcDumpAndValidate.sh;
source $runProcDumpAndValidate

TESTPROGNAME="ProcDumpTestApplication"
TESTPROGMODE="mem"

# TARGETVALUE is only used for stress-ng
#TARGETVALUE=3M

# These are all the ProcDump switches preceeding the PID
PREFIX="-m 1"

# This are all the ProcDump switches after the PID
POSTFIX=""

# Indicates whether the test should result in a dump or not
SHOULDDUMP=true

# The dump target
DUMPTARGET=""

runProcDumpAndValidate