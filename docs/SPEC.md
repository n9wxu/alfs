# alfs on-disk format, version 2

Every byte alfs writes, and how it reads them back. Little-endian
throughout; every check is CRC-32 (reflected, polynomial 0xEDB88320,
initial value and final XOR 0xFFFFFFFF: the CRC of zlib and Ethernet).

Terms are those of `docs/REQUIREMENTS.md` section 4.1. S is the sector
size, P the page size, U the prog unit, and RU(x) is x rounded up to a
multiple of U. "Erased" means every byte equals the configured erase value
E; "zero" means every byte equals its complement, ~E.

Version 2 adds to version 1 a sequence floor in the stamp, a sector number
in the claim, and the rules of sections 4 and 5 that program a claim or
dead mark a second time (`docs/V2_STRATEGY.md`). A version 1 region does
not mount as version 2.

---

## 1. A sector

| Offset | Size | Field | Written |
|---|---|---|---|
| 0 | 26 | stamp | by the erase step, after the erase and a check that every byte is erased |
| DM = RU(26) | 1 | dead mark | by `alfs_delete`, in a stream's first sector only |
| C = DM + RU(1) | 20 | claim | by the stream that takes the sector, in one program with its first chunk |
| D = C + 20 | | chunks | by appends and syncs |

The claim and its first chunk share one program, so they share a page:
P must be at least 64 bytes (`ALFS_EGEOMETRY` otherwise), and a sector at
least four pages.

| U | DM | C | D |
|---|---|---|---|
| 1 | 26 | 27 | 47 |
| 16 | 32 | 48 | 68 |
| 256 | 256 | 512 | 532 |

### 1.1 The stamp

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic, "alfs" |
| 4 | 1 | version, 2 |
| 5 | 1 | log2 S |
| 6 | 1 | log2 P |
| 7 | 1 | log2 U |
| 8 | 2 | sectors in the region |
| 10 | 4 | generation: the format that wrote it |
| 14 | 4 | erase count: this sector's erases, this one included |
| 18 | 4 | sequence floor: the next sequence when the stamp was written |
| 22 | 4 | CRC of bytes 0 to 21 |

A stamp is **valid** when its magic, version and CRC are right. It
**matches** when its geometry equals the configuration. A stamp that is
valid but does not match makes a mount fail with `ALFS_EGEOMETRY`, unless a
matching one exists.

The **current generation** is the largest generation among the matching
stamps. `alfs_format` writes one greater than any it finds.

The **erase count** written is the sector's previous stamp's count plus one
when that stamp was valid. Otherwise, after a power loss between an erase
and its stamp, or on a chip alfs never formatted, it is the largest count
known in the region, or 1 if none is. A large erase stamps each of its
sectors with one more than the largest valid count among them.

The **next sequence** at mount is two more than the largest of every valid
claim's sequence and every valid stamp's floor. A power loss can tear a
claim so that it reads invalid now and valid later; it was given the
sequence after the largest valid one, and the floors keep that largest
known after its sector is erased, so the torn claim's sequence is never
given again.

### 1.2 The dead mark

Erased: the stream whose first sector this is lives. Any other value: it
was deleted. alfs writes ~E, in one program, so a delete is atomic. Only a
stream's first sector's mark is read.

### 1.3 The claim

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | stream identity: the sequence of the stream's first claim |
| 4 | 4 | sequence: unique in the region, growing, never reused |
| 8 | 4 | tag: the caller's |
| 12 | 2 | the stream's first sector, an index into the region |
| 14 | 2 | sector number: 0 in the stream's first sector, one more in each next |
| 16 | 4 | CRC of bytes 0 to 15 |

A sector is a stream's **first** when its claim's sequence equals its
identity and its sector number is 0.

### 1.4 A chunk

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | length of the payload, 1 or more |
| 2 | 4 | CRC of the two length bytes and the payload |
| 6 | length | payload |

A chunk never crosses a page. The first chunk of a sector starts at D,
straight after the claim; each later chunk at RU of the end of the one
before, or at the start of a later page.

---

## 2. What a sector is

Decided from at most 60 bytes: the stamp, the dead mark and the claim, and
for a sector that is not its stream's first, the first sector's dead mark
and claim.

