#!/bin/bash
#
# Library API: writing to a dump path under a non-existent directory fails.
#
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
runLibTestAndValidate=$(readlink -m "$DIR/../runLibTestAndValidate.sh");
source $runLibTestAndValidate

LIBTEST_PID="target"
LIBTEST_PATH="baddir"
EXPECTSUCCESS=false
SHOULDDUMP=false

runLibTestAndValidate
