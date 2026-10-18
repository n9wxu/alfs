# Evaluation: files as frames of linked pages (format 6)

The option the user set out on 2026-10-17, as captured in
`docs/REQUIREMENTS.md` draft 6 and `docs/SPEC.md` format 6, weighed against
the brief's priorities: speed, then flash life, then safety, with
simplicity as the means. There is no code, so every figure here is computed
from the format, or estimated where marked. The option it is set against is
`experiment/v5-rows`, whose figures are measured.

---

## 1. Bottom line

- **Speed:** as good as any option so far. In flight a write is one program
  per page, about 0.4 ms, and nothing erases in a file allocated before
  flight. Reopening a log after a reset reads a header and a byte count per
  page, about 45 KB for a 10-minute log.
- **Flash life:** 236 data bytes in a 256-byte page. A 10-minute log fills
  175 sectors, against 189 for the rows option: about 7% fewer erases.
  Flushing costs a page per flush, so the application sets durability
  against wear.
- **Safety:** close to the rows option. Two places stay weaker:
  - A damaged page cuts the rows that cross it, because rows do not align
    with pages; the rows option loses whole rows.
  - A configuration update deletes before it writes (D-4).

  The reboot rule (D-2) is the user's choice, and its damage is detected.
- **Simplicity:** one store for logs, configuration and Lua programs.
  Estimated at 5.5 to 6.8 KB, against 6.6 KB measured for the rows option's
  two stores.

**Recommendation:** pursue it. Make the close leave its erases to the
cleanup (D-1), and write a configuration file's new copy before blocking
out the old (D-4).

---

## 2. The option in brief

- **Two kinds of file.**
  - A **log** is allocated, opened, appended to, closed, read and deleted.
    One cut short by a reset, never closed, is opened again and appended
    to.
  - A **configuration file**, Lua programs among them, is written whole and
    read whole, as the web interface's GET and PUT do. Writing it again
    makes a new file. A closed file is final; no file is edited.
