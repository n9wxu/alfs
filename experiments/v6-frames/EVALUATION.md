# Evaluation: files as frames of linked pages (format 6)

The option the user set out on 2026-10-17, as captured in
`docs/REQUIREMENTS.md` draft 6 and `docs/SPEC.md` format 6, weighed against
the brief's priorities: speed, then flash life, then safety, with
simplicity as the means. There is no code, so every figure here is computed
from the format, or estimated where marked. The option it is set against is
`experiment/v5-rows`, whose figures are measured.

---

## 1. Bottom line

- **Speed:** as good as any option so far. In flight, a write is one
  program per full page, about 0.4 ms. Nothing erases there as long as the
  file was allocated before flight.
- **Flash life:** better than the rows option, about 8% fewer erases
  logging. The cost of the reservation scheme is a second program into
  every page, and an extra erase for every whole sector reserved but not
  used.
- **Safety:** weaker than the rows option in four places, each one a
  decision of the design rather than a defect in it:
  - D-1: the durability step is a page.
  - D-4: reopening a closed file rewrites sectors.
  - D-6: nothing on the flash proves an erase completed.
  - D-7: a Lua update deletes before it writes.

  Each has an alternative that keeps the design's shape (section 4).
- **Simplicity:** one store for logs, settings and Lua programs, where the
  rows option has two. Its size is estimated at 5.9 to 7.4 KB, against 6.6 KB
  measured for the rows option's two stores.

**Recommendation:** pursue it if the user takes the alternatives to D-1,
D-4, D-6 and D-7. Taken as written, it gives up safety the rows option
already has, for a gain in flash life of about 8%.

---

## 2. The option in brief

