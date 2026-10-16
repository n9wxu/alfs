# alfs on-disk format, version 3

Every byte alfs writes, and how it reads them back. Little-endian
throughout; every check is CRC-32 (reflected, polynomial 0xEDB88320,
initial value and final XOR 0xFFFFFFFF: the CRC of zlib and Ethernet).

Terms are those of `docs/REQUIREMENTS.md` section 4.1. S is the sector
size, P the page size. "Erased" means every byte equals the configured
erase value E; "zero" means every byte equals its complement, ~E. The
prog unit is one byte (requirements A-8, A-9).

Version 3 gives files names and versions (requirements draft 3). Against
version 2:
- The claim drops its 4-byte tag.
- A file's first sector holds its name and a close record in its first
  page, and its data from its second page.
- The rules that resumed a file after a mount are gone.
- Prog units over one byte are gone.

A version 2 region does not mount as version 3.

---

## 1. A sector

| Offset | Size | Field | Written |
|---|---|---|---|
| 0 | 26 | stamp | by an erase, after the erase and a check that every byte is erased |
| 26 | 1 | dead mark | by `alfs_delete`, in a file's first sector only |
| 27 | 16 | claim | by the file that takes the sector |
| D = 43 | | chunks | in a sector that is not a file's first |

A file's first sector has more in its first page, and its chunks start at
its second:

| Offset | Size | Field | Written |
|---|---|---|---|
| 43 | 32 | name | by `alfs_create`, in one program with the stamp, dead mark and claim |
| 75 | 4 | name check | the same program |
| 79 | 8 | close record | by `alfs_close` |
| P | | chunks | by writes and syncs |

P must be at least 128 bytes and S at least four pages (`ALFS_EGEOMETRY`
otherwise).

### 1.1 The stamp

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic, "alfs" |
| 4 | 1 | version, 3 |
| 5 | 1 | log2 S |
| 6 | 1 | log2 P |
| 7 | 1 | 0 |
| 8 | 2 | sectors in the region |
| 10 | 4 | generation: the format that wrote it |
| 14 | 4 | erase count: this sector's erases, this one included |
| 18 | 4 | sequence floor: the next sequence when the stamp was written |
| 22 | 4 | CRC of bytes 0 to 21 |

A stamp is **valid** when its magic, version and CRC are right. It
**matches** when its geometry (bytes 5 to 9) equals the configuration. A
stamp that is valid but does not match makes a mount fail with
`ALFS_EGEOMETRY`, unless a matching one exists.

The **current generation** is the largest generation among the matching
stamps. `alfs_format` writes one greater than any it finds.

The **erase count** written is the sector's previous stamp's count plus one
when that stamp was valid. Otherwise, after a power loss between an erase
and its stamp, or on a chip alfs never formatted, it is the largest count
known in the region, or 1 if none is. A large erase stamps each of its
sectors with one more than the largest valid count among them.

The **next sequence** at mount is two more than the largest of every valid
claim's sequence and every valid stamp's floor, and three more when the
mount finds a torn claim.
- **Why:** a power loss can tear a claim so that it reads invalid now and
  valid later.
- **The bound:** that claim's sequence is at most two more than the largest
  known. Either it was the first sequence its mount gave, two more than the
  largest then, or it followed a claim that is still valid. The floors keep
  the largest sequence known after its sector is erased.
- **So** starting three more past a torn claim never gives its sequence
  again. Two more is not enough: a mount followed by a create torn at once
  left the next mount starting exactly at the torn claim's sequence (found
  with seeds 16 and 30, 2026-10-15).

### 1.2 The dead mark

Erased: the file whose first sector this is lives. Any other value: it
was deleted. alfs writes ~E, in one program, so a delete is atomic. Only a
file's first sector's mark is read.

### 1.3 The claim

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | identity: the sequence of the file's first claim, and its version |
| 4 | 4 | sequence: unique in the region, growing, never reused |
| 8 | 2 | the file's first sector, an index into the region |
| 10 | 2 | sector number: 0 in the file's first sector, one more in each next |
| 12 | 4 | CRC of bytes 0 to 11 |

