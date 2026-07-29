#!/bin/bash
#
# Library API: an empty dumpPath is rejected with an error.
#
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
runLibTestAndValidate=$(readlink -m "$DIR/../runLibTestAndValidate.sh");
source $runLibTestAndValidate

LIBTEST_PID="target"
LIBTEST_PATH="EMPTY"
EXPECTSUCCESS=false
SHOULDDUMP=false

runLibTestAndValidate
