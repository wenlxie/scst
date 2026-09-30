# Issue #279: upstream reproduction and fix validation

## Scope and provenance

- Upstream source: `SCST-project/scst` commit
  `5908a8da46c3f98dc1524d31d761bd4dd642b151` (3.11.0-pre). The tracked
  `scst/src/dev_handlers/scst_user.c` had no local edits for the failing runs.
  Its SHA-256 was
  `1a44edae0aa6ee2c7cca6ccdce756695d1342bd6ba3753ec5ff26cee8de22fc9`.
- Upstream includes PR #378 as commit `83745c0a2dbbdb0a5674fc1bb4e4d922bc38904b`.
  Its cleanup code has no added panic counter or forced reboot.
- Guest: Ubuntu 24.04, AArch64 Lima VM. The controlled before/after comparison
  used **the same `6.8.0-142-generic` kernel**, matching headers, and the same
  userspace workload. Both module sets reported that kernel in `modinfo -F
  vermagic`. The unfixed `scst_user.ko` srcversion was
  `16F5E208ACCC17B639E5C7B`; the fixed one was
  `68286AB09CBF1949541C3A8`.
- A preliminary unmodified upstream run on `6.8.0-134-generic` also produced
  the soft lockup. The VM booted a newly installed `-142` kernel after the
  recovery reboot, so the controlled runs below were repeated on `-142`.
- The two run directories were `/var/tmp/scst-user-upstream-279-142` and
  `/var/tmp/scst-user-fixed-279-142`. The exact commands to rebuild and run
  either case are in [README.md](README.md).