A sector k is a file's **first** when its claim is valid, its sequence
equals its identity, its first sector is k, and its sector number is 0.

### 1.4 The name

The name's 1 to 31 bytes, none 00h, then 00h to fill 32 bytes. The **name
check** is the CRC of the claim's identity (4 bytes, little-endian) and the
32 name bytes.

A first sector whose name check is wrong, or whose name bytes break the
rule, holds a **nameless** file: a create a power loss tore (requirements
SAFE-13). It lists with an empty name and is deleted by its version.

### 1.5 The close record

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | size: the bytes written to the file |
| 4 | 4 | CRC of the claim's identity (4 bytes) and the size |

Erased, or with a wrong CRC: the file is **unclosed**. With a right CRC it
is **closed**, and its size is known.

A file's first page holds no data. It takes the stamp, the create, the
close record and the dead mark, and in each mount at most once each the
dead mark again (section 6) and the close record again (section 5).

### 1.6 A chunk

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | length of the payload, 1 or more |
| 2 | 4 | CRC of the two length bytes and the payload |
| 6 | length | payload |

A chunk never crosses a page. A sector's first chunk starts at D, or at P
in a file's first sector; each later chunk at the end of the one before,
or at the start of a later page.

---

## 2. What a sector is

Decided from at most 60 bytes: the stamp, the dead mark and the claim, and
for a sector that is not its file's first, the first sector's dead mark
and claim.

| State | When |
|---|---|
| **ready** | the stamp matches, has the current generation, and the dead mark and the claim are erased |
| **live** | the stamp matches, has the current generation, the claim is valid, and the file's first sector has a valid first claim with the same identity and an erased dead mark |
| **dirty** | anything else: no valid stamp, an old generation, a claim neither erased nor valid (**torn**), a zero claim, a deleted or vanished file, or anything not alfs's |
| **bad** | an erase or the check after it failed, during this mount; kept in RAM, at most eight |

A bit error in a claim makes its sector dirty. In a file's first sector it
makes the whole file dirty, and the next erases reclaim it. Chunks are
protected one by one (section 3); claims are not repaired.

Before a file takes a ready sector, the rest of its first page, from the
dead mark on, is read and must be erased. Otherwise the sector is counted
dirty and the next ready one is tried.

---

## 3. Reading

**Finding a file.**
- By version: the live first sector whose identity is that version. If the
  caller also gives a name, the name must match.
- With version 0: the closed file of that name whose identity is
  greatest. Every live first sector's name and close record are read to
  find it.

**A sector's chunks**, from where its chunks start, at position x:

1. Read the six header bytes at x.
2. If they are erased: go to the next page start and continue. The writer
   moves to the next page when a chunk does not fit, or when the page has
   had its programs.
3. If the length is 0, or the chunk would cross its page, or its CRC fails:
   count one failed check, go to the next page start, and continue. The
   length cannot be trusted, so nothing else in that page is either.
4. Otherwise the chunk is good: deliver it, and continue at x + 6 + length.

**A file's sectors, in order.**
- First the file's first sector.
- Then, repeatedly, the live sector whose claim has the same identity and
  the next sector number. alfs finds it by scanning forward, circularly,
  from the sector just read. Allocation moves forward, so it is usually a
  few sectors on, and the scan stops there.
- When no sector has the next number, the one with the smallest larger
  number follows, and the gap counts one failed check.
- When none has a larger number, the file ends.

**Listing** visits every live first sector in growing identity. For each
it reports the name, or none for a nameless file; the identity, as the
version; whether it is closed; and its size if it is.

---

## 4. Writing

**Taking a sector.** alfs scans forward, circularly, from the sector after
the one taken last, for a ready sector that passes section 2's check. With
one exception, the fresh sector:
- What it is: the first sector, as the mount found them, after the sector
  of the newest valid claim of any file, wrapping round, whose stamp is
  current and whose claim is not valid and not zero. Its claim is erased or
  torn, however its bits happen to read.
