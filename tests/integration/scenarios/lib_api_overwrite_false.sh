#!/bin/bash
#
# Library API: with bOverwrite=false pdWriteDump fails when the dump file
# already exists, leaving the existing file untouched.
#
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
runLibTestAndValidate=$(readlink -m "$DIR/../runLibTestAndValidate.sh");
source $runLibTestAndValidate

LIBTEST_PID="target"
LIBTEST_PATH="dump"
LIBTEST_OVERWRITE=0
PRECREATE_DUMP=true
EXPECTSUCCESS=false
SHOULDDUMP=false

runLibTestAndValidate