This reproduces the **failure pattern and storage scenario** in
[issue #279](https://github.com/SCST-project/scst/issues/279): a spinning
`scst_usr_cleanup` worker, release threads in D state, and a soft lockup
following concurrent device handle releases. The issue reporter used SCST
3.6/3.8 on Ubuntu 22.04 with Linux 5.15; this validation uses current upstream
SCST on Ubuntu 24.04 with Linux 6.8. It does not claim an identical environment.

## Exact trigger sequence

1. `fileio_tgt` registers A and B with `--sgv_shared`, full memory reuse,
   `--sgv_disable_clustered_pool`, `--non_blocking`, one backend thread, and
   the default `SCST_USER_ON_FREE_CMD_IGNORE`. Its option log is saved in
   [`backend.log`](evidence/upstream-6.8.0-142/backend.log).
2. `scst_local` exposes A as `/dev/sg0` (LUN 0) and B as `/dev/sg1` (LUN 1).
   Two 4 KiB READ(10) operations at A's LBA 123 establish a reusable buffer.
3. A 4 KiB READ(10) at B's LBA 124 receives that same userspace buffer address.
   The ioctl interposer records the match, closes A's handle, then sends
   `SIGSTOP` to the backend while B's READ is outstanding. In the controlled
   upstream run, all three markers used `0xea5674001000`:

   ```text
   SCST_REPRO_A_BUFFER buffer=0xea5674001000
   SCST_REPRO_B_HOLDS_A_BUFFER buffer=0xea5674001000
   SCST_REPRO_A_HANDLE_CLOSED buffer=0xea5674001000
   ```

4. Before B's handle closes, SCST's `commands` sysfs file shows A's retained
   user command in `UCMD_STATE_ON_FREE_SKIPPED` (state 7), `ref=1`,
   `sent_to_user=0`, and `scst_cmd=NULL`. B has an `UCMD_STATE_EXECING`
   (state 3) command with `sent_to_user=1`. See the
   [upstream state snapshots](evidence/upstream-6.8.0-142/state.log).
5. A's release thread remains blocked. The cleanup worker becomes runnable
   continuously and the kernel reports a soft lockup after 26 seconds. The
   watchdog stack includes `sgv_pool_flush -> dev_user_cleanup_thread`.
6. The runner kills the stopped backend, closing B's handle. Cleanup still
   times out ten seconds later: the cleanup worker is runnable, and both
   release threads remain in D state. See the [run output](evidence/upstream-6.8.0-142/run.log)
   and [kernel excerpt](evidence/upstream-6.8.0-142/kernel-excerpt.log).

The first snapshot also contained a state `0x21` detach-session user command
for A. The state 7/ref 1 command is the one relevant to the retained buffer.
Kernel pointers printed by sysfs on this VM were obscured, so matching
userspace buffer addresses plus the source's SGV ownership flow support the
cross-device reuse mechanism; they do not directly prove kernel SGV object
identity in this run.

### Corroborating pointer chain from a separate dump

A separate x86_64 dump from the earlier investigation was taken with a local
panic counter in the cleanup loop. **That instrumented build was not used for
the upstream reproductions above.** With `crash 9.0.3`, raw memory reads and
the matching `scst_user` source layout showed:

| Object | Observed fields |
| --- | --- |
| A command `ffff8d6c8f33dd80` | `cmd=NULL`, `ucmd_ref=1`, `sgv=NULL`, `sent_to_user=0`, `state=7` |
| SGV object `ffff8d6d8107f980` | `allocator_priv=ffff8d6c8f33dd80` (A command) |
| B command `ffff8d6d90b3bc80` | `buf_ucmd=ffff8d6c8f33dd80`, `sgv=ffff8d6d8107f980`, `state=3` |

`kmem` reported all three objects as allocated. The dump's cleanup worker was
processing A while B's cleanup entry was queued. This directly establishes
the cross-device kernel pointer relationship; the pristine upstream VM run
above establishes that the soft lockup occurs without the panic counter.

## Why PR #378 does not resolve this case

[PR #378](https://github.com/SCST-project/scst/pull/378) added a second
`sgv_pool_flush()` after unjamming commands. That releases SGV objects which
**have already been returned to the pool cache** during unjamming. This is a
valid fix for that cache-resident lifetime path.

In this run, B's command is still executing while it holds the buffer reused
from A. An SGV object checked out to B is not on the pool's recycling list, so
neither the pre-unjam nor post-unjam flush can evict it. A's allocation command
therefore keeps its final reference and remains in A's hash. In the original
`dev_user_process_cleanup(A)`, `dev_user_unjam_dev()` counts that entry,
`dev_user_get_next_cmd()` finds no ready command, and the inner loop retries A
without returning to the single outer cleanup worker. B's cleanup cannot run
even after B's userspace handle is closed. The observed worker state, release
threads, and watchdog stack match this source path.

## Fix and same-kernel result

When `dev_user_get_next_cmd()` returns `-EAGAIN` and the hash still has pending
entries, `dev_user_process_cleanup()` now returns to the outer worker with a
retry result. The worker can process B and later retry A. The existing outer
worker sleeps 100 ms before retrying deferred devices, so it does not spin.
When the hash becomes empty and cleanup is marked done, A's completion is
signalled.

The fixed run used the same 30-second hold. Its backend log again confirmed
that B held A's buffer, at `0xf32268003000`. During the hold, B was still
executing and `scst_usr_cleanu` was sleeping in `msleep`, as shown in the
[fixed state snapshots](evidence/fixed-6.8.0-142/state.log). After B closed,
the run printed `SCST_REPRO_CLEANUP_COMPLETE`; both release threads and both
devices disappeared. The [fixed run output](evidence/fixed-6.8.0-142/run.log)
and [kernel excerpt](evidence/fixed-6.8.0-142/kernel-excerpt.log) record the
completion. No soft-lockup warning appeared in the fixed run's captured kernel
log.

The runner now reports handler disappearance as a handler observation only.
An earlier helper revision printed “A release completed” when the handler
directory vanished, although A's release thread was still blocked; that
wording was incorrect. `state.log` records the release threads, and the final
success condition checks that both handlers **and** release threads are gone.
