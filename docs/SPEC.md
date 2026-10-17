# alfs on-disk format, version 6

Every byte alfs writes, and how it reads them back. Little-endian
throughout; every check is CRC-32 (reflected, polynomial 0xEDB88320,
initial value and final XOR 0xFFFFFFFF: the CRC of zlib and Ethernet). S
is the sector size and P the page size, both from the configuration. The
erase value is 0xFF; "erased" means every byte reads 0xFF.

Pages are numbered from the area's start: page n lies at byte n × P of the
area. That number is a page's **address**. The area's first two sectors are
the catalog; every page after them is a frame page.

---

## 1. The catalog

Two sectors, catalog sectors 0 and 1. One is the catalog; the other is its
spare.

| Offset | Size | Field | Written |
|---|---|---|---|
| 0 | 20 | stamp | after the sector is erased and checked erased |
| 20 | 1 | complete mark | 00h, after every entry the sector starts with |
| 24 | | entries | one at each allocation |

### 1.1 The stamp

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic, "alfc" |
| 4 | 1 | version, 6 |
| 5 | 1 | log2 S |
| 6 | 1 | log2 P |
| 7 | 1 | 0 |
| 8 | 2 | sectors in the area |
| 10 | 2 | 0 |
| 12 | 4 | sequence: one more than the catalog it replaced |
| 16 | 4 | CRC of bytes 0 to 15 |

A sector is **the catalog** when its stamp's CRC is right, bytes 4 to 9
match the configuration, and its complete mark is 00h; of two, the one with
the greater sequence.

### 1.2 An entry

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | frame ID |
| 4 | 4 | first page: the address of the frame's first page |
| 8 | 32 | filename: 1 to 31 bytes, none 00h, then 00h to fill |
| 40 | 4 | CRC of bytes 0 to 39 |

An entry never crosses a page: a page holds five, from byte 24 on page 0
and from byte 0 on the others, so a sector of sixteen pages holds 80. An
entry is **valid** when its CRC is right, and **deleted** when its
filename is all 00h. A filename names the frame of its valid entry.

### 1.3 Rewriting

When no slot is left, the spare is erased, checked erased and stamped with
the next sequence; every valid entry whose frame's first page names it
(section 3) is copied in, in order; then its complete mark is programmed,
which makes it the catalog. The other sector is left as it is, until the
next rewrite erases it.

### 1.4 Frame IDs

At mount the next frame ID is two more than the largest frame ID in an
entry of either catalog sector whose CRC is right, and three more when an
entry reads neither erased nor valid. An entry is programmed in the first
slot after the last valid entry, unless that slot reads other than erased,
or was the first slot after the last valid entry at the mount: then in the
slot after it.

---

## 2. A frame page

| Offset | Size | Field | Written |
|---|---|---|---|
| 0 | 4 | frame ID | at allocation, or when a write takes the page past the reservation |
| 4 | 2 | page count: the page's position in its frame, from 0 | as the frame ID |
| 6 | 4 | header check: CRC of bytes 0 to 5 | as the frame ID |
| 10 | 4 | next page: FFFFFFFFh none yet, 0 the frame's last page, else an address | at allocation; by a write past the reservation; 0 by a close |
| 14 | | the frame block, on a frame's first page | section 2.1 |
| H | n | data | by a write, whole pages; by a close, the last |
| H + n | 4 | data check: CRC of the frame ID, the page count and the data | in the data's program |

H is 14, or 64 on a frame's first page. A page holds D = P − 18 bytes of
data, or P − 68 on a frame's first page: 238 and 188 on 256-byte pages.
Every page of a frame holds D bytes of data but the last page written,
which holds the rest.

A page's header is **valid** when its header check is right. A page is
**free** when it reads erased, every byte.

### 2.1 The frame block

| Offset | Size | Field | Written |
|---|---|---|---|
| 0 | 4 | frame ID | at allocation |
| 4 | 32 | filename: as an entry's | at allocation; 00h by a delete |
| 36 | 2 | page count: pages holding data | by a close |
| 38 | 4 | size: bytes of data | by a close |
| 42 | 4 | frame checksum: CRC of the data, in order | by a close |
| 46 | 4 | close check: CRC of bytes 0 to 45 | by a close |

The reserved size is not recorded: the list is the reservation, and its end
is the page whose link is erased. The first 36 bytes are programmed with
the page's header, in one program.
The last 14, the **close fields**, are programmed together. A frame is
**closed** when its close check is right.

---

## 3. What a file is

A filename names a file when a valid entry of the catalog holds it, and the
page at the entry's first page has a valid header with the entry's frame
ID and page count 0, and a frame block with the same frame ID and
filename.

A file's **list** starts at its first page. From each page, the next is the
one its link names, if that page lies in the area past the catalog, has a
valid header, the same frame ID and the next page count, and has not
already been passed. Otherwise the list ends at the page.

A page is **live** when it is in the list of a file: up to the page count
if the file is closed, to the list's end if not. A page in no file's list
that is not free is **dead**.

---

## 4. Allocating

An allocation of a filename and a size takes R pages, enough for the size
in the frame's data (section 2), at least one:

1. **Free pages** are found forward, circularly, from where the last search
   stopped. If fewer than R are found in a lap, sectors holding no live page
   that do not read erased are erased and checked erased, forward from the
   same place, until enough pages are free.
2. **The entry** is programmed in the catalog, with the next frame ID and
   the first page's address.
3. **The pages**, from the last to the first: each one's header, with its
   page count and the next page's address, the last page's link left
   erased; the first page's frame block with it.

---

## 5. Writing

A writer's block holds the data not yet programmed. Each complete page of
it, when the block is full, at a sync, or at a close, is programmed into
the next page of the list, with its data check, in one program.

**Without `ALFS_GROW`** a write takes only the bytes the pages left in the
list hold, counted at open from the links.

**Past the reservation**, a writer opened with `ALFS_GROW` takes the next
page from the free pages, forward from where the last search stopped: its
header is programmed, then the link to it in the page before. Without the
flag, or with no free page, the write returns `ALFS_ENOSPC`. Nothing is
erased.

**A close** programs the block's rest into the next page; then the close
fields; then 00h into the last page's link; then erases each sector of the
list past the last page that holds no live page.

**Reopening** a file not closed starts at the first page of its list whose
data and data check read erased, every byte. The size and checksum so far
are taken from the pages before it whose data check is right.

**Reopening a closed file**, through a block of at least S bytes, rewrites
the sector of its first page and the sector of its last page: each in turn
is read into the block, erased, checked erased, and programmed again, page
by page, as it was, except the first page's close fields, the last page's
link, and the last page's data and data check, which are left erased. The
last page's data goes into the writer's block, and writing goes on in that
page.

---

## 6. Reading

A file is read along its list. Each page's data is read with its data
check; a page whose check fails gives nothing, and one failure is counted.
A closed file ends at its page count, its last page's data being the size
less the data before it; the read that reaches the end checks the frame
checksum, and returns `ALFS_ECHECK` when it fails. A file not closed ends
at the first page whose data and data check read erased, or at the list's
end.

---

## 7. Deleting

A delete programs 00h over the filename in the frame block, then over the
filename in the entry. The pages are left as they are; they are dead once
the file is gone, and a cleanup erases the sectors that hold no live page.

---

## 8. Cleaning up

A cleanup step finds, forward from where the last one stopped, a sector
that holds no live page and does not read erased, erases it and checks it
erased. With none left, `ALFS_CLEAN`.

---

## 9. Formatting

Both catalog sectors are erased and checked erased; catalog sector 0 is
stamped with sequence 1 and given its complete mark. Every frame page is
then in no file.
