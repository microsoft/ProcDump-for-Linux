#!/bin/bash
#
# Library API: a negative processId is rejected with an error.
#
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
runLibTestAndValidate=$(readlink -m "$DIR/../runLibTestAndValidate.sh");
source $runLibTestAndValidate

LIBTEST_PID="invalid"
LIBTEST_PATH="dump"
EXPECTSUCCESS=false
SHOULDDUMP=false

runLibTestAndValidate