| State | When |
|---|---|
| **ready** | the stamp matches, has the current generation, and the dead mark and the claim are erased |
| **live** | the stamp matches, has the current generation, the claim is valid, and the stream's first sector has a valid first claim with the same identity and an erased dead mark |
| **dirty** | anything else: no valid stamp, an old generation, a claim neither erased nor valid (**torn**), a deleted or vanished stream, or anything not alfs's |
| **bad** | an erase or the check after it failed, during this mount; kept in RAM, at most eight |

A bit error in a claim makes its sector dirty, and in a stream's first
sector makes the whole stream dirty: the next erase steps reclaim it.
Chunks are protected one by one (section 3); claims are not repaired.

Before a stream takes a ready sector, the rest of the claim's page, from
the dead mark on, is read and must be erased; otherwise the sector is
counted dirty and the next ready one is tried.

---

## 3. Reading

**A sector's chunks**, from D, at position x:

1. Read the six header bytes at x.
2. If they are erased: when x is a page start and the whole page is
   erased, the sector's chunks end here. Otherwise go to the next page
   start and continue.
3. If the length is 0, or the chunk would cross its page, or its CRC fails:
   count one failed check (when reading for the caller), go to the next
   page start, and continue. The length cannot be trusted, so nothing else
   in that page is either.
4. Otherwise the chunk is good: deliver it, and continue at RU(x + 6 +
   length).

**A stream's sectors**, in order: its first sector, then repeatedly the
live sector whose claim has the same identity and the next sector number.
It is found by scanning forward, circularly, from the sector just read,
and the scan stops there: allocation moves forward, so it is usually a few
sectors on. When no sector has the next number, the one with the smallest
larger number follows, and the gap counts one failed check; when none has
a larger number, the stream ends.

---

## 4. Writing

- **A new sector:** the ready sector found by scanning forward, circularly,
  from the sector after the last claimed. One program writes the claim and
  the first chunk. Its sector number is one more than the stream's last.
- **The next chunk:** at RU of the last one's end, in the same page while
  it fits and the page has had fewer than `max_page_programs` programs;
  otherwise at the next page's start; past the sector's end, in a new
  sector. In a stream's first sector, one program of the dead mark's page
  is held back for the dead mark.
- **A failed program:** the stream leaves the sector and writes the same
  bytes to a new sector on its next call. A sector whose claim program
  failed is counted dirty.

**After a mount** (`alfs_open_append`), with U = 1. The power loss that
preceded the mount interrupted at most one program: the next chunk in the
page of the stream's last good chunk, the first chunk of the next page, or
the claim of a new sector. Its bits may read either way (littlefs's
METASTABLE, seen on the XT25F128F by the probe), but it cleared only bits
its data cleared (A-9). So:

1. The stream's first program is its last sector's claim again, with the
   bytes it reads. That completes the claim if the interrupted program was
   it.
2. Its next chunk starts at the start of the second page after the page of
   the last good chunk in its last sector (after the claim's page, if no
   chunk there is good), or in a new sector when there is no such page.
   Neither page that the interrupted program could have touched takes new
   data.
3. Before its first new sector, it scans every sector and programs zero
   over any claim that reads valid with its identity and a sector number
   past its last, and any torn claim whose identity, tag, first sector and
   sector number fields read erased in every bit where its own identity,
   tag, first sector and next number are erased (a torn program leaves a
   bit erased only where its data cleared it, A-9). One such claim a call. A torn claim of the
   sector it was moving into, which might otherwise read valid at a later
   mount, cannot then stand beside the sector that replaces it.

With U over 1 the chip cannot take a second program, so the stream goes on
in a new sector, as in version 1.

---

## 5. Erasing

The erase step scans forward, circularly, from where it last stopped, for
a dirty sector that is not bad. With a driver that has `erase_large` and a
whole aligned block of dirty sectors, it takes the block.

With U = 1, before erasing, it looks at each sector to be erased: one that
belongs to a deleted stream, and is not that stream's first, has the
stream's dead mark programmed with ~E again, one mark a call, unless that
mark was the last one programmed this mount. A dead mark a power loss left
half programmed then cannot read erased once some of its stream's sectors
are gone.

It erases the sector or block, reads every byte back as erased, programs
the stamp and reads the stamp back. Any failure makes the sector bad for
the rest of the mount. The stamp, not the blank check, is what shows an
erase completed: the probe found erases aborted at a tenth of their time
already reading blank (`docs/FLASH_PROBE.md` section 4).
