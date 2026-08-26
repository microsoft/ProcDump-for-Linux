#!/bin/bash
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROCDUMPPATH=$(readlink -m "$DIR/$1")
TESTWEBAPIPATH=$(readlink -m "$DIR/../TestWebApi")
HELPERS=$(readlink -m "$DIR/../helpers.sh")

source "$HELPERS"

dumpDir=$(mktemp -d -t dotnet_no_overwrite_XXXXXX)
dumpBase="$dumpDir/core"
expectedDump="${dumpBase}_1"
monitorLog=$(mktemp -t dotnet_no_overwrite_XXXXXX)
printf 'existing\n' >"$expectedDump"

cleanup() {
    [ -n "${monitor_pid:-}" ] && sudo kill -9 "$monitor_pid" 2>/dev/null
    [ -n "${target_pid:-}" ] && kill -9 "$target_pid" 2>/dev/null
    [ -n "${dotnet_pid:-}" ] && kill -9 "$dotnet_pid" 2>/dev/null
}
trap cleanup EXIT

pushd "$TESTWEBAPIPATH" >/dev/null
dotnet run --urls=http://localhost:5032 &
dotnet_pid=$!
waitforurl http://localhost:5032/throwinvalidoperation
target_pid=$(pgrep -n -x TestWebApi)

sudo "$PROCDUMPPATH" -log stdout -gcm 10 "$target_pid" "$dumpBase" >"$monitorLog" 2>&1 &
sudo_pid=$!
for _ in $(seq 1 30); do
    monitor_pid=$(pgrep -n -x procdump)
    if [ -n "$monitor_pid" ] && [ -f "/tmp/procdump/procdump-ready-${monitor_pid}-${target_pid}" ]; then
        break
    fi
    sleep 1
done

wget -O /dev/null http://localhost:5032/memincrease
for _ in $(seq 1 30); do
    if ! kill -0 "$sudo_pid" 2>/dev/null; then
        break
    fi
    sleep 1
done
wait "$sudo_pid"
status=$?
popd >/dev/null

if [ "$status" -eq 0 ]; then
    echo "TEST FAILED: managed dump unexpectedly overwrote an existing path"
    cat "$monitorLog"
    exit 1
fi
if [ "$(cat "$expectedDump")" != "existing" ]; then
    echo "TEST FAILED: existing managed dump content changed"
    exit 1
fi
if [ "$(find "$dumpDir" -maxdepth 1 -type f | wc -l)" -ne 1 ]; then
    echo "TEST FAILED: managed dump failure left an unexpected artifact"
    exit 1
fi
exit 0