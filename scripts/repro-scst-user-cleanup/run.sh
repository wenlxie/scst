#!/usr/bin/env bash
# Run inside a disposable Linux VM with this SCST tree's modules installed.
set -euo pipefail

if [[ ${SCST_REPRO_DISPOSABLE:-} != 1 || $(id -u) != 0 ]]; then
	echo 'Run as root in a disposable VM with SCST_REPRO_DISPOSABLE=1.' >&2
	exit 2
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "$script_dir/../.." && pwd)
run_dir=${SCST_REPRO_DIR:-/var/tmp/scst-user-cleanup-repro}
mkdir -p "$run_dir"
state_log="$run_dir/state.log"
: > "$state_log"

snapshot_state() {
	local phase=$1 device commands
	{
		echo "=== $phase $(date -Is) ==="
		ps -e -o pid,stat,comm,wchan:24 | grep -E 'PID|scst_usr_' || true
		for device in repro_a repro_b; do
			commands="/sys/kernel/scst_tgt/devices/$device/commands"
			if [[ -r "$commands" ]]; then
				echo "=== $device commands ==="
				cat "$commands"
			else
				echo "=== $device commands unavailable ==="
			fi
		done
	} >> "$state_log"
}

cc -O2 -Wall -Wextra -I"$repo_dir/scst/include" -shared -fPIC \
	"$script_dir/hold_reused_buffer.c" -ldl -o "$run_dir/hold_reused_buffer.so"
cc -O2 -Wall -Wextra "$script_dir/sg_read4k.c" -o "$run_dir/sg_read4k"
cc -O2 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE \
	-I"$repo_dir/scst/include" -I"$repo_dir/usr/include" \
	-I"$repo_dir/usr/fileio" \
	"$repo_dir/usr/fileio/fileio.c" "$repo_dir/usr/fileio/common.c" \
	"$repo_dir/usr/fileio/debug.c" "$repo_dir/usr/fileio/crc32.c" \
	-lpthread -o "$run_dir/fileio_tgt"

modprobe sg
for module in scst scst_user scst_local; do
	if [[ ! -d /sys/module/$module ]]; then
		modprobe "$module"
	fi
done
test -e /dev/scst_user
mkdir -p /var/lib/scst/pr

truncate -s 2M "$run_dir/a.img"
truncate -s 2M "$run_dir/b.img"
LD_PRELOAD="$run_dir/hold_reused_buffer.so" "$run_dir/fileio_tgt" \
	--sgv_shared --sgv_disable_clustered_pool --non_blocking \
	--threads=1 --multi_cmd=0 \
	repro_a "$run_dir/a.img" repro_b "$run_dir/b.img" \
	>"$run_dir/backend.log" 2>&1 &
backend_pid=$!
trap 'kill -KILL "$backend_pid" 2>/dev/null || true' EXIT
echo "Backend PID: $backend_pid"

for attempt in {1..100}; do
	if [[ -d /sys/kernel/scst_tgt/handlers/repro_a && \
	      -d /sys/kernel/scst_tgt/handlers/repro_b ]]; then
		break
	fi
	if ! kill -0 "$backend_pid" 2>/dev/null; then
		cat "$run_dir/backend.log" >&2
		exit 1
	fi
	sleep 0.1
done

mgmt=/sys/kernel/scst_tgt/targets/scst_local/scst_local_tgt/luns/mgmt
test -w "$mgmt"
echo 'add repro_a 0' > "$mgmt"
echo 'add repro_b 1' > "$mgmt"

sg_for_model() {
	local model=$1 sg candidate
	for candidate in /sys/class/scsi_generic/sg*; do
		[[ -f "$candidate/device/model" ]] || continue
		read -r sg < "$candidate/device/model"
		if [[ "$sg" == "$model" ]]; then
			printf '/dev/%s\n' "${candidate##*/}"
			return 0
		fi
	done
	return 1
}

sg_a= sg_b=
for attempt in {1..100}; do
	sg_a=$(sg_for_model repro_a || true)
	sg_b=$(sg_for_model repro_b || true)
	[[ -n "$sg_a" && -n "$sg_b" ]] && break
	sleep 0.1
done
if [[ -z "$sg_a" || -z "$sg_b" ]]; then
	echo 'Could not find both scst_local SCSI generic devices.' >&2
	exit 1
fi
echo "Device A: $sg_a; device B: $sg_b"

"$run_dir/sg_read4k" "$sg_a" 123 | tee "$run_dir/read-a.log"
"$run_dir/sg_read4k" "$sg_a" 123 | tee -a "$run_dir/read-a.log"
sleep 0.2
"$run_dir/sg_read4k" "$sg_b" 124 >"$run_dir/read-b.log" 2>&1 &

for attempt in {1..100}; do
	if grep -q 'SCST_REPRO_A_HANDLE_CLOSED' "$run_dir/backend.log"; then
		break
	fi
	if ! kill -0 "$backend_pid" 2>/dev/null; then
		cat "$run_dir/backend.log" >&2
		exit 1
	fi
	sleep 0.1
done
if ! grep -q 'SCST_REPRO_A_HANDLE_CLOSED' "$run_dir/backend.log"; then
	echo 'A did not close while B held its buffer; this run is inconclusive.' >&2
	exit 1
fi

echo 'Confirmed: B holds A buffer while A handle is closed.'
snapshot_state after_a_handle_closed
sleep "${SCST_REPRO_HOLD_SECONDS:-2}"
if [[ -d /sys/kernel/scst_tgt/handlers/repro_a ]]; then
	echo 'A handler is still present while B holds its buffer.'
else
	echo 'A handler was removed; check state.log for release-thread status.'
fi
snapshot_state before_b_handle_closed
echo 'Killing B handle.'
kill -KILL "$backend_pid"
wait "$backend_pid" 2>/dev/null || true
trap - EXIT

# An unfixed kernel can soft-lock up while A waits. A corrected kernel should
# finish both release threads and reach the success marker below.
for attempt in {1..100}; do
	if [[ ! -d /sys/kernel/scst_tgt/handlers/repro_a &&
	      ! -d /sys/kernel/scst_tgt/handlers/repro_b ]] &&
	   ! ps -e -o comm= | grep -q '^scst_usr_releas'; then
		echo 'SCST_REPRO_CLEANUP_COMPLETE'
		snapshot_state cleanup_complete
		dmesg > "$run_dir/dmesg.log"
		exit 0
	fi
	sleep 0.1
done

echo 'SCST_REPRO_CLEANUP_TIMEOUT: release threads remain after 10 seconds.' >&2
snapshot_state cleanup_timeout
dmesg > "$run_dir/dmesg.log"
exit 1
