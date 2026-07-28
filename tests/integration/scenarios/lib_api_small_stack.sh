#!/bin/bash
#
# Library API: invoke pdWriteDump from a worker with a 2 MiB stack.
# Regression test for large stack allocations in corex_dump_pid.
#
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
runLibTestAndValidate=$(readlink -m "$DIR/../runLibTestAndValidate.sh");
source $runLibTestAndValidate

LIBTEST_PID="target"
LIBTEST_PATH="dump"
LIBTEST_STACK_SIZE=$((2 * 1024 * 1024))
EXPECTSUCCESS=true
SHOULDDUMP=true
VALIDATE_SIZE=true
VALIDATE_CONTENT=true

runLibTestAndValidate