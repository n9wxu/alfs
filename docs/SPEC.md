# alfs on-disk format, version 4

Every byte alfs writes, and how it reads them back. Little-endian
throughout; every check is CRC-32 (reflected, polynomial 0xEDB88320,
initial value and final XOR 0xFFFFFFFF: the CRC of zlib and Ethernet). S
is the sector size and P the page size, both from the configuration.
Offsets are from the start of a sector, or of a half. The erase value is
0xFF; "erased" means every byte reads 0xFF.

alfs has two areas, each configured on its own: the log area (sections 1
to 5) and the object area (sections 6 to 10). Neither reads or writes the
other.

---

## 1. A log sector

| Offset | Size | Field | Written |
|---|---|---|---|
| 0 | 18 | stamp | by an erase, after the erase and a check that every byte is erased |
| 18 | 12 | claim | by the log that takes the sector, in one program with the stamp again and the sector's first chunk |
| D = 30 | | chunks | by writes and syncs |

P must be at least 128 bytes and S at least four pages (`ALFS_EGEOMETRY`
otherwise).

### 1.1 The stamp

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic, "alfl" |
| 4 | 1 | version, 4 |
| 5 | 1 | log2 S |
| 6 | 1 | log2 P |
| 7 | 1 | 0 |
| 8 | 2 | sectors in the area |
| 10 | 4 | sequence floor: the next sequence when the stamp was written |
| 14 | 4 | CRC of bytes 0 to 13 |

A stamp is **valid** when its magic, version and CRC are right. It
**matches** when bytes 5 to 9 equal the configuration. Only a matching
stamp counts; a sector whose stamp does not match is dirty.

The **next sequence** at mount is two more than the largest of every valid
claim's sequence and every matching stamp's floor, and three more when the
mount finds a torn claim (section 2).
- **Why:** a power loss can tear a claim so that it reads invalid at one
  mount and valid at a later one. Its sequence must never be given again.
- **The bound:** a torn claim's sequence is at most two more than the
  largest known. Either it is the first sequence its mount gave, two more
  than the largest then, or it follows a claim that is still valid. The
  floors keep the largest sequence known after its sector is erased.
- **So** three more than the largest known is past any sequence a torn
  claim holds. Two more is not: a claim torn straight after a mount holds
  exactly two more than the largest that mount knew, which is also the
  largest the next mount knows.

### 1.2 The claim

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | log number |
| 4 | 4 | sequence: unique in the area, growing, never reused |
| 8 | 4 | CRC of bytes 0 to 7 |

A claim is **valid** when it is not erased and its CRC is right, and
**zero** when all 12 bytes are 00h.

A log's number is the sequence alfs gave when the log started. Its
sectors' sequences are greater, and grow in the order the log took them.

### 1.3 A chunk

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | length of the payload, 1 or more |
| 2 | 1 | tag: the stream |
| 3 | 4 | CRC of the length, the tag and the payload |
| 7 | length | payload |

A chunk never crosses a page. A sector's first chunk starts at D; each
later chunk at the end of the one before, or at the start of a later page.

---

## 2. What a log sector is

| State | Stamp | Claim |
|---|---|---|
| ready | matches | erased |
| used: a sector of the log its claim names | matches | valid |
| dirty | anything else | |

A sector whose stamp matches and whose claim is neither erased, valid nor
zero holds a **torn claim**.

---

## 3. Reading a log

A log's sectors are the used sectors whose claim names it, in the order of
their sequences.
- **The first** is the one with the least sequence.
- **The next** after sector k is found forward from k, circularly: the
  first sector of the log with a greater sequence. Allocation goes forward
  (section 4), so it is the next sector of the log forward.

Within a sector, from D, each place holds a chunk header, or is the end:
- **Fewer than 8 bytes to the page's end:** no chunk fits; the next page.
- **Erased header:** the rest of the page holds nothing; the next page.
- **A length of 0, or one past the page's end:** the header is damaged.
  One failed check is counted, and reading goes on at the next page.
- **Another stream's chunk:** skipped by its length, unchecked.
- **The stream's own chunk:** its CRC is checked. A right one gives its
  payload. A wrong one counts one failed check, and reading goes on at
  the next page.

The sector ends at its last page.

---

## 4. Writing a log

**Starting a log** gives it the next sequence as its number. It takes no
sector until its first chunk.

**Allocation** scans forward, circularly, from the sector after the one
holding the newest claim the mount found, or after the last sector taken.
It takes the first sector that:
- is ready, and not bad this mount;
- reads erased from 18 to the end of its first page;
- is not the **fresh sector**: after a mount, the first sector after the
  sector of the newest valid claim whose stamp matches and whose claim is
  neither valid nor zero, or the first such sector anywhere if no claim is
  valid. A claim program torn at its start can leave a sector that reads
  ready. When allocation reaches the fresh sector, alfs programs 00h over
  its claim, which makes it dirty.