- **Two layers.**
  - A file is a frame. Its first page carries the frame block: frame ID,
    filename, and at close the page count and size, with their check.
  - Every page carries a header (frame ID, position, a header check, the
    next page's address), then its byte count, data and data check, all
    programmed once.
- **The catalog** maps filenames to frame IDs and first pages.
- **Writes and syncs** program full pages. A **flush** programs a partial
  page and the file goes on in the next. A **close** writes the close
  fields, ends the list with a zero link, and erases the reservation's
  unused whole sectors.
- **A delete** zeros the filename. **A cleanup** erases the sectors that
  hold no live page.
- **A file is valid when its pages are.** There is no whole-file checksum;
  a read of a closed file that met a damaged page, or a list cut short,
  ends with `ALFS_ECHECK`.
- **An erase is finished** when the sector reads back all 1s.

**My additions, marked in the requirements:**
- the catalog as two sectors (D-3);
- one file per filename;
- the order of allocation and close (SAFE-11, SAFE-12);
- the count of the reservation at open, so that a writer without
  `ALFS_GROW` takes only bytes it can program.

---

## 3. Against the priorities

### 3.1 Speed

| | Format 6 | Rows option (v5) |
|---|---|---|
| A write in flight | one program a full page: about 0.4 ms | one program a chunk, up to a page: about 0.4 ms |
| A flush | one program, of the partial page | one program, of the chunk |
| Erases in the write path | none, for a file allocated before flight | none |
| Erases outside it | allocation (when short of free pages), close, cleanup, catalog rewrite | only when the application asks |
| Mount | at most two sectors, 8 KB, whatever the area | 30 bytes a sector: 61 KB on an 8 MB area |
| Open for writing | a header for each page of the list: 39 KB for the 10-minute log | none beyond the first sector |
| Reopen after a reset | a header and a byte count for each page, and one page: about 45 KB | a new log: one sector |

The one speed cost left in flight is **a close that erases** (D-1). If the
flight log is closed while the flight loop runs, on the pad or in flight,
each unused sector stalls it 35 to 800 ms.

### 3.2 Flash life

| On W-LOG, 10 minutes at 50 rows a second (660,000 bytes) | Format 6 | Rows option (v5) |
|---|---|---|
| Data in a 256-byte page | 236 bytes (92.2%) | ten 22-byte rows, 220 bytes (85.9%) |
| Sectors the log fills, synced each second | 175 (computed) | 189 (measured) |
| Programs into each page | two: the header at allocation, the data later | one, plus a claim per sector |

- **Not aligning rows with pages is where the gain comes from.** No page
  end goes unused, except where a flush leaves one.
- **A flush costs the rest of a page.** At 1 row a second, flushed each
  second, a 10-minute log takes 600 pages (38 sectors), where synced it
  takes 57. At 50 rows a second, flushed each second, it takes about 3,000
  pages (188 sectors), where synced it takes 2,797 (175).
- **Unused reservation costs erases.** Each whole sector reserved and not
  used is:
  1. erased by a cleanup;
  2. programmed with headers at allocation;
  3. erased again at close, with no data ever in it.

  Over-reserving a flight log by a minute costs about 17 erases a flight.
- **Every page takes two programs per erase.** A-2 allows at least 16.

### 3.3 Safety

What holds:
- **Allocation is atomic** (SAFE-11): the entry goes first, then the pages
  from last to first, so the file exists once its first page is written.
- **A close is atomic** (SAFE-12): the close fields carry their own check
  and are written after the last data and before the zero link.
- **What was flushed survives** any later power loss (SAFE-02). A flush
  each second meets FLT-LOG-06.
- **Damage is detected and reported.** Every page's data and header carry
  checks (SAFE-03, SAFE-15). A closed file read to its end reports a
  damaged page or a short list (SAFE-14), so a configuration file or a Lua
  program partly lost is never taken as whole.
- **A delete holds** (SAFE-13). The filename is zeroed in both places, so
  neither the catalog nor the frames name the file again.

Where it differs from the rows option:

| | Format 6 | Rows option (v5) |
|---|---|---|
| Lost at a power cut | after a flush, nothing; after a sync, under a page | nothing synced |
| A failed or damaged page | its bytes; the rows that cross it are cut, and the application must find where rows start again | whole rows; the rows after it read in step |
| A configuration or Lua update | delete, then write: a power cut between leaves no file (D-4) | atomic: the old object reads until the new one commits |
| An erase cut short that reads erased | used as erased, by the user's ruling | refused: the stamp shows an erase completed |
| A page torn so that it reads erased, after a reset | written over; its check shows damage and the page is lost (D-2) | never written again |

### 3.4 Simplicity

- **One store for everything.** Logs, configuration and Lua programs are
  all files. The rows option needs a log store, an object store and an
  application for settings.
- **Code, estimated.** By function, from the rows option's measured parts:

  | Part | Estimate |
  |---|---|
  | catalog | 1.2 to 1.5 KB |
  | allocation | 0.8 to 1.0 KB |
  | writes, growth, flush and close | 1.3 to 1.6 KB |
  | reads | 0.5 to 0.6 KB |
  | cleanup and liveness | 0.6 to 0.9 KB |
  | delete, list, status | 0.4 to 0.5 KB |
  | the shared flash layer | 0.7 KB |
  | **total** | **5.5 to 6.8 KB** |

  SIMP-02's 6 KiB sits within that range.
- **RAM:** an instance of about 80 bytes and a writer of 32, plus a page
  each.
- **The cost of liveness.** A cleanup must know, for every page header,
  whether its file is live. That is a catalog search and a read of the
  file's close fields, per file met in a sector. It is slow, but it runs
  on the ground.

---

## 4. The decisions still open, and their alternatives

**D-1. A close erases.**
- (a) As written.
- (b) A close only ends the list; the unused sectors are dead until a
  cleanup, which the application runs on the ground anyway (UC-5).

Recommended: (b). It removes the only erase a flight-time call makes.

**D-2. A page that reads erased after a reset is erased.**
- (a) As written: the cost is at most the page written over, which its
  check catches.
- (b) Skip one page past the last written one after a reset: one page a
  reset, and nothing written over torn bits.

Both are tested; (b) is the stricter.

**D-3. The catalog is two sectors.** One sector cannot be rewritten when
full without a moment holding no catalog.
- (a) Two sectors, as captured.
- (b) One sector, rebuilt from the frames' first pages when it fills or is
  lost. The filename is in the frame, so this is possible; a deleted file's
  zeroed filename keeps it out. The cost is a scan of every page header,
  about 460 KB on an 8 MB area, at each rebuild.

**D-4. A configuration update deletes, then writes.**
- (a) As written.
- (b) Write the new file first. Let a filename name its newest closed
  frame, then block out the old one. The update becomes atomic, and the
  old file reads until the new one is whole. It needs one change to IF-05:
  a filename names one closed file, or two only while an update is under
  way.

Recommended: (b).

---

## 5. What exists

- **`docs/REQUIREMENTS.md` draft 6:** 10 speed, 5 flash-life, 16 safety,
  5 simplicity, 18 interface and 3 observability requirements, the open
  decisions in its section 8.
- **`docs/SPEC.md` format 6:** every byte.
- **`alfs_fs.h`:** the interface.
- **`tests/test_alfs.c`:** 46 tests, one or more a requirement, with
  power-loss sweeps over every operation of a write run, synced and
  flushed, an allocation, a close, a delete and a catalog rewrite. They
  compile with `-Werror` under Clang and GCC. They cannot run until
  `alfs_fs.c` is written.
- **Not specified:** the flash task, until this interface settles.
