# alfs

The append-log flash store: on serial NOR flash, an object area of named
objects, each written and read whole, and a log area where logs are
appended around a ring of sectors. Built for systems that run code from
the same chip and cannot afford to stall.

Priorities, in order: speed, then flash life, then safety. A corrupted data
block is less important than a broken exec loop. Nothing in the log area
erases unless the application asks: every log write programs flash erased
ahead of time.

**Status:** format version 4. Requirements, tests and code.
- **Stores:** 41 tests, run on 8 simulator seeds under the sanitizers
  (`make test`) and on 100 seeds (`make soak`). 6,538 bytes of code on a
  Cortex-M0+: the log area 3,173, the object area 2,669, what both share
  696.
- **Flash task:** 8 tests on the FreeRTOS POSIX port (`make test-rt`).
  1,725 bytes of code.
- **Erases:** on the W-LOG workload (22-byte rows at 50 a second, synced
  every second, for 10 minutes), 168 sector erases, all of them asked for.
- **Hardware:** the flash assumptions alfs rests on are measured on the
  chips of pyro's boards (`docs/FLASH_PROBE.md`). alfs itself runs in the
  simulator and on the POSIX port only; it is untested on hardware.

## How it works

The application gives alfs two areas of the chip, each configured on its
own.

**The object area** holds configuration objects and Lua programs, each
under a name of up to 31 bytes.
- **Two halves.** One is the store; the other, the spare, is where the
  next put or delete writes every object again.
- **A put** names the object and its size, takes its bytes in any number
  of writes, and commits. It writes the other objects into the spare
  half, then the new one, then a commit record that makes the spare the
  store. Until the commit, a get reads what was there before; after any
  power loss, each object reads whole.
- **A get** checks the object whole before it returns a byte, and reads it
  as it was when opened, even when a put commits meanwhile.
- **The spare is erased** by the put that needs it, or ahead of time when
  the application asks.

**The log area** is a ring of sectors, each describing itself.
- **A sector** starts with a stamp, written when it is erased, then a
  claim naming its log, then chunks that never cross a page.
- **A log** is one flight. It takes its first sector when it starts, and
  its number is that sector's sequence: numbers grow and are never used
  again. Sectors are taken forward around the ring.
- **Streams.** Each chunk carries a tag, so two writers, a flight log and
  a high-rate log, write two streams of one log.
- **Writes** fill a block of RAM the writer brings and, when it is full,
  program it in chunks that fill their pages. A sync programs the rest.
- **After a reset** the application starts a new log; the one cut short
  stays readable.
- **Erasing** a log erases its sectors, with any dirty ones. Erasing
  happens only when the application asks.

There is no directory and no index. A mount reads 30 bytes of each log
sector and 29 bytes of each half.

**The flash task** (`alfs_rtos.c`) serves both areas from one FreeRTOS
task. The application's tasks send requests and wait on them, so the
stores run each operation as a plain loop, with nothing else touching the
flash. A log write that fits in its block never leaves the caller's task.

## Files

| Path | What |
|---|---|
| `docs/REQUIREMENTS.md` | what alfs must do, each requirement testable |
| `docs/TEST_PLAN.md` | how each requirement is tested |
| `docs/SPEC.md` | every byte on the flash |
| `alfs.h` | what both stores share: return codes, the driver, an area |
| `alfs_obj.h`, `alfs_obj.c` | the object area |
| `alfs_log.h`, `alfs_log.c` | the log area |
| `alfs_flash.h`, `alfs_flash.c` | the checked flash operations both use |
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
| `main` | format version 3: named, versioned files, and the flash task |
| `experiment/v4-object-store` | format version 4: the object area and the log ring, and the flash task |
| `experiment/format-1` | format 1: streams found by an identity and a tag, one flash operation a call; 5.3 KB |
| `experiment/format-2` | format 2: streams with sector numbers, exact programs, and resuming a stream after a reset; 6.8 KB |
| `experiment/v2-safety-rule-cost` | the code each of format 2's safety rules takes, measured by building format 2 without it |
| `experiment/v3-requirement-cost` | the code each requirement takes in format 3, and which tests fail without it |

Each measurement branch holds its method and results in
`experiments/<name>/RESULTS.md`.

## Tests and releases

Every push runs every test (`.github/workflows/test.yml`):
- the stores and the flash task on 8 seeds under the sanitizers, built
  with GCC and with Clang;
- the stores on 100 seeds;
- the Cortex-M0+ size and stack budgets, built with Arm GNU Toolchain
  13.2.Rel1, the release the budgets are set for.

A release is made by pushing a tag `v*`. The release workflow runs all of
those tests on the tagged commit, and makes the release only if every one
passes.

## License

MIT (`LICENSE`). The datasheets in `docs/datasheets/` are their makers',
kept here as the sources the figures cite.