- Why: that is where the allocation cursor stood when the power was lost,
  so it is the sector a torn claim program would have taken. Such a
  program, torn at its start, leaves a sector that reads ready but holds a
  bit that reads either way (the probe's FLICKER).
- What alfs does: when an allocation reaches it, alfs programs zero over
  its claim, which makes it dirty, and takes the next.

**Creating a file.**
- alfs takes a sector k and gives the file the next sequence as its
  identity.
- One program from byte 0 writes the stamp again, with the bytes it reads;
  the dead mark, erased; the claim (identity, identity, k, 0); the name;
  and the name check.
- Programming the stamp again completes one that an erase left half
  programmed (A-9).
- If the program fails, the sector is counted dirty, the file takes a new
  identity (the failed sector may hold a first claim), and alfs tries the
  next ready sector, up to the configured number of times.

**The next chunk.**
- Where it goes: straight after the last chunk, in the same page while at
  least seven bytes are left in it and the page has had fewer than
  `max_page_programs` programs. Otherwise it goes at the next page's start.
- Past the sector's end, it goes in a new sector, taken as above. One
  program from byte 0 then writes the stamp again (as read), the dead mark
  erased, the claim (identity, next sequence, first sector, number + 1),
  and the chunk.
- How big it is: at most the room left in its page. A write programs the
  block's bytes when the block is full, all but what is left after the last
  whole page's room (requirements IF-12); a sync programs them all.

**A failed program.** The file leaves the sector and programs the same
bytes in a new sector, in the same call, up to the configured number of
times in a row. Then the bytes are dropped and counted. A sector whose
claim program failed is counted dirty.

**Closing.** One program writes the close record: the bytes written to the
file over its life, and the check. A close record that will not program
cannot move, since its place is fixed: the file is released unclosed, and
reads as one a reset cut short.

**After a mount.** No file found unclosed is written again; a new version
takes its place. The power loss that preceded the mount interrupted at most
one program, and every byte that program touched is in the unclosed file's
last page, a sector no file holds yet, or a stamp.

---

## 5. Deleting

1. alfs finds the version's first sector, and refuses if any handle has it
   open.
2. If the file has a whole name, alfs finds the file of the same name with
   the greatest identity above this version whose close record is not
   erased. Names are matched by the CRC of their 32 bytes; a chance match
   only completes another file's close record, which it was writing anyway.
3. If there is such a file, and it has not had this already in this mount,
   alfs programs that file's close record again (A-9). It uses the bytes it
   reads if they are whole. Otherwise it uses the record that close was
   writing: the bytes the file's chunks hold, since a close programs its
   record only after its last chunk.
4. One program writes ~E to the dead mark.

Step 3 means a close a power loss left half programmed cannot read unclosed
once this version is gone. It completes any close record that is not
erased, not only one that reads whole: the application may have seen it
whole in a listing a moment before, and a second read can differ (found by
`safe_config_replace`, 2026-10-15).

---

## 6. Erasing

Erasing happens only when the application asks, one step at a time. A
step scans forward, circularly, from where the last one stopped, for a
dirty sector that is not bad. With a driver that has `erase_large` and a
whole aligned block of dirty sectors, the step takes the block.

**Before erasing,** the step reads each sector to be erased again, and
decides from one read of its file's dead mark:
- A sector that now reads live is left for later. A bit that reads either
  way can make a sector read dirty when chosen and live a moment after.
- A sector that belongs to a deleted file, and is not that file's first,
  has the file's dead mark programmed with ~E again. That is done once per
  file per mount. A dead mark a power loss left half programmed then cannot
  read erased once some of its file's sectors are gone.

**Then** the step erases the sector or block, reads every byte back as
erased, programs the stamp and reads the stamp back. Any failure makes the
sector bad for the rest of the mount.

The stamp, not the blank check, is what shows an erase completed. The probe
found erases aborted at a tenth of their time already reading blank
(`docs/FLASH_PROBE.md` section 4).
