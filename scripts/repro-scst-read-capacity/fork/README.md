# scst_user: cached buffers are not pinned across fork()

## Problem

`dev_user_map_buf()` in `scst/src/dev_handlers/scst_user_main.c` takes the
handler's buffer pages with `get_user_pages(FOLL_WRITE)`. With
`SCST_USER_MEM_REUSE_*` the buffer stays cached in the SGV pool and is reused
by later commands, so SCST holds that page reference for a long time.

A page reference (FOLL_GET) does not keep the handler and SCST on the same page
across `fork()`. After the handler forks, its private pages are shared
copy-on-write; the handler's next write to a cached buffer moves the handler to
a new copy, while SCST keeps using the old page. SCST then sends the old
contents to the initiator, and for READ CAPACITY it also takes the old bytes as
the device's block length.

Any `fork()` in the handler triggers it: a crash reporter writing a minidump,
`system()`, `popen()`. We hit it in production when a user-space handler
aborted and its crash handler (breakpad) cloned a minidump writer without
`CLONE_VM`: SCST logged `1 << 29 != 809052976` and switched the device to
536870912-byte blocks; other occurrences reported 16 MiB, 64 MiB, 4 and 0.
The same window affects any command using a cached buffer: READ can return
stale data, and WRITE data can land in the page the handler no longer reads.

## Reproduction

Uses only upstream code: `usr/fileio/fileio_tgt` (defaults
`SCST_USER_MEM_REUSE_ALL`, `SCST_USER_ON_FREE_CMD_IGNORE`, page-aligned
buffers) behind `scst_local`. SCSI I/O is read-only. One handler thread and one
initiator issuing commands one at a time, so every command reuses the same
cached buffer.

- `fork_once.c`: `LD_PRELOAD` hook that makes `fileio_tgt` call `fork()` once
  when `$FORK_TRIGGER` exists. The child only sleeps and never touches the
  buffers.
- `run-minimal.sh`: the test.

Steps performed by `run-minimal.sh`:

1. Backing file block 0 = `SCSTDATA` repeated; 4096-byte blocks.
2. `sg_readcap` → 4096 (baseline).
3. `sg_dd` READ of block 0 → the cached buffer page now holds `SCSTDATA...`.
4. Trigger the fork, then `sg_turs` (the hook forks inside `fileio_tgt`).
5. `sg_readcap` again. `fileio_tgt` writes the correct reply into its new COW
   page; unpatched SCST reads the old page.

Build SCST (`scst`, `scst_local`, `usr/fileio`) for the running kernel, then as
root in a disposable VM:

```sh
cc -shared -fPIC -O2 -Wall -Wextra -o fork_once.so fork_once.c -ldl
SCST_TREE=/path/to/scst HOOK=$PWD/fork_once.so bash run-minimal.sh
SCST_TREE=/path/to/scst HOOK=$PWD/fork_once.so NO_FORK=1 bash run-minimal.sh   # control
```

Unload `scst_local`, `scst_user` and `scst` before switching between trees.

## Results

Ubuntu 24.04, arm64, kernel 6.8.0-142-generic. "master" is SCST master
21567d8c.

| Tree | Fork | Runs | Final READ CAPACITY |
| --- | --- | --- | --- |
| master | yes | 6 | last LBA `0x53435354` ("SCST"), block length `1145132097` = `0x44415441` ("DATA") every run |
| master | no (`NO_FORK=1`) | 3 | 4096, correct |
| master + patch | yes | 6 | 4096, correct |

Full output of one run per configuration is in `results/`
(`master-fork.txt`, `master-nofork.txt`, `patched-fork.txt`; the
`*-srcversion.txt` files identify the loaded `scst_user` module).

Unpatched master, final READ CAPACITY (`results/master-fork.txt`):

```text
== 4. READ CAPACITY
   Last LBA=1396921172 (0x53435354), Number of logical blocks=1396921173
   Logical block length=1145132097 bytes
== kernel log
[60778.106665] 1 << 30 != 1145132097
[60778.107217] [31719]: scst_user: forkdisk: Changed block shift from 12 into 30 / 30
RESULT: BUG REPRODUCED (stale page returned after fork)
```

`1 << 30 != ...` is a `WARN_ONCE`, so it is logged only for the first
occurrence after `scst` is loaded; the block shift change is logged every time.

Master + patch, final READ CAPACITY (`results/patched-fork.txt`):

```text
== 4. READ CAPACITY
   Last LBA=8191 (0x1fff), Number of logical blocks=8192
   Logical block length=4096 bytes
== kernel log
(nothing)
RESULT: OK (block length 4096)
```

## Fix

Pin the pages instead of only referencing them (kernels >= 5.8):

- `dev_user_map_buf()`: `pin_user_pages(..., FOLL_WRITE | FOLL_LONGTERM, ...)`
  instead of `get_user_pages(..., FOLL_WRITE, ...)`.
- `dev_user_unmap_buf()`: `unpin_user_pages_dirty_lock(pages, n, buf_dirty)`
  instead of `SetPageDirty()` + `put_page()`.
- The map error path: `unpin_user_pages()` instead of `put_page()`.

For pinned anonymous pages, fork() copies the page for the child, so the
handler and SCST keep sharing the same page. Kernels older than 5.8 keep the
existing code. The patch is the commit
`scst_user: Pin cached user buffers so they survive fork()` in
[SCST-project/scst#405](https://github.com/SCST-project/scst/pull/405).
