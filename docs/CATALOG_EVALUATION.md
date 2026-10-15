# A file catalog: evaluation

**Status: options for a decision, 2026-10-14.** Nothing here is built. The
v1 format (`docs/SPEC.md`) is unchanged. **Revised by `docs/V2_STRATEGY.md`:**
judged by "minimize writes", D0+ (the claim's sector number) is
recommended over D2, since every catalog adds programs and erases.

---

## 1. The question

The user, 2026-10-14:

> "I am thinking of an initial block (or perhaps sector) that is the
> directory entry. This entry will simply be a pointer to the first block
> of a file. The first file block would be special and keep a catalog of
> blocks (sectors?) used by that file. As a file grows and a new block is
> allocated to the file its address is written to the first file block. If
> the file gets large an extension block can be added to the first file
> block to hold additional pointers. The purpose is to minimize erases and
> speed file processing by having all the file block addresses in one
> place. Evaluate the consequences to size, safety, speed of this idea."

**The answer in short.** A catalog makes reading and opening a file much
faster, most of all when two files were written at the same time. It does
not reduce erases: the v1 format already erases nothing but sectors that
held deleted data. It adds one small program each time a file takes a new
sector. Unless every data sector keeps its own header, it concentrates
risk: one damaged catalog page loses up to 256 KB of a file, where today
one damaged header loses 4 KB. The directory sector costs the most and
gains the least. Option D2 below keeps the catalog and drops the
directory.

---

## 2. What the v1 format does today

Every sector describes itself. Its header is 41 bytes with a prog unit of
1: a 22-byte stamp from its erase, a 1-byte dead mark, and an 18-byte claim
naming its stream and a sequence number unique in the region (SPEC
section 1). There is no directory and no catalog. So:

- **Opening a stream** reads every sector's header to find the stream's
  first sector: `find_stream`, `alfs.c` line 510. On an 8 MB region (2,048
  sectors) that is 2,048 headers, about 84 KB in 6,144 driver reads.
- **Reading on to the next sector** scans for the claim with the same
  stream and the next larger sequence: `next_sector`, `alfs.c` line 535.
  The scan stops early only when that sequence is exactly one more. That
  holds while one stream writes alone. When two streams write at the same
  time, their sequences alternate, nothing stops the scan early, and every
  sector change reads every header. Reading back a 1,000-sector stream on
  2,048 sectors then reads about 2 million headers, roughly 84 MB.
- **Writing** puts the claim and the first chunk of a sector into one
  program, so a stream taking a new sector costs no extra program.
- **Erasing** happens only in erase steps, only to sectors that held
  deleted or foreign data. Nothing else is ever erased.

---

## 3. The options

**D1, the proposal.** A directory sector holds one entry per file: a
pointer to the file's first sector. The first sector is the file's
catalog: one entry per data sector, appended as the file grows. An
extension sector, pointed to from the first sector, takes entries when the
first sector fills. Data sectors need no header of their own beyond the
stamp.

**D2, the catalog as an index.** Every data sector keeps its claim, which
gains a 2-byte sector number within its stream. A stream's first sector
holds its claim and its catalog: 2-byte entries, one per later sector, and
no data. There is no directory: opening a stream still scans for its first
sector, as today. There is no extension sector: past the catalog's
capacity, or wherever an entry is missing or wrong, the reader falls back
to scanning claims, and the sector number lets that scan stop at the
right sector.

**D0+, the smallest change.** Only the claim's 2-byte sector number. A
scan for the next sector stops as soon as it finds the claim with the next
sector number, which is usually within a few sectors.

Appending an entry to a catalog writes still-erased bytes of a page that
already holds others: case 1 of `docs/FLASH_PROBE.md`, not case 2. Case 2
(programming bits of an entry already written) is only needed to change
an entry in place, such as marking a directory entry deleted. A spare
erased byte per entry does the same with case 1.

---

## 4. Consequences

Region sizes are pyro's littlefs regions, which alfs would replace: 246
sectors (MK1B), 1,024 (MK1B-8MB), 2,048 (MK1A, MK1C). Program times are
measured by the probe (`docs/FLASH_PROBE.md`): 2 to 4 bytes take 24-26 us
on the W25Q128JV and 56-61 us on the XT25F128F and BY25Q64ES; a whole page
0.2-0.4 ms.

### 4.1 Speed

| | v1 today | D1 | D2 | D0+ |
|---|---|---|---|---|
| Reading on to the next sector, streams written together | every header in the region | one entry, from a page already read | one entry; a scan only past the catalog's end | a scan that usually stops within a few sectors |
| Opening a stream | every header | the directory and the first sector | every header, as today | every header, as today |
| Mounting | every header | the directory, every catalog, and every stamp: the stamps still say which sectors are erased | every header, as today | every header, as today |
| Extra programs while logging | none | one entry per new sector, before any data goes there; the call that crosses into a new sector takes two calls | one entry per new sector, in the next call with nothing else to program; never in a sync | none |
| Cost of each extra program | | 24-61 us for a 2-4 byte entry, against 200-400 us for a page | the same | |

In D1 the entry must be written before the data. A sector holding synced
data that no catalog lists is lost at the next mount, since without its
own claim nothing says whose it is. In D2 the claim says so, and the entry
may trail behind.

### 4.2 Flash life

| | v1 today | D1 | D2 | D0+ |
|---|---|---|---|---|
| Erases beyond the data's own sectors | none | one per file (its catalog sector), and two more every ~1,000 files created: the directory is rewritten into a second sector and the first erased | one per stream (its catalog sector) | none |
| Programs beyond the data's | none | one per sector | one per sector | none |

None of the options erases less than v1 does today. The directory rewrite
in D1 erases a sector that holds live metadata, the one thing the founding
rulings were written to prevent in flight. It needs two directory
sectors, so that a power cut mid-rewrite leaves one whole copy, and it
must only run in erase steps on the ground.

### 4.3 Size

| | v1 today | D1 | D2 | D0+ |
|---|---|---|---|---|
| Flash per data sector | 41 B header | 22 B stamp and a 4 B entry (2 B pointer, 2 B check): 15 B less, 0.4% of a sector | 43 B header and a 2 B entry | 43 B header |
| Flash per file | none | one catalog sector, plus an extension sector past ~1,000 sectors | one catalog sector, holding ~2,000 entries: nearly 8 MB, all but the last ~20 sectors of an MK1A or MK1C region | none |
| Fixed | none | two directory sectors | none | none |
| Code (5,301 B today, budget 6,144 B) | | directory, its rewrite, catalog, extension, and unlisted sectors: likely past the budget (estimate) | a few hundred bytes (estimate) | under 100 bytes (estimate) |
| RAM | | finding sectors no catalog lists needs a map of used sectors that grows with the region (31 B to 256 B on pyro's regions), or they are lost until a format | 2-4 B per stream, within the 32-byte stream budget | none |

### 4.4 Safety

| | v1 today | D1 | D2 | D0+ |
|---|---|---|---|---|
| A torn or flipped entry | | points at another sector. Without a check on each entry, and a chunk CRC that covers the file's identity, another file's data reads back as this one's | checked against the target's claim: a wrong entry only sends the reader to the scan | |
| One damaged page | loses one sector (4 KB), or the stream if it is the first sector's claim | a catalog page loses the 64 sectors it lists (256 KB); the directory loses every file unless it is kept twice | as today | as today |
| Programs into one page | at most 16 (LIFE-06) | up to 64 into a catalog page | up to 128 into a catalog page: LIFE-06 must allow it, which A-2 and A-8 cover where the probe passed | at most 16 |
| A power cut | covered by SAFE-01 to SAFE-04 | the directory rewrite and the entry before the data are new orderings to prove | entries may be lost; the scan covers them. After a mount, entries continue on a fresh catalog page (SAFE-04), so a catalog sector covers ~15 mounts before falling back to scans | as today |
| Relies on case 2 | no | only for deleting directory entries in place | no | no |

---

## 5. Block or sector

Allocate in 4 KB sectors. With 64 KB blocks as the unit, MK1B's 984 KB
region holds 15; a directory and one catalog block per file would take
most of it, and each file would waste 32 KB on average at its end. A
64 KB erase is faster per byte, 150 ms against 16 x 45 ms (W25Q128JV
p. 66), but the v1 format already erases a whole aligned 64 KB block at
once whenever all of it is dirty (LIFE-07), and every erase happens
before flight.

---

## 6. Recommendation

**D2.** It puts every sector's address in one place, which is the speed
the proposal is after. It keeps each sector's claim, so a damaged catalog
only slows reading down and never loses or mixes data. It needs no
directory and so no metadata erase, no extension sector, and no case 2.
It costs one erase per stream and one 24-61 us program per sector.

If only reading a whole log back matters, and seeking does not, D0+ gets
most of the reading speed for 2 bytes a sector and almost no code.

**For the user to decide:**

1. D1, D2 or D0+.
2. For D2: a catalog sector of its own per stream, as above, or a fixed
   catalog area at the front of the first sector that also holds data
   (smaller files cost a sector less; the catalog's size is fixed when the
   stream starts).
3. For D2: raising LIFE-06 to 128 programs on a catalog page. The probe
   programmed pages 2,048 times on every chip it tested; the W25Q16JV
   (MK1B) is not yet tested.
