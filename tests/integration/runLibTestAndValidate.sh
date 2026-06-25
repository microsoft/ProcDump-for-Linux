#!/bin/bash
#
# runLibTestAndValidate.sh
#
# Drives the ProcDump on-demand dump library API (pdWriteDump / pdFreeError)
# through the ProcDumpLibTestDriver binary and validates the outcome. This is
# the library-API counterpart of runProcDumpAndValidate.sh and is sourced by
# the lib_api_*.sh scenarios.
#
# Scenario-tunable variables (all optional, sensible defaults shown):
#
#   LIBTEST_PID        target  - spawn ProcDumpTestApplication and dump it (default)
#                      invalid - pass -1 (negative test)
#                      zero    - pass 0  (negative test)
#                      dead    - pass the pid of a process that has already exited
#                      <num>   - pass a literal pid
#
#   LIBTEST_PATH       dump      - $dumpDir/core (default)
#                      NULL      - pass a NULL pointer (negative test)
#                      EMPTY     - pass an empty string (negative test)
#                      baddir    - a path under a non-existent directory (negative test)
#
#   LIBTEST_MASK       default (default) or a coredump_filter bitmask value
#   LIBTEST_OVERWRITE  1 (default) or 0
#   PRECREATE_DUMP     true to create the dump file before running (overwrite tests)
#
#   EXPECTSUCCESS      true (default) - expect pdWriteDump to return 0
#                      false          - expect pdWriteDump to return non-zero
#   SHOULDDUMP         defaults to EXPECTSUCCESS - whether a dump file must exist
#   VALIDATE_SIZE      true to compare dump size against a gcore reference
#   VALIDATE_CONTENT   true to validate dump content with GDB
#
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/helpers.sh"

