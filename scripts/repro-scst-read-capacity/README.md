# READ CAPACITY block-length tests

Tests and results for
[SCST-project/scst#405](https://github.com/SCST-project/scst/pull/405):

1. `scst_tape: Size fixed-block transfers by the actual block size`
2. `scst_lib: Read split READ CAPACITY responses before parsing them`
3. `scst_lib: Reject invalid block lengths in READ CAPACITY responses`

The original failure: a user-space backend returned a READ CAPACITY response
whose block length field held 809052976 (`0x30392b30`). SCST logged
`1 << 29 != 809052976` from `scst_calc_block_shift()`, switched the device to
536870912-byte blocks, and failed the following READ and WRITE commands in
PARSE until another READ CAPACITY restored the block size.

Run everything in a disposable VM: the tests load and unload SCST modules.

## Test 1: `capacity_probe` kernel module

`capacity_probe.c` calls the exported `scst_calc_block_shift()`,
`scst_block_generic_dev_done()` and `scst_tape_generic_parse()` on synthetic
devices and commands. No target, initiator or backend is needed. It checks:

- `scst_calc_block_shift()` for 0, 512, 4096, 65536, 4, 768 and 809052976;
- READ CAPACITY(10) and (16) responses in one and in two scatterlist entries,
  with block lengths 4096, 809052976 and 0, plus a valid change from 4096 to
  512 bytes. For each case: the block length read, the device geometry,
  whether the response data is intact, the command status and the response
  length;
- tape fixed-block parsing with 512-, 1536- and 80-byte blocks, variable-block
  mode, a zero fixed block size, and overflow limits for `bufflen`,
  `out_bufflen` and `data_len`.

Build SCST, then the probe against the same tree:

```sh
make -C scst 2release
make -C scst -j"$(nproc)"
make -C scripts/repro-scst-read-capacity SCST_TREE="$PWD"
```

Run it on the patched tree:

```sh
sudo insmod scst/src/scst.ko
sudo insmod scst/src/dev_handlers/scst_tape.ko
sudo insmod scripts/repro-scst-read-capacity/capacity_probe.ko
sudo dmesg | grep CAPACITY_PROBE
sudo rmmod capacity_probe scst_tape scst
```

On an unpatched tree, load only `scst.ko` and pass `test_tape=0` to the
probe. Unpatched tape parsing shifts by a block shift that is invalid for
non-power-of-two tape sizes, so those cases are skipped.

## Test 2: real SCSI I/O through `scst_local` and `scst_user`

`run-real-io.sh` exports a file-backed `scst_user` disk (`fileio_tgt`, 4096-byte
blocks, 32 MiB disposable image) through `scst_local`. It writes and reads back
32 blocks with `sg_dd` WRITE(10)/READ(10) and compares the data byte for byte.

`real_io_fault.c` is an `LD_PRELOAD` interposer for the `fileio_tgt` reply
ioctl. While `/var/tmp/scst-read-capacity-real-io/inject_bad_capacity`
exists, it replaces the block length in the next READ CAPACITY(10)/(16) reply
with 809052976. The backend's real block size stays 4096 bytes.

The script runs: I/O, then a READ CAPACITY with the injected value, then I/O,
then a normal READ CAPACITY, then I/O. It exits non-zero if a step does not
match the expected result for `base` (unpatched) or `fixed` (patched).

Requires `sg3-utils`. Build `scst_local` and `usr/fileio` in the tree under
test, and build the interposer:

```sh
make -C scst_local -j"$(nproc)"
make -C usr/fileio -j"$(nproc)"
mkdir -p /var/tmp/scst-read-capacity-real-io
cc -shared -fPIC -O2 -Wall -Wextra -I"$PWD/scst/include" \
  -o /var/tmp/scst-read-capacity-real-io/real_io_fault.so \
  scripts/repro-scst-read-capacity/real_io_fault.c -ldl
sudo env SCST_REAL_IO_VM=1 SCST_TREE=/path/to/unpatched \
  bash scripts/repro-scst-read-capacity/run-real-io.sh base
sudo env SCST_REAL_IO_VM=1 SCST_TREE=/path/to/patched \
  bash scripts/repro-scst-read-capacity/run-real-io.sh fixed
```

## Results

Environment (`results/environment.txt`): Ubuntu 24.04.4 LTS, aarch64, kernel
6.8.0-142-generic, gcc 13.3.0, `2release` builds. "Unpatched" is upstream
`master` at 21567d8c; "patched" is that commit plus the three commits above.

### Test 1

| Tree | Summary | Log |
| --- | --- | --- |
| Unpatched (`test_tape=0`) | `tests=21 failed=12` | [`results/probe-base.txt`](results/probe-base.txt) |
| Patched | `tests=31 failed=0` | [`results/probe-fixed.txt`](results/probe-fixed.txt) |

Failures on the unpatched tree:

- 809052976 gives shift 29 with the `1 << 29 != 809052976` warning, and 768
  gives shift 9.
- A one-entry response with 809052976 changes the device geometry to
  536870912 bytes; zero and invalid lengths are returned with GOOD status.
- Two-entry responses: the block length is read as 0 from the uncopied
  temporary buffer and the response is overwritten (`unchanged=0`), for every
  block length including a valid 4096.

On the patched tree, invalid and zero lengths complete with CHECK CONDITION
(`status=0x2`) and no data (`resp_len=0`), the geometry stays 4096, two-entry
responses keep their data, valid changes are still applied, and tape transfer
sizes are block size × count with overflows rejected.

### Test 2

| Step | Unpatched | Patched |
| --- | --- | --- |
| I/O before | write, read, compare OK | write, read, compare OK |
| READ CAPACITY with injected 809052976 | GOOD (`rc=0`) | `Aborted command` (`rc=11`) |
| I/O after the bad reply | write `rc=22`, read `rc=22`, compare fails | write, read, compare OK |
| I/O after a normal READ CAPACITY | write, read, compare OK | write, read, compare OK |

Kernel log, unpatched ([`results/real-io-base-dmesg.txt`](results/real-io-base-dmesg.txt)):

```text
1 << 29 != 809052976
scst_user: real_io_disk: Changed block shift from 12 into 29 / 29
scst: ***WARNING***: bufflen 32, data_len 32 or out_bufflen 0 too large for device real_io_disk (block size 536870912)
scst_user: ***ERROR***: PARSE failed (ucmd ..., rc -22)
scst_user: real_io_disk: Changed block shift from 29 into 12 / 12
```

Kernel log, patched ([`results/real-io-fixed-dmesg.txt`](results/real-io-fixed-dmesg.txt)):

```text
scst: ***ERROR***: Invalid sector size 809052976 in READ CAPACITY response
```

Script output: [`results/real-io-base.txt`](results/real-io-base.txt),
[`results/real-io-fixed.txt`](results/real-io-fixed.txt).
