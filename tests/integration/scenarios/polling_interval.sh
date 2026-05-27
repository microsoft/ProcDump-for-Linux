#!/bin/bash
# Test: -pf (polling frequency) controls how often ProcDump checks thresholds
# Uses a fast polling interval (2000ms) and verifies dump is still triggered correctly
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
runProcDumpAndValidate=$(readlink -m "$DIR/../runProcDumpAndValidate.sh");
source $runProcDumpAndValidate

TESTPROGNAME="ProcDumpTestApplication"
TESTPROGMODE="mem 90M"

# -pf 2000 sets polling interval to 2s (slower than default 1000ms)
# Use memory trigger since it's not timing-sensitive like CPU
PREFIX="-m 50 -pf 2000"

POSTFIX=""

SHOULDDUMP=true

DUMPTARGET=""

runProcDumpAndValidate
