# alfs on-disk format, version 6

Every byte alfs writes, and how it reads them back. Little-endian
throughout; every check is CRC-32 (reflected, polynomial 0xEDB88320,
initial value and final XOR 0xFFFFFFFF: the CRC of zlib and Ethernet). S
is the sector size and P the page size, both from the configuration. The
erase value is 0xFF; "erased" means every byte reads 0xFF.

Pages are numbered from the area's start: page n lies at byte n × P of the
area. That number is a page's **address**. Page 0 holds no file: its
address marks a file's last page. Every other page can belong to a file.
Nothing else is on the flash: no catalog, no stamp. A mount finds the files
by reading every page's header.

---

## 1. A page

| Offset | Size | Field | Written |
|---|---|---|---|
| 0 | 4 | frame ID | at allocation, or when a write takes the page past the reservation |
| 4 | 2 | page count: the page's position in its file, from 0 | as the frame ID |
| 6 | 4 | header check: CRC of bytes 0 to 5 | as the frame ID |
| 10 | 4 | next page: FFFFFFFFh none yet, 0 the file's last page, else an address | at allocation; by a write past the reservation; 0 by a close |
| 14 | | the frame block, on a file's first page | section 1.1 |
| H | 2 | byte count: n | with the data, in one program |
| H + 2 | n | data | with the byte count |
| H + 2 + n | 4 | data check: CRC of the frame ID, the page count, the byte count and the data | with the byte count |

H is 14, or 65 on a file's first page. A page holds at most D = P − 20
bytes of data, or P − 71 on a file's first page: 236 and 185 on 256-byte
pages. A page holds D bytes when a write or a sync programs it, and fewer
when a flush or a close leaves it partial; its data is programmed once.

The byte count reads:
- **FFFFh:** the page holds no data yet.
- **0:** the page was skipped after a reset (section 4); it holds no data,
  and a reader goes on to the next page.
- **1 to D:** the page holds that much data.

A page's header is **valid** when its header check is right. A page is
**free** when it reads erased, every byte. Before an allocation or a
growth takes a page, its first 65 bytes, all an allocation programs, must
read erased on 16 reads: a header a power loss cut early can read erased
but for a bit that reads either way.

No program covers a byte that must stay erased: a reservation's last page
is programmed without its link.

### 1.1 The frame block

| Offset | Size | Field | Written |
|---|---|---|---|
| 0 | 4 | frame ID | at allocation |
| 4 | 32 | filename: 1 to 31 bytes, none 00h, then 00h to fill | at allocation; 00h when deleted or replaced |
| 36 | 1 | version | at allocation |
| 37 | 4 | open check: CRC of bytes 0 to 36 | at allocation |
| 41 | 2 | page count: the pages of the file | by a close |
| 43 | 4 | size: the byte counts' sum | by a close |
| 47 | 4 | close check: CRC of bytes 0 to 46 | by a close |

The first 41 bytes are programmed together, with the page's link, before
the page's header (section 4). The last 10, the **close fields**, are
programmed together. A file is **closed** when its close check is right; a
closed file is final.

The reserved size is not recorded: the list is the reservation, and its end
is the page whose link is erased.

---

## 2. What a file is

A **copy** of a file is a page with a valid header and page count 0 whose
frame block has the same frame ID, a right open check, and a filename.

**Versions.** Two copies may share a filename while an update is under
way. Copy A is newer than copy B when the version of A less the version of
B, taken as a signed 8-bit number, is above 0. A filename names its newest
closed copy, or, with none closed, its newest copy.

A copy's **list** starts at its first page. From each page, the next is the
one its link names, if that page lies in the area past page 0, has a valid
header, the same frame ID and the next page count, and has not already been
passed. Otherwise the list ends at the page.

A page is **live** when it is in the list of a copy a filename names, or of
a copy newer than it that is not closed: up to the page count if the copy
is closed, to the list's end if not. A page in no such list that is not
free is **dead**.

---

## 3. Mounting

A mount reads every page's header, and the frame block of every page that
holds one. It builds, in a table the caller gives, a record of each copy
that is live: its frame ID, first page, version, page count, whether it is
closed, and a hash of its filename. It writes nothing.

The next frame ID is two more than the largest frame ID in a valid header,
and three more when a header reads neither erased nor valid.

---

## 4. Allocating and writing

An allocation of a filename and a size takes R free pages, enough for the
size in the file's data (section 1), at least one, found forward,
circularly, from where the last search stopped. With fewer than R free, it
writes nothing. The pages are programmed from the last to the first: each
one's header, with its page count and the next page's address, the last
page's link left erased. On the first page, the frame block, with the next
frame ID and a version one more than the filename's newest copy, and the
link are programmed first, and the header last: a copy exists only once
every page of it is on the flash. A copy of the filename that is not closed
is then blocked out (section 6).

A writer's block holds the data not yet programmed. Each complete page of
it, when the block is full or at a sync, is programmed into the next page of
the list, with its byte count and data check, in one program. A **flush**
programs the rest too, as a partial page, and the file goes on in the next
page.

**Without `ALFS_GROW`** a write takes only the bytes the pages left in the
list hold, counted at open from the links.

**Past the reservation**, a writer opened with `ALFS_GROW` takes the next
page from the free pages, forward from where the last search stopped: its
header is programmed, then the link to it in the page before. Without the
flag, or with no free page, the write returns `ALFS_ENOSPC`.

**A close** programs the block's rest into the next page, partial; then
the close fields; then 00h into the last page's link; then blocks out every
older copy of the filename (section 6).

**Opening** a copy not closed for writing finds the page after the last
page of its list holding data; a page whose program failed reads erased,
and may come before it. After a mount, that page is skipped: 0000h is
programmed into its byte count, and writing goes on in the next page. The
size so far is the sum of the byte counts that are at most D.

---

## 5. Reading

A file is read along its list. Each page's byte count, data and data check
are read together. A byte count of 0 gives nothing. A byte count over D, or
a check that fails, gives nothing, and one failure is counted; so does an
erased byte count in a closed copy holding data. A closed copy ends at its
page count; the read that reaches the end returns `ALFS_ECHECK` if a page
gave nothing for a failure, or the list ended before the page count. A
copy not closed ends at a page whose byte count reads erased, unless a
later page of the same sector, or the first page after it, holds data; or
at the list's end.

---

## 6. Deleting and replacing

A copy is blocked out by programming 00h over its filename: its open check
then fails, as it does for a filename a power loss left zeroed in part. A
delete blocks out every copy of the filename. The pages are left as they are; they are
dead once no live copy holds them, and a cleanup erases their sectors.

---

## 7. Cleaning up

The cleanup is the only thing that erases. A step finds, forward from where
the last one stopped, a sector that holds no live page and does not read
erased, erases it, and reads it back: the erase is finished when every
byte reads FFh. With none left, `ALFS_CLEAN`.
