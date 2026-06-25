#!/bin/bash
#
# Library API: with bOverwrite=true an existing dump file is replaced.
#
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
runLibTestAndValidate=$(readlink -m "$DIR/../runLibTestAndValidate.sh");
source $runLibTestAndValidate

LIBTEST_PID="target"
LIBTEST_PATH="dump"
LIBTEST_OVERWRITE=1
PRECREATE_DUMP=true
EXPECTSUCCESS=true
SHOULDDUMP=true

runLibTestAndValidate
