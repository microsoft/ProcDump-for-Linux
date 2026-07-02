#!/bin/bash
#
# Library API: basic on-demand dump of a live target process.
# Exercises pdWriteDump happy path and validates the produced dump.
#
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
runLibTestAndValidate=$(readlink -m "$DIR/../runLibTestAndValidate.sh");
source $runLibTestAndValidate

LIBTEST_PID="target"
LIBTEST_PATH="dump"
EXPECTSUCCESS=true
SHOULDDUMP=true
VALIDATE_SIZE=true
VALIDATE_CONTENT=true

runLibTestAndValidate