function runLibTestAndValidate {
	DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
	DRIVERPATH="${DRIVERPATH:-$DIR/../../ProcDumpLibTestDriver}";
	TESTPROGPATH=$(readlink -m "$DIR/../../ProcDumpTestApplication");
	GDBSCRIPT="$DIR/validate_dump.gdb"

	# Defaults
	LIBTEST_PID="${LIBTEST_PID:-target}"
	LIBTEST_PATH="${LIBTEST_PATH:-dump}"
	LIBTEST_MASK="${LIBTEST_MASK:-default}"
	LIBTEST_OVERWRITE="${LIBTEST_OVERWRITE:-1}"
	EXPECTSUCCESS="${EXPECTSUCCESS:-true}"
	SHOULDDUMP="${SHOULDDUMP:-$EXPECTSUCCESS}"
	VALIDATE_SIZE="${VALIDATE_SIZE:-false}"
	VALIDATE_CONTENT="${VALIDATE_CONTENT:-false}"
	PRECREATE_DUMP="${PRECREATE_DUMP:-false}"

	if [ ! -x "$DRIVERPATH" ]; then
		echo "[libtest] ERROR: driver not found or not executable: $DRIVERPATH"
		exit 1
	fi

	# Clean up dumps from a prior run
	pkill -9 gdb > /dev/null 2>&1
	rm -rf /tmp/libdump_*

	dumpDir=$(mktemp -d -t libdump_XXXXXX)
	cd "$dumpDir"

	# Spawn the target process when one is required.
	targetPid=""
	if [ "$LIBTEST_PID" = "target" ]; then
		echo [`date +"%T.%3N"`] Starting "$TESTPROGPATH" burn
		("$TESTPROGPATH" burn) &
		targetPid=$!
		echo "Target PID: $targetPid"
		sleep 3
	fi

	# Resolve the pid argument passed to the driver.
	case "$LIBTEST_PID" in
		target)  pidArg=$targetPid ;;
		invalid) pidArg=-1 ;;
		zero)    pidArg=0 ;;
		dead)
			# Start and immediately reap a short-lived process so its pid is stale.
			(true) &
			pidArg=$!
			wait $pidArg 2>/dev/null
			;;
		*)       pidArg="$LIBTEST_PID" ;;
	esac

	# Resolve the path argument passed to the driver.
	case "$LIBTEST_PATH" in
		dump)   pathArg="$dumpDir/core" ;;
		NULL)   pathArg="NULL" ;;
		EMPTY)  pathArg="EMPTY" ;;
		baddir) pathArg="$dumpDir/no_such_dir/core" ;;
		*)      pathArg="$LIBTEST_PATH" ;;
	esac

	# The library appends ".<pid>" to the supplied path.
	expectedDump=""
	if [ "$pathArg" != "NULL" ] && [ "$pathArg" != "EMPTY" ]; then
		expectedDump="${pathArg}.${pidArg}"
	fi

	# Optionally pre-create the dump file (overwrite tests).
	if [ "$PRECREATE_DUMP" = "true" ] && [ -n "$expectedDump" ]; then
		echo "[libtest] Pre-creating dump file: $expectedDump"
		echo "stale" > "$expectedDump"
	fi

	# Optionally produce a gcore reference dump for size comparison.
	gcoreRefDump=""
	if [ "$VALIDATE_SIZE" = "true" ] && [ -n "$targetPid" ] && ps -p "$targetPid" > /dev/null 2>&1; then
		gcoreRefDir=$(mktemp -d -t gcoreref_XXXXXX)
		gcoreRefDump="$gcoreRefDir/refcore.$targetPid"
		echo [`date +"%T.%3N"`] Generating gcore reference dump for PID "$targetPid"
		timeout 30 gdb -batch -ex "gcore $gcoreRefDump" -p "$targetPid" > /dev/null 2>&1
		[ -f "$gcoreRefDump" ] || gcoreRefDump=""
	fi

	# Run the driver.
	echo [`date +"%T.%3N"`] Running: "$DRIVERPATH" "$pidArg" "$pathArg" "$LIBTEST_MASK" "$LIBTEST_OVERWRITE"
	"$DRIVERPATH" "$pidArg" "$pathArg" "$LIBTEST_MASK" "$LIBTEST_OVERWRITE"
	rc=$?
	echo "[libtest] driver exit code: $rc"

	# Stop the target process.
	if [ -n "$targetPid" ] && ps -p "$targetPid" > /dev/null 2>&1; then
		kill -9 "$targetPid" > /dev/null 2>&1
	fi

	result=0

	# 1. Validate the return code against expectation.
	if [ "$EXPECTSUCCESS" = "true" ]; then
		if [ "$rc" -ne 0 ]; then
			echo "[libtest] FAIL: expected success but driver returned $rc"
			result=1
		else
			echo "[libtest] PASS: driver returned success as expected"
		fi
	else
		if [ "$rc" -eq 0 ]; then
			echo "[libtest] FAIL: expected failure but driver returned success"
			result=1
		else
			echo "[libtest] PASS: driver returned failure as expected"
		fi
	fi

	# 2. Validate dump file presence/absence.
	if [ "$SHOULDDUMP" = "true" ]; then
		if [ -n "$expectedDump" ] && [ -f "$expectedDump" ]; then
			echo "[libtest] PASS: dump file produced: $expectedDump"

			if [ "$VALIDATE_SIZE" = "true" ]; then
				if [ -n "$gcoreRefDump" ]; then
					if ! validatedumpsize "$expectedDump" "$gcoreRefDump" 20; then
						echo "[libtest] FAIL: dump size validation failed"
						result=1
					fi
				else
					echo "[libtest] SKIP: no gcore reference dump for size comparison"
				fi
			fi

			if [ "$VALIDATE_CONTENT" = "true" ] && [ -f "$GDBSCRIPT" ]; then
				if ! validatedumpcontent "$expectedDump" "$TESTPROGPATH" "$GDBSCRIPT"; then
					echo "[libtest] FAIL: dump content validation failed"
					result=1
				fi
			fi
		else
			echo "[libtest] FAIL: expected dump file not found: $expectedDump"
			result=1
		fi
	else
		# No dump expected. For overwrite=false tests a pre-existing file may remain,
		# but the library must not have produced a fresh, valid dump.
		if [ "$PRECREATE_DUMP" != "true" ] && [ -n "$expectedDump" ] && [ -f "$expectedDump" ]; then
			echo "[libtest] FAIL: unexpected dump file produced: $expectedDump"
			result=1
		else
			echo "[libtest] PASS: no unexpected dump produced"
		fi
	fi

	exit $result
}
