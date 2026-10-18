# alfs

The append-log flash store: files on serial NOR flash, each a frame of
pages linked in a list, found by filename through a catalog. A file's pages
can be reserved ahead of time, so that writing it in flight programs only
flash already erased. Built for systems that run code from the same chip
and cannot afford to stall.

Priorities, in order: speed, then flash life, then safety. A corrupted data
block is less important than a broken exec loop.

**Status:** format version 6, requirements and tests. There is no code on
this branch yet.
- **Requirements:** `docs/REQUIREMENTS.md` draft 6. Its section 8 lists the
  decisions still open.
- **Format:** `docs/SPEC.md`, every byte.
- **Interface:** `alfs_fs.h`.
- **Tests:** 46 in `tests/test_alfs.c`, one or more a requirement. They
  compile against the header; they run once `alfs_fs.c` exists.
- **Evaluation:** `experiments/v6-frames/EVALUATION.md`.

## How it works

The application gives alfs one area of the chip. Its first two sectors are
the catalog; every page after them can belong to a file.

**A page** starts with a header:
- the frame ID of its file;
- its position in the file;
- a check;
- the next page's address. Erased means no next page yet; zero means the
  file's last page.

Its byte count, data and data check follow, programmed once, together.

**A file's first page** also holds its frame block: the frame ID, the
filename, and, once the file is closed, its page count and size, with a
check of their own.

**The catalog** maps each filename to its frame ID and first page. The
filename converts to the frame ID, the file's descriptor.

**Two kinds of file.** A log is appended to, closed, read and deleted; one
a reset cut short is opened again and appended to. A configuration file, a
Lua program among them, is written whole and read whole, and written again
as a new file. A closed file is final: alfs edits no file.

**A file's life:**
- **Allocation** reserves the pages a size needs and links them, the last
  link left erased. The list is the reservation.
- **Writes** gather bytes in a block of RAM the writer brings, and program
  each complete page along the list. A sync programs the complete pages and
  keeps the rest; a flush programs the rest too, as a partial page, and the
  file goes on in the next page.
- **Growth.** Past the reservation, a file opened with `ALFS_GROW` takes
  pages already erased while there are any; without it, the write stops.
- **A close** programs the rest, the close fields and a zero link, then
  erases the reservation's sectors that hold no live page.
- **A reopen.** A file not closed is reopened at its first page that reads
  erased.
- **A file is valid** when its pages are: a read of a closed file reports
  a damaged page or a short list at its end.
- **A delete** zeros the filename and leaves the pages.
- **A cleanup** erases, sector by sector, the sectors that hold no live
  page.

## Files

| Path | What |
|---|---|
| `docs/REQUIREMENTS.md` | what alfs must do, each requirement testable |
| `docs/TEST_PLAN.md` | how each requirement is tested |
| `docs/SPEC.md` | every byte on the flash |
| `alfs.h` | the types the store and its driver share |
| `alfs_fs.h` | the file store's interface |
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
| `experiment/v6-frames` | format version 6: files as frames of linked pages, with a catalog; requirements and tests |
| `experiment/v5-rows` | format version 5: log areas of rows, the configuration area one of them, the object area, and the flash task |
| `experiment/v4-object-store` | format version 4: a key-value configuration store, the object area, the log ring, and the flash task |
| `experiment/format-1` | format 1: streams found by an identity and a tag, one flash operation a call; 5.3 KB |
| `experiment/format-2` | format 2: streams with sector numbers, exact programs, and resuming a stream after a reset; 6.8 KB |
| `experiment/v2-safety-rule-cost` | the code each of format 2's safety rules takes, measured by building format 2 without it |
| `experiment/v3-requirement-cost` | the code each requirement takes in format 3, and which tests fail without it |

## Tests and releases

On this branch every push compiles the tests against `alfs_fs.h`, with GCC
and with Clang (`.github/workflows/test.yml`). The suite, the soak and the
Cortex-M0+ budgets need the store, which is not written. A release, made by
pushing a tag `v*`, is refused until it is.

## License

MIT (`LICENSE`). The datasheets in `docs/datasheets/` are their makers',
kept here as the sources the figures cite.
