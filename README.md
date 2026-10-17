# alfs

The append-log flash store: on serial NOR flash, log areas where rows are
appended around a ring of sectors, and an object area of named objects
written and read whole. Built for systems that run code from the same chip
and cannot afford to stall.

Priorities, in order: speed, then flash life, then safety. A corrupted data
block is less important than a broken exec loop. Nothing in a log area
erases unless the application asks: every log write programs flash erased
ahead of time.

**Status:** format version 5. Requirements, tests and code.
- **Stores:** 46 tests, run on 8 simulator seeds under the sanitizers
  (`make test`) and on 100 seeds (`make soak`). 6,642 bytes of code on a
  Cortex-M0+: a log area 3,277, the object area 2,669, what they share 696.
- **Flash task:** 8 tests on the FreeRTOS POSIX port (`make test-rt`).
  1,833 bytes of code.
- **The configuration application** (`examples/kv.c`), which keeps settings
  as rows of a log area: 1,365 bytes. The store tests run it; it is not
  part of alfs.
- **Erases:** on the W-LOG workload (22-byte rows at 50 a second, synced
  every second, for 10 minutes), 189 sector erases, all of them asked for.
- **Hardware:** the flash assumptions alfs rests on are measured on the
  chips of pyro's boards (`docs/FLASH_PROBE.md`). alfs itself runs in the
  simulator and on the POSIX port only; it is untested on hardware.

## How it works

The application gives alfs areas of the chip, each configured on its own:
as many log areas as it needs, and an object area.

**A log area** is a ring of sectors, each describing itself.
- **A row** is what a write takes and a read returns, whole: 1 to 218 bytes
  on 256-byte pages. alfs gives a row no meaning; the application does.
- **A sector** starts with a stamp, written when it is erased, then a
  claim naming its log, then chunks that never cross a page. A chunk holds
  whole rows, each after its length, so a chunk lost to a fault loses
  whole rows, and the rows after it read in step.
- **A log** is the rows written from one start: in the flight log area,
  one flight. It takes its first sector when it starts, and its number is
  that sector's sequence: numbers grow and are never used again. Sectors
  are taken forward around the ring.
- **Streams.** Each chunk carries a tag, so two writers, a flight log and
  a high-rate log, write two streams of one log.
- **Writes** gather rows in a block of RAM the writer brings. When a row
  does not fit, the block's rows are programmed in chunks, each filling
  its page as far as whole rows do. A sync programs the rest.
- **After a reset** the application starts a new log; the one cut short
  stays readable.
- **Erasing** a log erases its sectors, with any dirty ones. Erasing
  happens only when the application asks.

**The configuration area** is a log area like any other, with the same
code and the same format; only its application differs. `examples/kv.c`
keeps settings as rows.
- **A row** is a key and its value; an empty value deletes the key.
- **At boot** it reads every row into RAM, where the web interface reads
  the settings; **a set** appends a row and syncs it.
- **A new log** starts with a row for every setting, then a mark. The
  newest log with a mark holds every setting, so the application erases
  the others: those before the new mark is synced, that one after. After
  any power loss each setting reads its old value or its new one.

**The object area** holds Lua programs, each under a name of up to 31
bytes, written as whole files.
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

There is no directory and no index. A mount reads 30 bytes of each log
sector and 29 bytes of each half.

**The flash task** (`alfs_rtos.c`) serves every area from one FreeRTOS
task; each call names its area's instance. The application's tasks send
requests and wait on them, so the stores run each operation as a plain
loop, with nothing else touching the flash. A log write whose row fits in
its block never leaves the caller's task.

## Files

| Path | What |
|---|---|
| `docs/REQUIREMENTS.md` | what alfs must do, each requirement testable |
| `docs/TEST_PLAN.md` | how each requirement is tested |
| `docs/SPEC.md` | every byte on the flash |
| `alfs.h` | what the stores share: return codes, the driver, an area |
| `alfs_log.h`, `alfs_log.c` | a log area |
| `alfs_obj.h`, `alfs_obj.c` | the object area |
| `alfs_flash.h`, `alfs_flash.c` | the checked flash operations the stores use |
| `alfs_rtos.h`, `alfs_rtos.c` | the flash task, on FreeRTOS |
| `examples/kv.h`, `examples/kv.c` | the configuration application: settings as rows of a log area |
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
| `experiment/v5-rows` | format version 5: log areas of rows, the configuration area one of them, the object area, and the flash task |
| `experiment/v4-object-store` | format version 4: a key-value configuration store, the object area, the log ring, and the flash task |
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