- **Two layers.** A file is a frame. Its first page carries the frame
  block: frame ID, filename, then at close the page count, size, checksum
  and a check over them. Every page carries a header (frame ID, its
  position, a header check, the next page's address) and a data check.
- **The catalog** maps filenames to frame IDs and first pages.
- **Lifecycle:**
  - `file_alloc` links a file's pages ahead of time.
  - Writes program whole pages along the list.
  - A close writes the close fields, ends the list with a zero link, and
    erases the reservation's unused whole sectors.
  - A delete zeros the filename.
  - A cleanup erases, sector by sector, the sectors that hold no live page.

**What the review settled:**
- the filename in the frame;
- the reservation not stored, the list being the reservation;
- the four checks and the byte size;
- the reboot rule: a page that reads erased is erased;
- `ALFS_GROW` to allow writes past the reservation.

**Additions of mine** (marked in the requirements):
- the catalog as two sectors (D-5);
- one frame per filename, and the order of allocation and close (SAFE-11, SAFE-12);
- the count of the reservation at open, so that a writer without
  `ALFS_GROW` takes only bytes it can program.

---

## 3. Against the priorities

### 3.1 Speed

| | Format 6 | Rows option (v5) |
|---|---|---|
| A write in flight | one program a full page, data and check: about 0.4 ms | one program a chunk, up to a page: about 0.4 ms |
| Erases in the write path | none, for a file allocated before flight | none |
| Erases outside it | allocation (when short of free pages), close, reopen of a closed file, cleanup, catalog rewrite | only when the application asks |
| Mount | at most two sectors, 8 KB, whatever the area | 30 bytes a sector: 61 KB on an 8 MB area |
| Open for writing | a header for each page of the list: 39 KB for the 10-minute log | none beyond the first sector |
| Reopen after a reset | every page's data once, for the size and checksum: 710 KB and its CRC for the 10-minute log, about 0.2 to 0.3 s (estimated) | a new log: one sector |

**Two speed costs that matter in flight:**
- **A close erases** (D-2). If the flight log is closed while the flight
  loop runs, on the pad or in flight, each unused sector stalls it 35 to
  800 ms.
- **Reopening after a reset** reads the whole file (UC-3: the time from
  reset to the first sample is flight time lost). The frame checksum is
  why: it covers every byte. A frame checksum taken over the pages' data
  checks would cut this to the headers and 4 bytes a page, about 50 KB.

### 3.2 Flash life

| On W-LOG, 10 minutes at 50 rows a second (660,000 bytes) | Format 6 | Rows option (v5) |
|---|---|---|
| Data in a 256-byte page | 238 bytes (93.0%) | ten 22-byte rows, 220 bytes (85.9%) |
| Sectors the log fills | 174 (computed) | 189 (measured) |
| Erases for the log's life | about 174, cleanup after it is deleted, plus any unused whole sectors at close | 189 |
| Programs into each page | two: the header at allocation, the data later | one, plus a claim per sector |

- **The gain comes from not aligning rows with pages.** No page end goes
  unused.
- **The reservation costs.** Each whole sector reserved and not used is:
  1. erased by a cleanup;
  2. programmed with headers at allocation;
  3. erased again at close, with no data ever in it.

  Over-reserving a flight log by a minute costs about 17 erases a flight.
  Sizing reservations close to the flight matters.
- **Every page takes two programs per erase.** A-2 allows at least 16, so
  this is within the flash's rules. It doubles the program count, but not
  the wear, which erases set.

### 3.3 Safety

What holds:
- **Allocation is atomic** (SAFE-11): the entry goes first, then the pages
  from last to first. The file exists once its first page is on the flash.
- **A close is atomic** (SAFE-12): the close fields carry their own check
  and are written after the last data and before the zero link.
- **Damage is detected.** Every page's data and header carry checks
  (SAFE-03, SAFE-15), and a closed file's frame checksum catches a page
  whose check passes but whose bytes are wrong (SAFE-14).
- **A delete holds** (SAFE-13). The filename is zeroed in both places, so
  neither the catalog nor the frames name the file again.

Where it is weaker than the rows option:

| | Format 6 | Rows option (v5) |
|---|---|---|
| Lost at a power cut, after a sync | up to a page's data less a byte, 237 bytes: at 1 row a second, 10.8 s of log (D-1) | nothing synced |
| A failed or damaged page | its bytes; the rows that cross it are cut, and the application must find where rows start again | whole rows; the rows after it read in step |
| A Lua update | delete, then write: a power cut between leaves no program (D-7) | atomic: the old object reads until the new one commits |
| Reopening a closed file | sector rewrite: a power cut loses that sector's pages, other files' too (D-4) | not done: a new log |
| An erase cut short that reads erased | used as erased (D-6) | refused: the stamp shows an erase completed |
| A page torn so that it reads erased, after a reset | written over; its check shows damage and the page is lost (D-3) | never written again (SAFE-04) |

### 3.4 Simplicity

- **One store for everything.** Logs, settings and Lua programs are all
  files. The rows option needs a log store, an object store and an
  application for settings.
- **Code, estimated.** By function, from the rows option's measured parts:

  | Part | Estimate |
  |---|---|
  | catalog | 1.2 to 1.5 KB |
  | allocation | 0.8 to 1.0 KB |
  | writes, growth and close | 1.2 to 1.5 KB |
  | reopen of a closed file | 0.5 to 0.7 KB |
  | reads | 0.5 to 0.6 KB |
  | cleanup and liveness | 0.6 to 0.9 KB |
  | delete, list, status | 0.4 to 0.5 KB |
  | the shared flash layer | 0.7 KB |
  | **total** | **5.9 to 7.4 KB** |

  The rows option's two stores measure 6.6 KB. SIMP-02's 6 KiB is an
  estimate the code may not meet.
- **RAM:** an instance of about 80 bytes and a writer of 36, plus their
  pages. Reopening a closed file needs a 4,096-byte block.
- **The cost of liveness.** A cleanup must know, for every page header,
  whether its file is live. That is a catalog search and a read of the
  file's close fields, per file met in a sector. It is slow, but it runs
  on the ground.

---

## 4. The decisions, and their alternatives

**D-1. Sync is of complete pages.** At low rates this breaks FLT-LOG-06,
which allows one second of loss.
- (a) Accept it at low rates.
- (b) Let a sync program a partial page with its length and check; the
  file goes on in the next page, and the rest of that page is left unused.
  The cost is one page a sync at low rates: 600 pages, about 38 sectors, in
  a 10-minute log at 1 row a second.
- (c) Let a page take more than one program, each with its own check.
  That is the chunk scheme the user set aside.

Recommended: (b), with the sync rate the application's to choose.

**D-2. A close erases.**
- (a) As written.
- (b) A close only marks; the unused sectors are dead until a cleanup,
  which the application runs on the ground anyway (UC-5).

Recommended: (b). It removes the only erase a flight-time call makes.

**D-3. A page that reads erased after a reset is erased.**
- (a) As written: the cost is at most the page written over, which its
  check catches.
- (b) Skip one page past the last written one after a reset: one page a
  reset, and nothing written over torn bits.

Both are tested; (b) is the stricter.

**D-4. A closed file is reopened by rewriting sectors.**
- (a) As written.
- (b) Make a close supersedable: leave the last link erased (the page count
  marks the end), and give the frame block two or more close slots. A
  reopen zeros the slot in use; the next close writes the next one. No
  erase, no window, no 4 KB block, at the price of a fixed number of
  reopens per file.

Recommended: (b).

**D-5. The catalog is two sectors.** One sector cannot be rewritten when
full without a moment holding no catalog.
- (a) Two sectors, as captured.
- (b) One sector, rebuilt from the frames' first pages when it fills or is
  lost. The filename is in the frame, so this is possible; a deleted file's
  zeroed filename keeps it out. The cost is a scan of every page header,
  about 460 KB on an 8 MB area, at each rebuild.

**D-6. Nothing shows an erase completed.**
- (a) As written: the read-back after the erase is the only check, and the
  probe found erases cut short that read erased (P9).
- (b) A 16-byte stamp in each sector's first page, programmed after the
  erase. It costs 16 bytes of that page's data.

Recommended: (b).

**D-7. A Lua update deletes, then writes.**
- (a) As written.
- (b) Write the new file first. Let a filename name its newest closed
  frame, then block out the old one. The update becomes atomic, and the
  old program reads until the new one is whole. It needs one change to
  IF-05: a filename names one closed file, or two only while an update is
  under way.

Recommended: (b).

---

## 5. What exists

- **`docs/REQUIREMENTS.md` draft 6:** 10 speed, 5 flash-life, 17 safety,
  5 simplicity, 17 interface and 3 observability requirements, the open
  decisions in its section 8.
- **`docs/SPEC.md` format 6:** every byte.
- **`alfs_fs.h`:** the interface.
- **`tests/test_alfs.c`:** 47 tests, one or more a requirement, with
  power-loss sweeps over every operation of a write run, an allocation, a
  close, a delete, a catalog rewrite and a reopen of a closed file. They
  compile with `-Werror` under Clang and GCC. They cannot run until
  `alfs_fs.c` is written.
- **Not specified:** the flash task, until this interface settles.
