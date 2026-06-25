#!/bin/bash
#
# Library API: a zero processId is rejected with an error.
#
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
runLibTestAndValidate=$(readlink -m "$DIR/../runLibTestAndValidate.sh");
source $runLibTestAndValidate

LIBTEST_PID="zero"
LIBTEST_PATH="dump"
EXPECTSUCCESS=false
SHOULDDUMP=false

runLibTestAndValidate
