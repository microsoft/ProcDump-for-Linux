#!/bin/bash
function runProcDumpAndValidate {
	DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )";
	PROCDUMPPATH="$DIR/../../procdump";

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

	if [ -z "$TESTPROGNAME" ]; then
  	    echo [`date +"%T.%3N"`] Starting stress-ng
		if [ "$RESTYPE" == "MEM" ]; then
			stress-ng --vm 1 --vm-hang 0 --vm-bytes $TARGETVALUE -q&
		else
			stress-ng -c 1 -l $TARGETVALUE -q&
		fi
		pid=$!
		echo "PID: $pid"

	    # Give test app opportunity to start and get into scenario state
		sleep 5
		echo [`date +"%T.%3N"`] Done waiting for stress-ng to start

		childrenpid=""
		if [ "$OS" = "Darwin" ]; then
			childrenpid=$(pgrep stress-ng | grep -v "^$pid$")
		else    
			childrenpid=$(pidof -o $pid $(which stress-ng))
		fi

		echo "ChildrenPID: $childrenpid"

		childpid=$(echo $childrenpid | cut -d " " -f1)
		echo "ChildPID: $childpid"

		# We launch procdump in background and wait for 10 secs to complete the monitoring
		echo "$PROCDUMPPATH -log stdout $PREFIX $childpid $POSTFIX $dumpParam "
		echo [`date +"%T.%3N"`] Starting ProcDump
		$PROCDUMPPATH -log stdout $PREFIX $childpid $POSTFIX $dumpParam&
		pidPD=$!
		echo "ProcDump PID: $pidPD"
		sleep 30
		echo [`date +"%T.%3N"`] Killing ProcDump
	    if ps -p $pidPD > /dev/null
	    then
		    kill -9 $pidPD > /dev/null
	    fi
	    if ps -p $childpid > /dev/null
	    then
		    kill -9 $childpid > /dev/null
	    fi
	else
		# We launch procdump in background and wait for target process to start
		echo [`date +"%T.%3N"`] Starting ProcDump
		echo "$PROCDUMPPATH -log stdout $PREFIX -w $TESTPROGNAME" $POSTFIX $dumpParam
		$PROCDUMPPATH -log stdout $PREFIX -w "$TESTPROGNAME" $POSTFIX $dumpParam&
		pidPD=$!
		echo "ProcDump PID: $pidPD"

		# Wait for procdump to initialize
		sleep 10

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

		sleep 30
	    if ps -p $pidPD > /dev/null
	    then
			echo [`date +"%T.%3N"`] Killing ProcDump: $pidPD
		    kill -9 $pidPD > /dev/null
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
