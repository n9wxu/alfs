# alfs

The append-log flash store: a small filesystem of named, versioned files
on serial NOR flash, built for append-only logs and for systems that run
code from the same chip and cannot afford to stall.

Priorities, in order: speed, then flash life, then safety. A corrupted data
block is less important than a broken exec loop. Nothing erases unless the
application asks: every write programs flash erased ahead of time.

**Status:** format version 3. Requirements, tests and code.
- **Core:** 44 tests, run on 8 simulator seeds under the sanitizers
  (`make test`) and on 100 seeds (`make soak`). 7,101 bytes of code on a
  Cortex-M0+.
- **Flash task:** 7 tests on the FreeRTOS POSIX port (`make test-rt`).
  1,153 bytes of code.
- **Erases:** on the W-LOG workload (22-byte rows at 50 a second, synced
  every second, for 10 minutes), 168 sector erases, all of them asked for.
- **Hardware:** the flash assumptions alfs rests on are measured on the
  chips of pyro's boards (`docs/FLASH_PROBE.md`). alfs itself runs in the
  simulator and on the POSIX port only; it is untested on hardware.

## How it works

**The region is a ring of sectors, each describing itself.**
- Every sector starts with a stamp, written when it is erased: its format,
  generation, erase count and a sequence floor.
- A sector a file takes carries a claim: the file's identity, a sequence,
  the file's first sector, and the sector's number within the file.
- A file's first sector also holds its name and, once it is closed, a close
  record with its size. Its data starts on the next page.

There is no directory and no index. A mount reads every sector's header
once; a lookup by name scans the first sectors.

**Files** are created by name and written once, from their start, through a
block of RAM the caller supplies. A write fills the block and, when it is
full, programs it in chunks that fill their pages. A sync programs the
rest. A close records the size.

Each create gives a new version, alfs's identity for the file, which is
never used again. Version 0 opens the newest closed version of a name.
Deletes are by name and version. After a reset, the application starts a
new version; the one cut short stays readable.

**The flash task** (`alfs_rtos.c`) serves an instance from one FreeRTOS
task. The application's tasks send requests and wait on them, so the core
runs each operation as a plain loop, with nothing else touching the flash.
A write that fits in its block never leaves the caller's task.

## Files

| Path | What |
|---|---|
| `docs/REQUIREMENTS.md` | what alfs must do, each requirement testable |
| `docs/TEST_PLAN.md` | how each requirement is tested |
| `docs/SPEC.md` | every byte on the flash |
| `alfs.h`, `alfs.c` | the core: no RTOS, one caller at a time |
| `alfs_rtos.h`, `alfs_rtos.c` | the flash task, on FreeRTOS |
| `tests/` | the simulated flash, the harness and both test suites |
| `docs/datasheets/` | the flash parts the figures come from |
| `probe/`, `docs/FLASH_PROBE.md` | the flash probe, which measures the chips themselves, and its measurements |

The use cases come from the pyro flight computer
(github.com/n9wxu/pyro_fw), the flash fault model from the littlefs fork
(github.com/n9wxu/littlefs). alfs shares no code with either.

## Branches

| Branch | What it holds |
|---|---|
| `main` | format version 3 and the flash task |
| `experiment/format-1` | format 1: streams found by an identity and a tag, one flash operation a call; 5.3 KB |
| `experiment/format-2` | format 2: streams with sector numbers, exact programs, and resuming a stream after a reset; 6.8 KB |
| `experiment/v2-safety-rule-cost` | the code each of format 2's safety rules takes, measured by building format 2 without it |
| `experiment/v3-requirement-cost` | the code each requirement takes in format 3, and which tests fail without it |

Each experiment branch holds its method and results in
`experiments/<name>/RESULTS.md`.

## Tests and releases

Every push runs every test (`.github/workflows/test.yml`):
- the core and the flash task on 8 seeds under the sanitizers, built with
  GCC and with Clang;
- the core on 100 seeds;
- the Cortex-M0+ size and stack budgets, built with Arm GNU Toolchain
  13.2.Rel1, the release the budgets are set for.

A release is made by pushing a tag `v*`. The release workflow runs all of
those tests on the tagged commit, and makes the release only if every one
passes.

## License

MIT (`LICENSE`). The datasheets in `docs/datasheets/` are their makers',
kept here as the sources the figures cite.
