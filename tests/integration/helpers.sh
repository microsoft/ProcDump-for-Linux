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

  for i in {1..15}; do
      result=( $(ls $dumppattern | wc -l) )
      if [[ "$result" -ge $expecteddumps ]]; then
          echo "[script] Dump was written..."
          break
      fi
      echo "[script] Waiting for dump to be written..."
      sleep 1
  done
}
