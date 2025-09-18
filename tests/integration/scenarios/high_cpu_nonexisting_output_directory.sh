#!/bin/bash
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
runProcDumpAndValidate=$(readlink -m "$DIR/../runProcDumpAndValidate.sh");
source $runProcDumpAndValidate

TESTPROGNAME="ProcDumpTestApplication"
TESTPROGMODE="cpu 90"

# These are all the ProcDump switches preceeding the PID
PREFIX="-c 80"

# This are all the ProcDump switches after the PID
POSTFIX=""

# Indicates whether the test should result in a dump or not
SHOULDDUMP=false

# The dump target
DUMPTARGET="missing_subdir/custom_dump_file"

runProcDumpAndValidate