A ready sector that fails the second check is dirty: something touched it
since its erase.

**A sector's first program** starts at byte 0: the stamp again with the
bytes it reads (A-9), the claim with the next sequence, and the first
chunk.

**Chunks** fill pages. A writer's block is programmed as chunks, each as
long as the room left in the current page allows. Two writers of one log
add their chunks at the same place, one after the other.

**A program that fails** (an error, or a read-back that differs) is
counted, and its payload is dropped and counted as lost. The next chunk
takes a new sector. A sector whose first program failed is dirty.

---

## 5. Erasing the log area

Erasing happens only when the application asks, one sector a step. A step
scans forward, circularly, from where the last one stopped, for a sector
that is not bad and is dirty, or, when a log is named, is that log's. It
decides from one read of the sector's header.

The step erases the sector, reads every byte back as erased, programs the
stamp with the next sequence as its floor, and reads the stamp back. Any
failure makes the sector bad for the rest of the mount.

The stamp, not the blank check, is what shows an erase completed: an erase
aborted at a tenth of its time can already read blank
(`docs/FLASH_PROBE.md` section 4).

---

## 6. The object area

The object area is two halves of H sectors each: half 0 its first H
sectors, half 1 the rest. A half's first page holds its records; its
objects start at its second page.

| Offset | Size | Field | Written |
|---|---|---|---|
| 0 | 16 | stamp | after every sector of the half is erased and checked erased |
| 16 | 1 | begun mark | 00h, by the put or delete that begins writing the half, in one program with the stamp again |
| 17 | 12 | commit | by the put or delete that finishes, after its last object |
| P | | objects | by the put or delete |

### 6.1 The stamp

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic, "alfo" |
| 4 | 1 | version, 4 |
| 5 | 1 | log2 S |
| 6 | 1 | log2 P |
| 7 | 1 | 0 |
| 8 | 2 | H, sectors in a half |
| 10 | 2 | 0 |
| 12 | 4 | CRC of bytes 0 to 11 |

Valid and matching as a log sector's stamp (section 1.1), with bytes 5
to 11.

### 6.2 The commit

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | sequence: one more than the commit before it |
| 4 | 4 | end: the offset, from the half's start, of the byte after its last object |
| 8 | 4 | CRC of bytes 0 to 7 |

A commit is **valid** when it is not erased, its CRC is right, and P ≤ end
≤ H × S.

### 6.3 An object

| Offset | Size | Field |
|---|---|---|
| 0 | 32 | name: 1 to 31 bytes, none 00h, then 00h to fill |
| 32 | 4 | size of the payload |
| 36 | size | payload |
| 36 + size | 4 | CRC of the name, the size and the payload |

The first object starts at P; each later one at the end of the one before.
The last ends at the commit's end.

---

## 7. What a half is

| State | Stamp | Begun mark | Commit |
|---|---|---|---|
| committed | matches | | valid |
| ready | matches | erased | erased |
| dirty | anything else | | |

The **active** half is the committed half with the greater sequence, half
0 when both have the same. The other is the **spare**. With no committed
half, the area holds no store (`ALFS_ENOFS`).

---

## 8. Putting and deleting

A put of object N, of a given size:

1. **The spare is made ready,** if it is not. First the active half's
   commit is programmed again, built from the sequence and end it holds
   (A-9). Then each of the spare's sectors is erased and read back as
   erased, and the spare's stamp programmed.
2. **One program from the spare's byte 0:** the stamp again, built from the
   configuration, and the begun mark.
3. **Every object of the active half not called N** is copied, in order,
   in programs that do not cross a page.
4. **Object N:** its name and size; its payload, as the application writes
   it; its CRC.
5. **The commit:** the active half's sequence plus one, and the end.

The spare is then the active half, and the half that was active is the
spare, dirty: step 1 of the next put erases it.

A delete of N is a put without step 4. A put that is abandoned leaves the
spare dirty.

Step 1's program means a commit a power loss left half programmed cannot
read invalid once the half before it is gone. Step 2's means a stamp a
power loss left half programmed cannot decay under the objects.

---

## 9. Getting

An object is found in the active half by walking from P: each object's
size gives where the next begins. Opening it reads it whole and checks its
CRC; reading it then reads its payload. The half it is in is not erased
while it is open.

---

## 10. Formatting the object area

Both halves are erased, read back as erased and stamped. Half 0 then takes
its begun mark and a commit of sequence 1 and end P: a store with no
objects.
