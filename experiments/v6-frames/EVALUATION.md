# Evaluation: files as frames of linked pages (format 6)

The option the user set out on 2026-10-17, as built: `docs/REQUIREMENTS.md`
draft 6, `docs/SPEC.md` format 6, `alfs_fs.c`. Figures marked measured come
from the simulator or the Cortex-M0+ build; the rest are computed from the
format, or estimated where marked. The option it is set against is
`experiment/v5-rows`, measured the same way.

---

## 1. Bottom line

- **Speed.** In flight, a write is one program per page, about 0.4 ms, and
  nothing erases: the cleanup is the only call that does. A mount reads
  every page's header: about 2 µs a page on pyro's flash (estimated), 50 to
  100 ms for an 8 MB area.
- **Flash life.** 236 data bytes in a 256-byte page. A 10-minute flight log
  costs 175 erases over its life (measured), against 189 for the rows
  option.
- **Safety.** Every power-loss sweep passes, on 100 seeds: allocation,
  writing, reopening, closing, updating and deleting. A damaged page cuts
  the rows that cross it, since rows do not align with pages.
- **Simplicity.** One store for logs, configuration and Lua programs:
  6,664 bytes (measured), against 6,642 for the rows option's two stores.

---

## 2. What the design settled

**The user's rulings:**
- Pages linked in a list, the list being the reservation.
- A page's data programmed once, with a byte count; a flush leaves a
  partial page.
- A frame block with the filename and a version.
- No catalog: a cache built at mount from the pages.
- The cleanup the only eraser, an erase finished when the sector reads all
  1s.
- No whole-file checksum: a file is valid when its pages are.
- Logs and whole-file configuration only: no edits, closed files final.
- An update writes the new copy first; its close blocks out the old.
- After a reset, the page after the file's data is skipped (0000h in its
  byte count).

**Mine, marked in the requirements:**
- **Page 0 holds no file**, since its address marks a list's end.
- **One file per filename**, or two copies during an update.
- **Allocation order:** the pages are programmed from the last to the
  first, and the first page's header after the rest of it, so the copy
  exists only once whole.
- **The open check:** the frame block's frame ID, filename and version
  carry a check, so a filename a power loss left zeroed in part reads as
  blocked out, not as another filename.
- **Close order:** the close fields are written before the zero link.
- **Bytes that must stay erased:** no program covers them, so a
  reservation's last page is programmed without its link.
- **A page taken by an allocation** must read erased on 16 reads of the
  bytes an allocation programs. A header a power cut tore can read erased
  but for one bit, and nothing records where an allocation stood before a
  reset.
- **Stale copies:** a copy a power cut left behind its replacement is
  recorded stale at mount, and blocked out by the next cleanup, allocation
  or delete.
- **Frame IDs:** the next frame ID adds the count of torn headers to the
  largest seen.
- **The cleanup waits** while any file is open for reading.

---

## 3. Against the priorities

### 3.1 Speed

| | Format 6 | Rows option (v5) |
|---|---|---|
| A write in flight | one program a full page: about 0.4 ms | one program a chunk, up to a page: about 0.4 ms |
| Erases outside the cleanup | none | none outside the erase call |
| Mount | a header a page, and each first page's frame block: 50 to 100 ms on 8 MB (estimated) | 30 bytes a sector: under 1 ms on 8 MB |
| Open for writing | two walks of the list, a header and a byte count a page: about 84 KB for a 10-minute log | none beyond the first sector |
| Allocation of a 10-minute log | 2,797 pages checked free on 16 reads of their first 65 bytes, twice, and 2,798 programs: about 6 MB read and 0.2 s of programs (estimated) | none: a log takes sectors as it goes |

The slowest calls are on the ground: allocation, mount, and the cleanup's
final lap, which reads the area once. The mount also runs after a reset in
flight, where its time delays the first sample.

### 3.2 Flash life

| On W-LOG, 10 minutes at 50 rows a second | Format 6 | Rows option (v5) |
|---|---|---|
| Data in a 256-byte page | 236 bytes (92.2%) | ten 22-byte rows, 220 bytes (85.9%) |
| Erases over the log's life | 175 (measured) | 189 (measured) |
| Programs into each page | two: the header at allocation, the data later | one, plus a claim per sector |

- **A flush costs the rest of its page.** At 1 row a second, a flush each
  second, a 10-minute log takes 600 pages instead of 57.
- **A reopen after a reset costs a page,** the skipped one.
- **Unused reservation costs erases.** It is erased again by the cleanup,
  with no data ever in it.

### 3.3 Safety

Tested under every power-loss behaviour at every operation, on 100 seeds:
- **Allocation** is atomic: the file exists only with every reserved page
  in its list.
- **A close** is atomic.
- **An update** reads the old copy or the new one, whole; once the close
  returns, the new.
- **A delete** is permanent once it returns.
- **No file appears** under a filename no one wrote.
- **A reopen after a loss** writes nothing over a torn page, and keeps
  every synced byte.
- **A flushed byte** survives any later power loss.

Where it differs from the rows option:
- **Damaged pages and rows.** A damaged or lost page loses its bytes, and
  the rows that cross it are cut; the rows option loses whole rows.
- **Erase completion.** An erase cut short that reads erased counts as
  finished, by the user's ruling. The probe found such erases (P9); the
  rows option's stamp refused them.
- **A torn header in a free page** is found with high probability, not
  certainty (16 reads, each with a half chance of showing the torn bit).
- **Nothing marks the area as alfs's** (D-1): an area configured over
  something else mounts with no file, and a cleanup erases it.

### 3.4 Simplicity

| Part | Bytes (measured) |
|---|---|
| mount, with its scan | 588 |
| allocation: finding free pages and linking them | 938 |
| opening for writing, writing, growth, syncs and flushes | 928 |
| the end of a file's data at open | 404 |
| close | 272 |
| reading: opening, reading, closing | 816 |
| listing, delete, cleanup, status | 944 |
| the cache, headers and links, shared by the rest | 1,078 |
| the shared flash layer | 696 |
| **total** | **6,664** |

- **No part is large:** the largest single function is the mount's, 588
  bytes.
- **RAM:** an instance of 84 bytes, a writer of 36 and a reader of 28, plus
  a page each, and 12 bytes a cached file.

---

## 4. The decision still open

**D-1. Nothing on the flash marks the area as alfs's.**
- (a) As built: any area mounts. Its foreign contents are dead pages, and
  the cleanup erases them. The area's configured bounds are the only
  guard, as they are for the chip's end.
- (b) One sector holding a stamp, written by a format and never erased by a
  cleanup. A mount refuses an area without it, at the cost of a sector and
  a format call.
