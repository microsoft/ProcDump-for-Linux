#!/bin/bash

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
TESTPATH=$(readlink -m "$DIR/../../../WaitForMultipleObjectsCleanupTest")

if [ ! -x "$TESTPATH" ]; then
	echo "WaitForMultipleObjectsCleanupTest not found or not executable: $TESTPATH"
	exit 1
fi

timeout 15 "$TESTPATH"