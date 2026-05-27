# Max number of seconds for programs to be available in test run.
MAX_WAIT=60

#
# Waits until the specified URL is reachable.
#
function waitforurl {
  local url=$1

  i=0
  wget $url
  while  [ $? -ne 8 ]
  do
      ((i=i+1))
      if [[ "$i" -gt $MAX_WAIT ]]; then
          return -1
      fi
      sleep 1s
      wget $url
  done

  return 0
}

#
# Waits until the procdump child process has become available
#
function waitforprocdump {
  local -n result=$1

  i=0
  pid=$(ps -o pid= -C "procdump" | tr -d ' ')
  while [ ! $pid ]
  do
      ((i=i+1))
      if [[ "$i" -gt $MAX_WAIT ]]; then
          result=-1
          return
      fi
      sleep 1s
      pid=$(ps -o pid= -C "procdump" | tr -d ' ')
  done

  result=$pid
}

#
# Waits until the procdump status socket (in case of .net apps) is available
#
function waitforprocdumpsocket {
  local procdumpchildpid=$1
  local testchildpid=$2
  local -n result=$3

  ps -A -l

  if [[ -n "${TMPDIR:-}" ]];
  then
      tmpfolder=$TMPDIR
  else
      tmpfolder="/tmp"
  fi
  prefixname="/procdump/procdump-status-"
  socketpath=$tmpfolder$prefixname$procdumpchildpid"-"$testchildpid

  echo "ProcDump .NET status socket: "$socketpath
  echo "List of ProcDump sockets:"
  sudo ls /tmp/procdump

  i=0
  while [ ! -S $socketpath ]
  do
      ((i=i+1))
      if [[ "$i" -gt $MAX_WAIT ]]; then
        echo "List of ProcDump sockets:"
        sudo ls /tmp/procdump
        echo "ProcDump .NET status socket not available within alloted time"
        result=-1
        return
      fi
      sleep 1s
  done

  sudo ls /tmp/procdump
  echo "ProcDump .NET status socket found"
  result=$socketpath

  # wait for profile to be initialized
  search="Initialization complete (procdumppid=${procdumpchildpid},targetpid=${testchildpid})"
  (timeout "10s" tail -F -n +1 "/var/tmp/procdumpprofiler.log" &) | grep -q "$search"
  if [ $? -eq 0 ]; then
      result=$socketpath
      return
  else
      echo "Timeout reached. Unable to find '$search' in '/var/tmp/procdumpprofiler.log'"
      result=-1
      return
  fi
}

#
# wait for at least N dumps with a certain pattern name to be created/written
#
function waitforndumps {
  local expecteddumps=$1
  local dumppattern=$2
  local -n result=$3

  local _old_nullglob=$(shopt -p nullglob)
  shopt -s nullglob
  for i in {1..30}; do
    files=( $dumppattern )
    result=${#files[@]}
    if [ "$result" -ge "$expecteddumps" ]; then
        echo "[script] Dump was written..."
        break
    fi
    echo "[script] Waiting for dump to be written..."
    sleep 1
  done
  $_old_nullglob
}

#
# Validate core dump size against a gcore reference dump.
# Returns 0 if within tolerance, 1 otherwise.
# Usage: validatedumpsize <corex_dump> <gcore_ref_dump> <tolerance_percent>
#
function validatedumpsize {
  local corex_dump=$1
  local gcore_dump=$2
  local tolerance=$3

  if [ ! -f "$corex_dump" ] || [ ! -f "$gcore_dump" ]; then
    echo "[validate] ERROR: dump file(s) not found"
    return 1
  fi

  local corex_size
  local gcore_size
  if [[ "$(uname -s)" == "Darwin" ]]; then
    corex_size=$(stat -f%z "$corex_dump")
    gcore_size=$(stat -f%z "$gcore_dump")
  else
    corex_size=$(stat -c%s "$corex_dump")
    gcore_size=$(stat -c%s "$gcore_dump")
  fi

  if [ "$gcore_size" -eq 0 ]; then
    echo "[validate] ERROR: gcore reference dump is empty"
    return 1
  fi

  # Calculate percentage difference
  local diff
  if [ "$corex_size" -gt "$gcore_size" ]; then
    diff=$(( (corex_size - gcore_size) * 100 / gcore_size ))
  else
    diff=$(( (gcore_size - corex_size) * 100 / gcore_size ))
  fi

  echo "[validate] Size check: corex=${corex_size} gcore=${gcore_size} diff=${diff}% tolerance=${tolerance}%"

  if [ "$diff" -gt "$tolerance" ]; then
    echo "[validate] FAIL: dump size difference ${diff}% exceeds ${tolerance}% tolerance"
    return 1
  fi

  echo "[validate] PASS: dump sizes within tolerance"
  return 0
}

#
# Validate core dump content using GDB.
# Checks that the marker variable and shared libraries are present.
# Usage: validatedumpcontent <dump_file> <executable_path> <gdb_script_path>
# Returns 0 on success, 1 on failure.
#
function validatedumpcontent {
  local dump_file=$1
  local exec_path=$2
  local gdb_script=$3

  if [ ! -f "$dump_file" ]; then
    echo "[validate] ERROR: dump file not found: $dump_file"
    return 1
  fi

  local gdb_output
  gdb_output=$(gdb -batch -x "$gdb_script" -c "$dump_file" "$exec_path" 2>&1)
  local gdb_rc=$?

  # Check marker value (0xDEADBEEF = 3735928559)
  if echo "$gdb_output" | grep -q "MARKER=3735928559"; then
    echo "[validate] PASS: marker value correct (0xDEADBEEF)"
  else
    echo "[validate] FAIL: marker value not found or incorrect"
    echo "[validate] GDB output: $gdb_output"
    return 1
  fi

  # Check that libc appears in shared library list
  if echo "$gdb_output" | sed -n '/SHAREDLIBS_START/,/SHAREDLIBS_END/p' | grep -q "libc"; then
    echo "[validate] PASS: shared libraries resolved (libc found)"
  else
    echo "[validate] FAIL: libc not found in shared library list"
    echo "[validate] GDB output: $gdb_output"
    return 1
  fi

  # Check that backtrace contains 'main' (proves symbol resolution works)
  if echo "$gdb_output" | sed -n '/BACKTRACE_START/,/BACKTRACE_END/p' | grep -q "main"; then
    echo "[validate] PASS: backtrace contains main"
  else
    echo "[validate] FAIL: backtrace does not contain main"
    echo "[validate] GDB output: $gdb_output"
    return 1
  fi

  # If caller specified expected functions, check for each one
  shift 3
  for func in "$@"; do
    if echo "$gdb_output" | sed -n '/BACKTRACE_START/,/BACKTRACE_END/p' | grep -q "$func"; then
      echo "[validate] PASS: backtrace contains $func"
    else
      echo "[validate] FAIL: backtrace does not contain $func"
      echo "[validate] GDB output: $gdb_output"
      return 1
    fi
  done

  return 0
}

