#!/bin/bash
#
# Library API: dumping a process id that is not running fails gracefully.
#
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
runLibTestAndValidate=$(readlink -m "$DIR/../runLibTestAndValidate.sh");
source $runLibTestAndValidate

LIBTEST_PID="dead"
LIBTEST_PATH="dump"
EXPECTSUCCESS=false
SHOULDDUMP=false

runLibTestAndValidate
