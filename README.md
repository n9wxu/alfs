# alfs

The append-log flash store: files on serial NOR flash, each a frame of
pages linked in a list. A file's first page holds its filename; a mount
builds a cache of the files from the pages themselves. A file's pages can
be reserved ahead of time, so that writing it in flight programs only flash
already erased. Built for systems that run code from the same chip and
cannot afford to stall.

Priorities, in order: speed, then flash life, then safety. A corrupted data
block is less important than a broken exec loop.

**Status:** format version 6. Requirements, tests and code.
- **Store:** 47 tests, run on 8 simulator seeds under the sanitizers
  (`make test`) and on 100 seeds (`make soak`). 6,664 bytes of code on a
  Cortex-M0+ (`alfs_fs.c` 5,968, `alfs_flash.c` 696); 308 bytes of stack at
  most.
- **Erases:** a 10-minute flight log (W-LOG: 22-byte rows at 50 a second,
  synced every second) costs 175 sector erases over its life, all of them
  the cleanup's.
- **Hardware:** the flash assumptions alfs rests on are measured on the
  chips of pyro's boards (`docs/FLASH_PROBE.md`). alfs itself runs in the
  simulator only so far.
- **Evaluation:** `experiments/v6-frames/EVALUATION.md`.

## How it works

The application gives alfs one area of the chip. Page 0 of the area holds
no file; every other page can.

**A page** starts with a header: the frame ID of its file, its position in
the file, a check, and the next page's address (erased: no next page yet;
zero: the file's last page). Its byte count, data and data check follow,
programmed once, together.

**A file's first page** also holds its frame block: the frame ID, the
filename and a version, with a check; and, once the file is closed, its
page count and size, with a check of their own.

**Two kinds of file.** A log is appended to, closed, read and deleted; one
a reset cut short is opened again and appended to. A configuration file, a
Lua program among them, is written whole and read whole; it is written
again as a new copy, which replaces the old one when it closes. A closed
file is final: alfs edits no file.

**A file's life:**
- **A mount** reads every page's header and builds the cache of files in a
  table the application gives: about 12 bytes a file.
- **Allocation** reserves the pages a size needs from the free pages and
  links them, the last link left erased, the first page's header last. The
  list is the reservation.
- **Writes** gather bytes in a block of RAM the writer brings, and program
  each complete page along the list. A sync programs the complete pages and
  keeps the rest; a flush programs the rest too, as a partial page, and the
  file goes on in the next page.
- **Growth.** Past the reservation, a file opened with `ALFS_GROW` takes
  free pages while there are any; without it, the write stops.
- **A close** programs the rest, the close fields and a zero link, and
  blocks out the older copy it replaces.
- **A reopen** after a reset goes on past the end of the file's data, one
  page skipped, so nothing is written over a page a power cut may have torn.
- **A delete** zeros the filename and leaves the pages.
- **The cleanup** is the only thing that erases: sector by sector, the
  sectors that hold no live page.
- **A file is valid** when its pages are: a read of a closed file reports a
  damaged page or a short list at its end.

## Files

| Path | What |
|---|---|
| `docs/REQUIREMENTS.md` | what alfs must do, each requirement testable |
| `docs/TEST_PLAN.md` | how each requirement is tested |
| `docs/SPEC.md` | every byte on the flash |
| `alfs.h` | the types the store and its driver share |
| `alfs_fs.h`, `alfs_fs.c` | the file store |
| `alfs_flash.h`, `alfs_flash.c` | the checked flash operations the store uses |
| `tests/` | the simulated flash, the harness and the test suite |
| `experiments/v6-frames/EVALUATION.md` | this option weighed against the priorities |
| `docs/datasheets/` | the flash parts the figures come from |
| `probe/`, `docs/FLASH_PROBE.md` | the flash probe, which measures the chips themselves, and its measurements |

The use cases come from the pyro flight computer
(github.com/n9wxu/pyro_fw), the flash fault model from the littlefs fork
(github.com/n9wxu/littlefs). alfs shares no code with either.

## Branches

| Branch | What it holds |
|---|---|
| `main` | format version 3: named, versioned files, and the flash task |
| `experiment/v6-frames` | format version 6: files as frames of linked pages, a cache built at mount, the cleanup the only eraser |
| `experiment/v5-rows` | format version 5: log areas of rows, the configuration area one of them, the object area, and the flash task |
| `experiment/v4-object-store` | format version 4: a key-value configuration store, the object area, the log ring, and the flash task |
| `experiment/format-1` | format 1: streams found by an identity and a tag, one flash operation a call; 5.3 KB |
| `experiment/format-2` | format 2: streams with sector numbers, exact programs, and resuming a stream after a reset; 6.8 KB |
| `experiment/v2-safety-rule-cost` | the code each of format 2's safety rules takes, measured by building format 2 without it |
| `experiment/v3-requirement-cost` | the code each requirement takes in format 3, and which tests fail without it |

## Tests and releases

Every push runs every test (`.github/workflows/test.yml`):
- the store on 8 seeds under the sanitizers, built with GCC and with Clang;
- the store on 100 seeds;
- the Cortex-M0+ size and stack budgets, built with Arm GNU Toolchain
  13.2.Rel1, the release the budgets are set for.

A release is made by pushing a tag `v*`. The release workflow runs all of
those tests on the tagged commit, and makes the release only if every one
passes.

## License

MIT (`LICENSE`). The datasheets in `docs/datasheets/` are their makers',
kept here as the sources the figures cite.
