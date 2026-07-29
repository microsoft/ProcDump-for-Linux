#!/bin/bash
#
# Library API: an explicit coredump_filter bitmask is honored by pdWriteDump.
# 0x7f enables all standard mapping types.
#
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
runLibTestAndValidate=$(readlink -m "$DIR/../runLibTestAndValidate.sh");
source $runLibTestAndValidate

LIBTEST_PID="target"
LIBTEST_PATH="dump"
LIBTEST_MASK="0x7f"
EXPECTSUCCESS=true
SHOULDDUMP=true

runLibTestAndValidate
