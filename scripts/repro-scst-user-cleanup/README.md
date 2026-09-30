# Reproduce SCST issue #279 with upstream modules

This is a local SCSI storage reproducer for the `scst_user` cleanup soft
lockup in [issue #279](https://github.com/SCST-project/scst/issues/279). It
uses `scst_local` and `/dev/sg*`; it does not create a network target. Run the
unfixed case in a disposable VM: the cleanup worker can remain stuck until the
guest is restarted.

The userspace helper works with unmodified upstream SCST. No kernel panic,
reboot, logging, or timing patch is needed. The recorded upstream run used
commit `5908a8da46c3f98dc1524d31d761bd4dd642b151`, which already includes
[PR #378](https://github.com/SCST-project/scst/pull/378).

## Prepare two source trees

Keep an upstream checkout for the failing run and this review branch for the
fixed run. Copy only the userspace reproducer into the upstream checkout:

```sh
git clone https://github.com/SCST-project/scst.git scst-upstream
git -C scst-upstream checkout 5908a8da46c3f98dc1524d31d761bd4dd642b151
git clone --branch fix-issue-279-shared-sgv-cleanup \
  https://github.com/wenlxie/scst.git scst-fixed
cp -a scst-fixed/scripts/repro-scst-user-cleanup \
  scst-upstream/scripts/
git -C scst-upstream diff --exit-code -- scst/src/dev_handlers/scst_user.c
```

Perform the following steps inside the VM, after moving both trees there.
Use matching headers for the running guest kernel and note `uname -r` before
each run. If the guest changes kernels after a reboot, clean and rebuild both
trees for the new kernel before comparing results.

## Build and load upstream

From the `scst-upstream` root:

```sh
uname -r
make -C scst 2release
make -C scst -j"$(nproc)" all
make -C scst_local -j"$(nproc)" all
modinfo -F vermagic scst/src/dev_handlers/scst_user.ko
sudo modprobe dlm sg
sudo insmod scst/src/scst.ko
sudo insmod scst/src/dev_handlers/scst_user.ko
sudo insmod scst_local/scst_local.ko
sudo env SCST_REPRO_DISPOSABLE=1 SCST_REPRO_HOLD_SECONDS=30 \
  SCST_REPRO_DIR=/var/tmp/scst-user-upstream-279 \
  bash scripts/repro-scst-user-cleanup/run.sh
```

`vermagic` must name the running kernel. If the tree has objects built for a
different kernel, run `make -C scst clean` and `make -C scst_local clean`
before building. Keep the VM console or `dmesg` available while the unfixed
case runs.

The runner produces `backend.log`, `state.log`, `read-a.log`, `read-b.log`,
and `dmesg.log` in `SCST_REPRO_DIR`. Preserve them before restarting the VM.
An unfixed run should report `SCST_REPRO_CLEANUP_TIMEOUT`; check the process
snapshot and kernel log for `scst_usr_cleanu` running, release threads in D
state, and a watchdog soft lockup. A handler directory disappearing alone does
not prove its release thread finished.

After collecting the logs, restart the disposable guest to clear the stuck
upstream modules. Confirm the kernel release again before the fixed run.

## Build and load the fix

From the `scst-fixed` root, on the same guest kernel:

```sh
uname -r
make -C scst 2release
make -C scst -j"$(nproc)" all
make -C scst_local -j"$(nproc)" all
modinfo -F vermagic scst/src/dev_handlers/scst_user.ko
sudo modprobe dlm sg
sudo insmod scst/src/scst.ko
sudo insmod scst/src/dev_handlers/scst_user.ko
sudo insmod scst_local/scst_local.ko
sudo env SCST_REPRO_DISPOSABLE=1 SCST_REPRO_HOLD_SECONDS=30 \
  SCST_REPRO_DIR=/var/tmp/scst-user-fixed-279 \
  bash scripts/repro-scst-user-cleanup/run.sh
```

The fixed run should print `SCST_REPRO_CLEANUP_COMPLETE` after B's handle is
closed. Check `state.log`: while B holds the buffer, the cleanup worker should
sleep instead of consuming a CPU; after B closes, no `scst_usr_released`
thread or `repro_a`/`repro_b` device should remain.

## What the runner does

1. Build a file-backed `scst_user` backend, an ioctl interposer, and an SG
   READ(10) helper from the selected source tree.
2. Register `repro_a` and `repro_b` as 2 MiB devices with a shared SGV pool,
   full memory reuse, nonblocking commands, and `ON_FREE_CMD_IGNORE`.
3. Export the devices through `scst_local` as LUN 0 and LUN 1.
4. Read 4 KiB from A twice at LBA 123, then start a 4 KiB read from B at
   LBA 124. The interposer requires B's buffer address to equal A's.
5. Close A's `/dev/scst_user` handle while B's read is in flight, then stop
   the backend process so B continues holding the shared buffer.
6. Snapshot SCST commands and release-thread states, hold for 30 seconds,
   terminate the backend to close B, and observe whether cleanup completes.

The same address in `SCST_REPRO_A_BUFFER`,
`SCST_REPRO_B_HOLDS_A_BUFFER`, and `SCST_REPRO_A_HANDLE_CLOSED` is the
workload precondition. The failure or success result is determined from the
kernel log, SCST command state, and release threads, not that address alone.
See [validation.md](validation.md) and its [captured evidence](evidence/).
