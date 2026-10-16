# alfs

The append-log flash store: a small filesystem of named, versioned files
on serial NOR flash, built for append-only logs and for systems that run
code from the same chip and cannot afford to stall.

Priorities, in order: speed, then flash life, then safety. A corrupted data
block is less important than a broken exec loop. Nothing erases unless the
application asks: every write programs flash that was erased ahead of
time.

**Status:** format version 3, 2026-10-15. Requirements, tests and code.
- **Core:** 44 tests pass on 8 simulator seeds under the sanitizers
  (`make test`) and on 100 seeds (`make soak`). It is 7.1 KB of code on a
  Cortex-M0+, against littlefs's 15.5 KB on the same build.
- **Flash task:** 7 tests pass on the FreeRTOS POSIX port
  (`make test-rt`). It is 1.2 KB of code.
- **Erases:** on littlefs's 10-minute logging workload at 50 rows a second,
  alfs erases 168 sectors, all when asked, where littlefs 2.11.3 erases
  807 in its write path.
- **Hardware:** the flash assumptions alfs rests on were measured on the
  chips of pyro's boards (`docs/FLASH_PROBE.md`). alfs itself has not yet
  run on hardware.

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
| `probe/`, `docs/FLASH_PROBE.md` | the flash probe, which measures the chips themselves, and what it found |
| `docs/CATALOG_EVALUATION.md`, `docs/V2_STRATEGY.md` | earlier design decisions, kept for their reasoning |

The use cases come from the pyro flight computer
(github.com/n9wxu/pyro_fw); the flash fault model from the littlefs v3 work
(github.com/n9wxu/littlefs). alfs shares no code with either.

## Branches

| Branch | What |
|---|---|
| `main` | format version 3 and the flash task |
| `experiment/format-1` | the first build: streams found by an identity and a tag, one flash operation a call, 5.3 KB |
| `experiment/format-2` | exact programs, sector numbers and resuming a file after a reset, 6.8 KB; replaced by format 3 |
| `experiment/v2-safety-rule-cost` | what each of format 2's safety rules costs in code, by building alfs without it |
| `experiment/v3-requirement-cost` | what each requirement costs in format 3's code, and the simplest alfs that still meets them |

An experiment's branch holds its own `experiments/<name>/RESULTS.md`.

## Tests and releases

Every push runs every test (`.github/workflows/test.yml`):
- the core and the flash task on 8 seeds under the sanitizers, built with
  GCC and with Clang;
- the core on 100 seeds;
- the Cortex-M0+ size and stack budgets, with the Arm GNU Toolchain
  release they were measured with (13.2.Rel1).

A release is made by pushing a tag `v*`. The release workflow runs all of
those tests on the tagged commit first, and makes the release only if every
one passes.

## License

MIT (`LICENSE`). The datasheets in `docs/datasheets/` are their makers',
kept here as the sources the figures cite.
