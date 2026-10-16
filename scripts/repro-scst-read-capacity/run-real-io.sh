#!/usr/bin/env bash
# Run only in the dedicated disposable Ubuntu VM. The image is test data.
set -euo pipefail

if [[ ${SCST_REAL_IO_VM:-} != 1 || $(id -u) != 0 ]]; then
	echo 'Run as root in a disposable VM with SCST_REAL_IO_VM=1.' >&2
	exit 2
fi

mode=${1:-}
case $mode in
	base|fixed) ;;
	*) echo 'Usage: SCST_TREE=<built tree> run-real-io.sh base|fixed' >&2; exit 2 ;;
esac

# SCST_TREE: tree whose scst, scst_user and scst_local modules are loaded.
# FILEIO_TGT: fileio_tgt binary; defaults to the one built in SCST_TREE.
tree=${SCST_TREE:?set SCST_TREE to a built SCST tree}
fileio_tgt=${FILEIO_TGT:-$tree/usr/fileio/fileio_tgt}
# real_io_fault.c checks for inject_bad_capacity in this directory.
run_dir=/var/tmp/scst-read-capacity-real-io
backend_pid=
lun_added=0
lun_mgmt=/sys/kernel/scst_tgt/targets/scst_local/scst_local_tgt/luns/mgmt
mkdir -p "$run_dir" /var/lib/scst/pr
cd "$run_dir"

cleanup() {
	if [[ $lun_added == 1 && -e $lun_mgmt ]]; then
		echo 'del 0' > "$lun_mgmt" || true
	fi
	if [[ -n $backend_pid ]]; then
		kill "$backend_pid" 2>/dev/null || true
		wait "$backend_pid" 2>/dev/null || true
	fi
	for _ in {1..50}; do
		rmmod scst_local 2>/dev/null || true
		rmmod scst_user 2>/dev/null || true
		rmmod scst 2>/dev/null || true
		[[ ! -d /sys/module/scst ]] && break
		sleep 0.1
	done
	if [[ -d /sys/module/scst ]]; then
		echo 'Warning: SCST modules are still releasing; check lsmod.' >&2
	fi
}
trap cleanup EXIT

rmmod capacity_probe 2>/dev/null || true
rmmod scst_tape 2>/dev/null || true
rmmod scst_local 2>/dev/null || true
rmmod scst_user 2>/dev/null || true
rmmod scst 2>/dev/null || true
modprobe sg
insmod "$tree/scst/src/scst.ko"
insmod "$tree/scst/src/dev_handlers/scst_user.ko"
insmod "$tree/scst_local/scst_local.ko"

truncate -s 32M backing.img
dd if=/dev/urandom of=pattern.bin bs=4096 count=32 status=none
rm -f inject_bad_capacity
LD_PRELOAD="$run_dir/real_io_fault.so" \
	"$fileio_tgt" \
	--block=4096 --threads=1 --multi_cmd=0 --on_free=call --mem_reuse=none \
	real_io_disk "$run_dir/backing.img" >"backend-$mode.log" 2>&1 &
backend_pid=$!

for _ in {1..100}; do
	[[ -d /sys/kernel/scst_tgt/handlers/real_io_disk ]] && break
	if ! kill -0 "$backend_pid" 2>/dev/null; then
		cat "backend-$mode.log" >&2
		exit 1
	fi
	sleep 0.1
done
[[ -d /sys/kernel/scst_tgt/handlers/real_io_disk ]]
echo 'add real_io_disk 0' > "$lun_mgmt"
lun_added=1

sg=
for _ in {1..100}; do
	for candidate in /sys/class/scsi_generic/sg*; do
		[[ -f $candidate/device/model ]] || continue
		read -r model < "$candidate/device/model"
		if [[ $model == real_io_disk ]]; then
			sg=/dev/${candidate##*/}
			break
		fi
	done
	[[ -n $sg ]] && break
	sleep 0.1
done
[[ -n $sg ]] || { echo 'No scst_local sg device appeared' >&2; exit 1; }
echo "SCSI device: $sg"

run_io() {
	local label=$1 write_rc=0 read_rc=0 cmp_rc=0
	timeout 20s sg_dd if=pattern.bin of="$sg" bs=4096 count=32 bpt=32 cdbsz=10 seek=10 \
		>"$mode-$label-write.log" 2>&1 || write_rc=$?
	timeout 20s sg_dd if="$sg" of="$mode-$label-read.bin" bs=4096 count=32 bpt=32 cdbsz=10 skip=10 \
		>"$mode-$label-read.log" 2>&1 || read_rc=$?
	cmp pattern.bin "$mode-$label-read.bin" >"$mode-$label-cmp.log" 2>&1 || cmp_rc=$?
	io_write_rc=$write_rc
	io_read_rc=$read_rc
	io_cmp_rc=$cmp_rc
	echo "REAL_IO $mode $label write_rc=$write_rc read_rc=$read_rc cmp_rc=$cmp_rc"
}

sg_readcap "$sg" >"normal-capacity-$mode.log" 2>&1
run_io before
[[ $io_write_rc == 0 && $io_read_rc == 0 && $io_cmp_rc == 0 ]]
touch inject_bad_capacity
bad_rc=0
timeout 20s sg_readcap "$sg" >"bad-capacity-$mode.log" 2>&1 || bad_rc=$?
echo "REAL_IO $mode bad_capacity_rc=$bad_rc"
run_io after_bad_capacity
if [[ $mode == base ]]; then
	[[ $bad_rc == 0 && $io_write_rc != 0 && $io_read_rc != 0 && $io_cmp_rc != 0 ]]
else
	[[ $bad_rc != 0 && $io_write_rc == 0 && $io_read_rc == 0 && $io_cmp_rc == 0 ]]
fi
rm -f inject_bad_capacity
sg_readcap "$sg" >"recovered-capacity-$mode.log" 2>&1
run_io after_recovery
[[ $io_write_rc == 0 && $io_read_rc == 0 && $io_cmp_rc == 0 ]]
injector_events=$(grep -c CAPACITY_REAL_IO_INJECT "backend-$mode.log" || true)
[[ $injector_events -ge 1 ]]
echo "REAL_IO $mode injector_events=$injector_events PASS"
echo "REAL_IO $mode logs=$run_dir"
