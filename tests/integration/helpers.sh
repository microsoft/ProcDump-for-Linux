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

  if [[ -v TMPDIR ]];
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

  # wait for profile to be loaded
  for i in {1..15}; do
      PROF="$(cat /proc/${testchildpid}/maps | awk '{print $6}' | grep '\procdumpprofiler.so' | uniq)"
      if [[ "$PROF" == *"procdumpprofiler.so" ]]; then
          echo "[script] Profiler was loaded..."
          break
      fi
      echo "[script] Waiting for profiler to be loaded..."
      sleep 1
  done
}

#
# wait for dump to be created/written
#
function waitfordump {
  local dumppattern=$1
  local -n result=$2

  for i in {1..15}; do
      result=( $(ls $dumppattern | wc -l) )
      if [[ "$result" -eq 1 ]]; then
          echo "[script] Dump was written..."
          break
      fi
      echo "[script] Waiting for dump to be written..."
      sleep 1
  done
}
