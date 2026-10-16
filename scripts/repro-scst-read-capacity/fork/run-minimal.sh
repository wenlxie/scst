#!/usr/bin/env bash
# Minimal scst_user fork reproduction with upstream's sample handler (fileio_tgt).
# Disposable VM only, as root. SCSI I/O is read-only.
#
#   SCST_TREE=<built SCST tree> HOOK=<fork_once.so> [NO_FORK=1] bash run-minimal.sh
set -euo pipefail
[[ $(id -u) == 0 ]] || { echo 'run as root' >&2; exit 2; }

TREE=${SCST_TREE:?set SCST_TREE to a built SCST tree}
HOOK=${HOOK:?set HOOK to the built fork_once.so}
RUN=${RUN:-/var/tmp/scst-fork-repro}
NAME=forkdisk
mgmt=/sys/kernel/scst_tgt/targets/scst_local/scst_local_tgt/luns/mgmt
mkdir -p "$RUN" /var/lib/scst/pr
cd "$RUN"

fileio_pid=
lun_added=0
cleanup() {
	if [[ $lun_added == 1 ]]; then echo "del 0" > "$mgmt" || true; fi
	child=$(grep -o 'FORK_ONCE child=[0-9]*' fileio.log 2>/dev/null | cut -d= -f2 || true)
	[[ -n $child ]] && kill "$child" 2>/dev/null || true
	[[ -n $fileio_pid ]] && kill "$fileio_pid" 2>/dev/null || true
	wait 2>/dev/null || true
}
trap cleanup EXIT

lsmod | grep -q '^scst ' || insmod "$TREE/scst/src/scst.ko"
lsmod | grep -q '^scst_user ' || insmod "$TREE/scst/src/dev_handlers/scst_user.ko"
lsmod | grep -q '^scst_local ' || insmod "$TREE/scst_local/scst_local.ko"

# 32 MiB disk; block 0 holds a recognizable pattern, the rest is zero.
rm -f disk.img fork.trigger fileio.log
truncate -s 32M disk.img
printf 'SCSTDATA%.0s' $(seq 512) | dd of=disk.img conv=notrunc status=none

# Defaults: SCST_USER_MEM_REUSE_ALL, SCST_USER_ON_FREE_CMD_IGNORE, page-aligned buffers.
# One thread and one initiator: every command reuses the same cached buffer.
FORK_TRIGGER="$RUN/fork.trigger" LD_PRELOAD="$HOOK" \
	"$TREE/usr/fileio/fileio_tgt" --block=4096 --threads=1 --multi_cmd=0 \
	"$NAME" "$RUN/disk.img" > fileio.log 2>&1 &
fileio_pid=$!
for _ in $(seq 1 100); do [[ -d /sys/kernel/scst_tgt/handlers/$NAME ]] && break; sleep 0.1; done
echo "add $NAME 0" > "$mgmt"
lun_added=1

sg=
for _ in $(seq 1 100); do
	for c in /sys/class/scsi_generic/sg*; do
		[[ -f $c/device/model ]] || continue
		read -r model < "$c/device/model"
		[[ $model == "$NAME" ]] && sg=/dev/${c##*/} && break
	done
	[[ -n $sg ]] && break
	sleep 0.1
done
[[ -n $sg ]] || { echo 'no scst_local sg device' >&2; exit 1; }
sleep 3 # let the initiator's own probe commands finish
# Consume the power-on/reset UNIT ATTENTION reported for a new LUN.
for _ in 1 2 3; do sg_turs "$sg" > /dev/null 2>&1 && break; done
marker="FORK_REPRO start $$ $(date +%s%N)"
echo "$marker" > /dev/kmsg

echo "== 1. READ CAPACITY before fork"
sg_readcap "$sg" | grep -E 'Last LBA|block length'

echo "== 2. READ block 0 (puts 'SCSTDATA...' into the cached buffer page)"
sg_dd if="$sg" of=/dev/null bs=4096 count=1 bpt=1 2>&1 | tail -1

if [[ ${NO_FORK:-0} == 1 ]]; then
	echo "== 3. (control) no fork"
	sg_turs "$sg"
else
	echo "== 3. fork() inside fileio_tgt (child only sleeps)"
	touch fork.trigger
	sg_turs "$sg"
	grep FORK_ONCE fileio.log
fi

echo "== 4. READ CAPACITY"
out=$(sg_readcap "$sg" 2>&1 || true)
echo "$out" | grep -E 'Last LBA|block length|failed|Sense'

echo "== kernel log"
dmesg | sed -n "/$marker/,\$p" | grep -E '1 << |block shift|sector size|PARSE failed|Invalid' || echo '(nothing)'

if echo "$out" | grep -q 'Logical block length=4096 bytes'; then
	echo "RESULT: OK (block length 4096)"
else
	echo "RESULT: BUG REPRODUCED (stale page returned after fork)"
fi
