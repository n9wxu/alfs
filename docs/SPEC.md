# alfs on-disk format, version 1

Every byte alfs writes, and how it reads them back. Little-endian
throughout; every check is CRC-32 (reflected, polynomial 0xEDB88320,
initial value and final XOR 0xFFFFFFFF: the CRC of zlib and Ethernet).

Terms are those of `docs/REQUIREMENTS.md` section 4.1. S is the sector
size, P the page size, U the prog unit, and RU(x) is x rounded up to a
multiple of U. "Erased" means every byte equals the configured erase value.

---

## 1. A sector

| Offset | Size | Field | Written |
|---|---|---|---|
| 0 | 22 | stamp | by the erase step, after the erase and a check that every byte is erased |
| DM = RU(22) | 1 | dead mark | by `alfs_delete`, in a stream's first sector only |
| C = DM + RU(1) | 18 | claim | by the stream that takes the sector, in one program with its first chunk |
| D = C + 18 | | chunks | by appends and syncs |

The claim and its first chunk share one program, so they share a page:
P must be at least 64 bytes (`ALFS_EGEOMETRY` otherwise), and a sector at
least four pages.

| U | DM | C | D |
|---|---|---|---|
| 1 | 22 | 23 | 41 |
| 16 | 32 | 48 | 66 |
| 256 | 256 | 512 | 530 |

### 1.1 The stamp

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic, "alfs" |
| 4 | 1 | version, 1 |
| 5 | 1 | log2 S |
| 6 | 1 | log2 P |
| 7 | 1 | log2 U |
| 8 | 2 | sectors in the region |
| 10 | 4 | generation: the format that wrote it |
| 14 | 4 | erase count: this sector's erases, this one included |
| 18 | 4 | CRC of bytes 0 to 17 |

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

### 1.2 The dead mark

Erased: the stream whose first sector this is lives. Any other value: it
was deleted. One program, so a delete is atomic. Only a stream's first
sector's mark is read.

### 1.3 The claim

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | stream identity: the sequence of the stream's first claim |
| 4 | 4 | sequence: unique in the region, growing, never reused |
| 8 | 4 | tag: the caller's |
| 12 | 2 | the stream's first sector, an index into the region |
| 14 | 4 | CRC of bytes 0 to 13 |

A sector is a stream's **first** when its claim's sequence equals its
identity. Sequences continue across formats: a mount takes one more than
the largest sequence in any valid claim, so identities never repeat.

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
| **live** | the stamp matches, has the current generation, the claim is valid, and the stream's first sector has a valid claim with the same identity, a sequence equal to it, and an erased dead mark |
| **dirty** | anything else: no valid stamp, an old generation, a claim neither erased nor valid, a deleted or vanished stream, or anything not alfs's |
| **bad** | an erase or the check after it failed, during this mount; kept in RAM, at most eight |

A bit error in a claim makes its sector dirty, and in a stream's first
sector makes the whole stream dirty: the next erase steps reclaim it.
Chunks are protected one by one (section 3); claims are not repaired.

Before a stream takes a ready sector, the rest of the claim's page, from
the dead mark on, is read and must be erased; otherwise the sector is
counted dirty and the next ready one is tried.

---

## 3. Reading a sector's chunks

From D, at position x:

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

A stream's sectors, in order: its first sector, then repeatedly the sector
whose claim has the same identity and the smallest sequence greater than
the last.

---

## 4. Writing

- **A new sector:** the ready sector found by scanning forward, circularly,
  from the sector after the last claimed. One program writes the claim and
  the first chunk.
- **The next chunk:** at RU of the last one's end, in the same page while
  it fits and the page has had fewer than `max_page_programs` programs;
  otherwise at the next page's start; past the sector's end, in a new
  sector. In a stream's first sector, one program of the dead mark's page
  is held back for the dead mark.
- **After a mount** (`alfs_open_append`): the next chunk goes in a new
  sector. The power loss that preceded the mount may have left bits that
  read either way (littlefs's METASTABLE) in the last sector's claim or
  chunks. Only synced bytes were promised, and a new sector keeps the bytes
  synced from then on clear of them. It costs the rest of one sector a
  mount.
- **A failed program:** the stream leaves the sector and writes the same
  bytes to a new sector on its next call. A sector whose claim program
  failed is counted dirty.

---

## 5. Erasing

The erase step scans forward, circularly, from where it last stopped, for
a dirty sector that is not bad. It erases it (or, with a driver that has
`erase_large` and a whole aligned block of dirty sectors, the block),
reads every byte back as erased, programs the stamp and reads the stamp
back. Any failure makes the sector bad for the rest of the mount.
