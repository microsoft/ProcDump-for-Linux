#!/bin/bash
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/helpers.sh"

function runProcDumpAndValidate {
	DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
	PROCDUMPPATH="$DIR/../../procdump";
	GDBSCRIPT="$DIR/validate_dump.gdb"

	OS=$(uname -s)

	# In cases where the previous scenario is still writing a dump we simply want to kill it
	pkill -9 gdb > /dev/null

	# Make absolutely sure we cleanup dumps from prior run
	rm -rf /tmp/dump_*

	dumpDir=$(mktemp -d -t dump_XXXXXX)
	cd $dumpDir

	dumpParam=""
	if [ -n "$DUMPTARGET" ]; then
		dumpParam="$dumpDir/$DUMPTARGET"
	fi

	# Launch target process
	echo [`date +"%T.%3N"`] Starting $TESTPROGNAME
	if [ "$OS" = "Darwin" ]; then
		TESTPROGPATH=$DIR/../../$TESTPROGNAME;
	else
		TESTPROGPATH=$(readlink -m "$DIR/../../$TESTPROGNAME");
	fi
	# Break out the arguments in case there are spaces (e.g. mem 100M)
	read -r -a _args <<< "$TESTPROGMODE"
	($TESTPROGPATH "${_args[@]}") &
	pid=$!
	echo "Test App: $TESTPROGPATH ${_args[@]}"
	echo "PID: $pid"

	# Give the test program time to start and stabilize
	if [ -n "$STABILIZATION_SLEEP" ]; then
		sleep $STABILIZATION_SLEEP
	elif $SHOULDDUMP; then
		sleep 3
	else
		sleep 1
	fi
	
	# Launch procdump in background using either wait by name or target PID
	echo [`date +"%T.%3N"`] Starting ProcDump
	if [[ "$PROCDUMPWAITBYNAME" == "true" ]]; then
		launchMode="-w $TESTPROGNAME"
	else
		launchMode=$pid
	fi
	echo "$PROCDUMPPATH -log stdout $PREFIX $launchMode $POSTFIX $dumpParam"
	$PROCDUMPPATH -log stdout $PREFIX $launchMode $POSTFIX $dumpParam&
	pidPD=$!
	echo "ProcDump PID: $pidPD"

	# Wait for ProcDump to exit; notdump tests use a shorter timeout
	if [ -z "$TIMEOUT" ]; then
		if $SHOULDDUMP; then
			timeout=30
		else
			timeout=10
		fi
	else
		timeout=$TIMEOUT
	fi
	end=$((SECONDS + timeout))
	while ps -p "$pidPD" >/dev/null && [ $SECONDS -lt $end ]; do
		sleep 1
	done
	
	if ps -p $pidPD > /dev/null
	then
		echo [`date +"%T.%3N"`] Killing ProcDump: $pidPD
		kill -9 $pidPD > /dev/null
	fi

	# Determine if this is a native (non-.NET) test that expects dumps
	isNativeTest=false
	if [[ "$TESTPROGNAME" == "ProcDumpTestApplication" ]] && $SHOULDDUMP; then
		isNativeTest=true
	fi

	# Generate gcore reference dump while the test process is still alive
	gcoreRefDump=""
	if $isNativeTest && ps -p $pid > /dev/null 2>&1; then
		gcoreRefDir=$(mktemp -d -t gcoreref_XXXXXX)
		gcoreRefDump="$gcoreRefDir/refcore.$pid"
		echo [`date +"%T.%3N"`] Generating gcore reference dump for PID $pid
		timeout 30 gdb -batch -ex "gcore $gcoreRefDump" -p $pid > /dev/null 2>&1
		if [ ! -f "$gcoreRefDump" ]; then
			echo "[validate] WARNING: gcore reference dump was not generated"
			gcoreRefDump=""
		fi
	fi

	if ps -p $pid > /dev/null
	then
		echo [`date +"%T.%3N"`] Killing Test Program: $pid
		kill -9 $pid > /dev/null
	fi

	# If we are checking restrack results
	if [[ $PREFIX == *"-restrack"* ]]; then
		foundFile=$(find "$dumpDir" -mindepth 1 -name "*.restrack" -print -quit)
		if [[ -n $foundFile ]]; then
			pwd
			if [ $(stat -c%s "$foundFile") -gt 19 ]; then
				exit 0
			fi
		fi
		exit 1;
	fi

	# We're checking dump results
	if find "$dumpDir" -mindepth 1 -print -quit | grep -q .; then
		if $SHOULDDUMP; then
			# Dump validation for native (non-.NET) tests
			if $isNativeTest; then
				# Find the first dump file
				corexDump=$(find "$dumpDir" -mindepth 1 -maxdepth 1 -type f ! -name "*.restrack" -print -quit)

				if [ -n "$corexDump" ]; then
					# 1. Size comparison against gcore reference
					if [ -n "$gcoreRefDump" ] && [ -f "$gcoreRefDump" ]; then
						if ! validatedumpsize "$corexDump" "$gcoreRefDump" 20; then
							echo "[validate] FAIL: dump size validation failed"
							exit 1
						fi
					else
						echo "[validate] SKIP: no gcore reference dump available for size comparison"
					fi

					# 2. GDB content validation (marker + shared libs + backtrace)
					if [ -f "$GDBSCRIPT" ]; then
						# Determine expected function names based on test mode
						# Only cpu mode keeps stress_cpu on the stack (infinite loop).
						# mem/thread modes return from their functions before the dump.
						local expected_funcs=()
						case "$TESTPROGMODE" in
							cpu*)    expected_funcs=("stress_cpu") ;;
						esac
						if ! validatedumpcontent "$corexDump" "$TESTPROGPATH" "$GDBSCRIPT" "${expected_funcs[@]}"; then
							echo "[validate] FAIL: dump content validation failed"
							exit 1
						fi
					else
						echo "[validate] SKIP: GDB validation script not found"
					fi
				fi
			fi

			exit 0
		else
			exit 1
		fi
	else
		if $SHOULDDUMP; then
			exit 1
		else
			exit 0
		fi
	fi
}
