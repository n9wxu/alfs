# alfs requirements

alfs, the append-log flash store: files on serial NOR flash. Each file is a
frame of pages linked in a list, found by its filename through a catalog.
A file's pages can be reserved ahead of time, so that writing it in flight
programs only flash already erased.

**Status: draft 6, requirements and tests.** Requirements come first, then
the tests that verify them (`docs/TEST_PLAN.md`), then the code. There is
no code yet. `experiments/v6-frames/EVALUATION.md` evaluates this option.

---

## 1. Purpose and priorities

The brief, from the user:

> "Clean sheet a focused flash filesystem optimized for append logging and
> preserving the flash memory. ... Use the pyro lessons as use-cases. Use
> the flash behavior data from littlefs. Requirements then tests then code.
> Prefer speed then long flash life then safety. A corrupted data block is
> less important than a broken exec loop. Simplicity will keep the code
> fast and flash overhead cheap."

The founding rules, the user's rulings for the pyro flight computer:

> "Fast writes require pre-erase. Before flight, we must ensure all unused
> flash is erased."
>
> "writing to a dirty sector is not allowed, we must only write to erased
> sectors. It does not matter if the write is metadata or document data.
> Erase before write adds significant delay to the append operations."

The user, on files:

> "Since the UI is simply GET and PUT of whole files, we are effectively
> making an object store."
>
> "LUA programs should still be a full file write."
>
> On writes: "Write will block until the write is covered. ... Applications
> bring their ram ... A write will bring data and a ram block. The write
> will be placed in the ram block by alfs. If the ram block is full, it
> will be sent to the flash ops. When flash ops are over, the remaining
> data will be put in the ram block. If all data is consumed, the writer
> will return to the application."

The user, on this design:

> "Do not synchronize the rows with the flash pages. Accept some memory,
> Block the memory into pages, sync the complete pages, and wait for more
> data. There shall be 2 layers of data blocking. First is a "frame block"
> that encapsulates the entire memory from the application. The frame
> block includes a frame ID, frame checksum, page count and data. Pages
> are allocated to frames in whole pages. A whole page includes the frame
> ID, page cont, reserved page count and the "next page" address. This
> makes a linked list of pages. A catalog sector contains a list of frame
> id's, frame first page address and frame name. Frames can be considered
> files. Files can be pre-allocated with a file_alloc command. The
> file_alloc will accept the file name, and reserved file size. Files that
> have a reserved size but not a final size shall have the page count FF
> but the full linked list of pages erased and created. When the file is
> eventually opened, written and closed, the close will update the
> page-count value mark the last page address link with 0's, and erase all
> the unused pages. If the file grows beyond the reservation, a best effort
> will be made to continue page allocations if there are erased pages
> available. No new pages will be erased. A filesystem "cleanup" operation
> is available to pre-erase all unused pages sector by sector. Compacting
> sectors to maximize the erased space is a future option."
>
> "the first frame should also have a filename. Files are
> opened/closed/allocd by filename. The filename converts to a frame id
> which is essentially the file descripter."
>
> "If the page looks erased after a reboot, it can only be treated as
> erased. The preceeding page will reference the erased page with no data.
> If the page had been alloced, it will have a page number and a next page
> address but no data and no CRC. If there is partial data, then it can be
> left alone and the next frame in the allocated sequence get the next
> data. OR we can erase, rewrite and continue on this frame. But erase
> requires a sector or a block and not simply a page."
>
> "Erasing a file is simply writing 0's over the filename. The rest of the
> data can be left alone unless we need to scrub the file system. A simple
> LUA update will block out the filename and write a new file."
>
> "I don't think having the frame reservation size stored helps. The
> linked list is assembled and that is sufficient. We will discover if we
> are out of space by the last frame allocated missing the next frame
> address. We can add a flag that will allow writes to continue if space
> is available, or fail if no space is reserved."
>
> "A partial write to a page cannot update the CRC so only full pages can
> be written. A page byte count can be added to the page format so that a
> force flush can leave a partial page and the following page will
> continue the file. Erases are finshed when a read returns 1's for all
> pages. ... A file is valid if its pages are valid. maintining a full file
> checksum is not possible without many rewrites of the checksum page."
>
> "But this usecase is not a general purpose filesystem. I expect all
> files to be either log files (open, close, append, read, delete) or
> configuration (open, close, read whole file, write whole file). Lua
> programs count as configuration. Edits can happen on a host computer and
> not on this. the file system uses REST so it already works on whole
> files. We should not support PATCH."

The user's choices in the review of the formats: a page's "page count" is
its position in its frame; each page's data carries a check, and so does
each page's header; the frame records its size in bytes; the fields a close
writes carry a check.

**Two kinds of file.**
- **A log** is allocated, opened, appended to, closed, read and deleted.
  One cut short by a reset, never closed, is opened again and appended to.
- **A configuration file**, Lua programs among them, is written whole and
  read whole. Writing it again deletes the old file and writes a new one.

A closed file is final: alfs edits no file.

**The two layers.**
- **The frame** is a file: what the application writes to it, end to end.
  Its first page holds the frame block: the frame ID, the filename, and,
  once it is closed, its page count and size.
- **The page** is the unit of allocation and of programming. Each page of a
  frame holds the frame ID, its position in the frame, and the address of
  the next page, which makes the frame's pages a linked list. A page's data
  is programmed once, with its byte count and its check: a full page, or a
  partial one that a flush or a close leaves, after which the file goes on
  in the next page.
- **The catalog** maps each filename to its frame ID and first page.

**The order of priority.** Every requirement carries one:

1. **Speed (SPD):** no flash operation of a file write stalls the system
   for longer than one page program, and no write waits for an erase.
2. **Flash life (LIFE):** every byte programmed and every sector erased is
   one the files needed.
3. **Safety (SAFE):** what was synced or closed survives power loss and
   faults, as far as 1 and 2 allow.

Where two requirements conflict, the higher priority wins. **Simplicity
(SIMP)** is how the three are met; its requirements are budgets.

---

## 2. Scope

**In scope:**

- **Files** on raw serial NOR flash, found by filename: allocate with a
  reserved size, open, write, sync, flush, close, read, delete, list.
- **Reopening** a log that a reset cut short, never closed.
- **Growth** past a reservation into pages already erased.
- **Cleanup:** erasing, sector by sector, the sectors that hold no page of
  a file.
- Power loss at any moment; the bad-sector and power-loss behaviours of
  section 4.4.
- A chip shared with executing code: alfs owns only the area it is given.

**Out of scope:**

- **Compacting** sectors to gather erased space (the user: "a future
  option").
- **The flash task**, which serves alfs from one FreeRTOS task, until this
  interface settles.
- **Rows:** what a file holds is the application's.
- **Editing a file:** writing into its middle, inserting, reopening a
  closed file (the user: "We should not support PATCH."). A configuration
  file is written whole, as a new file.
- **Renaming**, directories.
- NAND, SD cards, eMMC; a part whose prog unit is over one byte; an erase
  value other than 0xFF; encryption.

---

## 3. Use cases

Each comes from the pyro flight computer (github.com/n9wxu/pyro_fw). The
references are to its `DECISIONS.md` (DD-nnn), its requirements
(`REQUIREMENTS.md`, FLT-LOG-nn) and its open tasks
(`docs/outstanding_tasks.md`).

| # | Use case | Need |
|---|---|---|
| UC-1 | **A flight log.** 22-byte rows at 1 to 50 a second, written by a task of a system whose flight loop runs every 20 ms, and synced every second. | Every flash program or erase on the RP2040 stops both cores (DD-074), and an erase takes 35 to 800 ms (section 4.3), many loop periods. Hence the rulings. |
| UC-2 | **A high-rate log beside it.** Everything sensed, about 24 kB a second, written by another task into a file of its own. | On NOR it is about 100 page programs a second. |
| UC-3 | **Restart after a reset in flight.** The board restarts and logs on: into the file cut short, reopened, or into another file allocated before flight. | The time from reset to the first sample is flight time lost (DD-086). |
| UC-4 | **Keep the newest flights.** Two, the oldest deleted by the application. | C6. |
| UC-5 | **Erase before flight.** On the bench, or paced on the pad; launch waits until it is done. | The rulings; C6's OK to LAUNCH gate. A 4 KB erase takes 35 to 45 ms typically and up to 800 ms (section 4.3). |
| UC-6 | **Read a log back.** Sequentially, for download, perhaps while the newest log is written. | `/api/flight.csv` renders the log as it reads. |
| UC-7 | **Power cut at any moment.** A battery lead lost at landing. | FLT-LOG-06: "no more than the last second shall be lost." |
| UC-8 | **The wrong geometry.** An image built for an 8 MB chip, run on a 2 MB one. | Nothing may be written past the chip's end or outside the area, where the bootloader and the code lie (DD-098). |
| UC-9 | **Write failures.** A program that fails part-way. | Nothing is written twice, and every byte lost is counted (L4). |
| UC-10 | **Unattended, soldered-in flash.** Replacing the chip loses everything; the unit may be out of reach. | Degrade gracefully, shrink with bad sectors, stay readable at end of life, report health. |
| UC-11 | **Configuration.** Settings saved on the ground, a few hundred bytes in all, read at boot. | pyro's settings are files of under 1 kB (`pins.ini`, `beep.ini`, `pad.mkr`). |
| UC-12 | **Lua programs.** Each its own file, saved whole from the web interface, read whole when loaded. | The user: "LUA programs should still be a full file write." |
| UC-13 | **Many tasks, one flash.** The flight log from a storage task, settings, programs and downloads from a network task, each blocking only itself. | pyro runs FreeRTOS on both cores (DD-073); its flash operations go through one routine that stops both cores for one operation at a time (DD-074, `src/flash_op.c`). |
| UC-14 | **Allocate before flight.** The flight log's file is allocated on the bench or the pad with the size a flight needs, so that logging takes no flash work but page programs. | The rulings; the user's file_alloc. |

---

## 4. The flash model

### 4.1 Terms

- **area:** the sector-aligned range of the chip given to alfs.
- **sector:** the erase unit, a power of two (4,096 bytes on every part
  here).
- **page:** the most one program writes, and the unit a file's space is
  allocated in; a program never crosses a page boundary (256 bytes here).
  Pages are numbered from the area's start; that number is a page's
  **address**.
- **prog unit:** the smallest unit the flash programs on its own. alfs
  requires one byte (A-2, A-8, A-9).
- **flash operation:** one call to the driver's program or erase. Reads are
  not operations: they do not take the chip out of execute-in-place.
- **stall:** the time one flash operation stops a system that executes from
  the same chip: on the RP2040, every core (section 4.3).
- **file**, or **frame:** the bytes an application writes under one
  filename, held in a frame's pages.
- **filename:** 1 to 31 bytes the application chooses, none 00h. A filename
  of all 00h is one a delete blocked out.
- **frame ID:** the number alfs gives a frame when it is allocated: unique
  for the life of the area, growing. The filename converts to it; it is the
  file's descriptor.
- **page header:** the start of every page of a frame: the frame ID, the
  page's position in the frame (its **page count**, from 0), a check, and
  the next page's address.
- **byte count:** the bytes of data a page holds: a full page's, or fewer
  in a page a flush or a close left partial.
- **link:** a page header's next page address. Erased: no next page yet.
  Zero: the frame's last page.
- **frame block:** the start of a frame's first page, after its page
  header: the frame ID, the filename, and the **close fields**: the page
  count and the size in bytes, with a check of their own.
- **data check:** a CRC-32 straight after each page's data, programmed with
  it and its byte count.
- **reservation:** the pages `alfs_file_alloc` links into a frame before
  any data is written. Its end is the page whose link is erased.
- **growth:** pages a write links past the reservation, when the file was
  opened with `ALFS_GROW`.
- **catalog:** the sectors that map filenames to frame IDs and first pages;
  one is in use, the other its spare.
- **close:** the call that ends a file: its last data, its close fields,
  the zero link, and the erase of the reservation's unused sectors. A
  closed file is final.
- **live page:** a page of a file that is not deleted: any page of a file
  not closed, or a page of a closed file within its page count.
- **free page:** a page that reads erased, header and data, and is in no
  file.
- **dead page:** a page that is neither live nor free: what a cleanup
  erases.
- **block:** RAM the application gives with a file it writes or reads,
  where alfs gathers the bytes written into whole pages, and holds a page
  read.
- **sync:** a call after which every complete page written is durable.
- **flush:** a call after which every byte written is durable: the last
  page partial, the file going on in the next page.
- **durable:** survives a power loss at any later moment.

### 4.2 Assumptions about the flash

| # | Assumption | Source |
|---|---|---|
| A-1 | A program moves bits only away from the erase value; only an erase moves them back. | Every NOR part |
| A-2 | A program of fewer bytes than a page, into erased bytes, leaves the page's other bytes as they were, and a page may be programmed so at least 16 times between erases. | All four datasheets (`docs/datasheets/README.md`); none states a limit. The probe programs pages 2,048 times (A-8) |
| A-3 | A program interrupted by power loss changes no byte outside its own page. | The fault model (the littlefs fork's `lfs3.h`, `gc_preerase_count`) |
| A-4 | An erase interrupted by power loss changes no byte outside its own sector, and may leave that sector in any state, including one that reads as erased but holds bits that read randomly. | METASTABLE (4.4) |
| A-5 | Operations complete in the order issued; there is no write-back cache. | Raw serial NOR |
| A-6 | The erase value is known and fixed: 0xFF on every part here. | The datasheets |
| A-7 | A read can fail, as at low supply voltage, and succeed when retried. | UC-10 |
| A-8 | A program into bytes already programmed clears exactly the bits that are 0 in its data and changes no other bit of the page. A page takes at least 2,048 programs between erases. | No datasheet states it: the Winbond parts' Page Program writes "previously erased (FFh) memory locations" (W25Q16JV p. 34, W25Q128JV p. 37), and the XT25F128F and BY25Q64ES sheets say nothing. Measured by the probe (`docs/FLASH_PROBE.md`): it holds on the W25Q128JV, two XT25F128F and two BY25Q64ES; the W25Q16JV is untested |
| A-9 | A program that a power cut interrupts never clears a bit its data left 1. Programming the intended bytes again over it leaves them reading as intended on every later read, and changes no other byte. | No datasheet states it. Measured by the probe's P8, which interrupts programs with the chips' software reset (66h then 99h; W25Q128JV p. 60, XT25F128F p. 41, BY25Q64ES p. 30) as a stand-in for a power cut (`docs/FLASH_PROBE.md` section 4). It holds on the W25Q128JV and two XT25F128F, including bits torn into reading either way; the BY25Q64ES finishes an operation despite the reset, so is untested, as is the W25Q16JV |

A-8 and A-9 are measured, not read. A part with error correction inside
the chip breaks them, so a new part is probed before alfs runs on it.

A-9 lets alfs settle what a power cut may have left half programmed, by
programming 00h over it: the link a close ends (IF-10), the filename a
delete blocks out (IF-12).

### 4.3 Timing and endurance

| Part (pyro board) | Page program typ / max | 4 KB erase typ / max | 64 KB erase typ / max | Endurance |
|---|---|---|---|---|
| W25Q16JV (MK1B) | 0.4 / 3 ms | 45 / 400 ms | 150 / 2,000 ms | 100K cycles, minimum |
| W25Q128JV (MK1A) | 0.4 / 3 ms | 45 / 400 ms | 150 / 2,000 ms | 100K, minimum |
| XT25F128F (MK1C) | 0.4 / 2 ms | 40 / 800 ms | 250 / 1,600 ms | 100,000, minimum |
| BY25Q64ES (MK1B-8MB) | 0.45 / 2.4 ms | 35 / 300 ms | 180 / 2,000 ms | 100k, typical |

Pages are cited in `docs/datasheets/README.md`. All four can suspend an
erase, so a driver can slice one and let the system run between the
slices; to alfs, an erase returns when it is done. On the RP2040 a program
or erase stops every core that runs code from the same chip for its whole
duration (`rp2040-datasheet_2025-02-20.pdf` p. 136).

Measured on the chips themselves (`docs/FLASH_PROBE.md`), a program of 1
to 4 bytes takes 21-26 us on the W25Q128JV and 55-61 us on the XT25F128F
and BY25Q64ES; a whole page 0.2-0.4 ms; a sector erase 15 ms (XT25F128F),
26 ms (BY25Q64ES) and 47 ms (W25Q128JV). The requirements use the
datasheets' figures.

### 4.4 Faults

Power-loss behaviours, after the littlefs fork's emulated block device
(`bd/lfs3_emubd.h`), and FLICKER. The interrupted operation:

| Name | Program | Erase |
|---|---|---|
| ATOMIC | is not applied | is not applied |
| SOMEBITS | one bit of its first byte that its data clears is programmed | the old data stays and one bit flips |
| MOSTBITS | is applied, then one bit it cleared reads erased again | is applied, then one bit anywhere flips |
| METASTABLE | is applied, then one bit it cleared reads randomly until the sector is erased or a program clears it | is applied, then one bit of the sector reads randomly until erased |
| TORNTAIL | its first byte stays as it was, a run of random length after it is programmed, and one bit the run cleared reads erased again | is atomic |
| FLICKER | is not applied, but one bit its data clears reads randomly | is not applied, but one bit of the sector reads randomly |

FLICKER is the state the probe produces on the XT25F128F by aborting a
program early: a page that reads erased but for a bit that reads either
way (`docs/FLASH_PROBE.md` section 4).

Every torn program moves only bits its data moves (A-9): a program cannot
move a bit its data leaves as it was.

Reordering of the writes since the last sync needs a write cache, and is
out of scope (A-5).

Bad-sector behaviours, from the same device: PROGERROR (a program fails and
writes nothing), ERASEERROR (an erase fails and erases nothing), READERROR
(reads fail), PROGNOOP (a program silently does nothing), ERASENOOP (an
erase silently does nothing, and programs do nothing), PROGFLIP (a program
lands with one bit wrong), READFLIP (reads flip one bit half the time).
And wear-out: a sector goes bad after a number of erases.

The tests also inject a torn erase that reads as erased (A-4), a read that
fails once then succeeds (A-7), and a failure confined to some offsets of
a sector.

### 4.5 Workloads

Measurements use these, on the W25Q128JV's geometry (4,096-byte sectors,
256-byte pages), timed with the typical figures of 4.3, and with a block of
one page unless a workload says otherwise.

- **W-LOG:** 22-byte rows produced at 1 or 50 a second, written every
  200 ms, synced every second, for 10 minutes, into a file allocated with
  room for them.
- **W-HR:** 480 bytes written every 20 ms (24,000 a second), synced every
  second, for 10 minutes.
- **W-RING:** W-LOG at 50 rows a second into a file of its own each 10
  minutes; the oldest file deleted whenever fewer than two files' worth of
  pages are free, and one cleanup step every 20 ms; until every sector has
  been erased at least 10 times.
- **W-CFG:** a 300-byte settings file and a 4,096-byte and a 300-byte Lua
  program, each updated by blocking out its filename and writing a new
  file, one of them deleted and written again.

---

## 5. How to read a requirement

```
#### ALFS-<AREA>-<NN>
The requirement, one sentence where possible.
- Priority: 1 speed, 2 life, 3 safety, or S simplicity.
- Source: the use case, ruling, datasheet or measurement it comes from.
- Measure: what is counted, and how.
- Pass: the threshold. Fail: what fails it.
- Verified by: the test (named in docs/TEST_PLAN.md).
```

---

## 6. Requirements

### 6.1 Speed and bounded time (SPD)

#### ALFS-SPD-01

Only these erase: `alfs_file_alloc`, when too few pages are free, and
when it rewrites the catalog; `alfs_file_close`, the sectors of the
reservation it did not use; `alfs_fs_cleanup`; and `alfs_fs_format`.
Nothing else erases: not a write, a sync, a flush, a read, a list, a
mount, or an open.

- Priority: 1.
- Source: the rulings; UC-1, UC-14; the user: "No new pages will be erased."
- Measure: erases the simulator counts during each call, under W-LOG,
  W-HR, W-RING and W-CFG, with power loss at every operation.
- Pass: zero in every other call. Fail: any.
- Verified by: `spd_no_erase_outside_erase`.

#### ALFS-SPD-02

A write, sync, flush or close programs each page's data once, in one
program with its byte count and its data check. A write programs only when
the block is full, and then every complete page the block holds; a sync
programs every complete page it holds; a flush and a close program all it
holds, the last page partial. Each page taken past the reservation adds two
programs: its header, and the link to it.

- Priority: 1.
- Source: the user: "Block the memory into pages, sync the complete pages,
  and wait for more data." and "A partial write to a page cannot update the
  CRC so only full pages can be written. A page byte count can be added to
  the page format so that a force flush can leave a partial page." UC-1,
  UC-2; the stall of 4.3.
- Measure: programs the simulator counts during each write, sync and flush,
  against the pages the call filled and the pages it took, with blocks of
  one page and of 4,096 bytes.
- Pass: as stated. Fail: more programs, a second program into a page's
  data, or a partial page outside a flush or a close.
- Verified by: `if_write_pages`, `if_flush`, `spd_write_programs_bounded`.

#### ALFS-SPD-03

Every program is of one page or less, within one page.

- Priority: 1.
- Source: 4.1; a page program is the shortest stall (4.3).
- Measure: the simulator rejects a program that crosses a page.
- Pass: none crosses. Fail: any.
- Verified by: every test (the simulator asserts it).

#### ALFS-SPD-04

A write past the reservation returns `ALFS_ENOSPC`, keeps the bytes it
holds in the block, and erases nothing, when the file was opened without
`ALFS_GROW`, or when no page is free.

- Priority: 1.
- Source: the user: "a best effort will be made to continue page
  allocations if there are erased pages available. No new pages will be
  erased." and "We can add a flag that will allow writes to continue if
  space is available, or fail if no space is reserved."
- Measure: files written past their reservations, opened with and without
  `ALFS_GROW`, until no page is free.
- Pass: `ALFS_ENOSPC`, no erase, the bytes taken read back after a close;
  without the flag, no page taken past the reservation. Fail: an erase, a
  lost byte, or a page taken without the flag.
- Verified by: `if_grow`.

#### ALFS-SPD-05

Mounting reads at most the two catalog sectors and performs no flash
operation.

- Priority: 1.
- Source: UC-3: the time from reset to the first sample is flight time
  lost.
- Measure: bytes the simulator reads during each mount of an area holding
  files.
- Pass: at most two sectors' bytes, no operation. Fail: more, or any.
- Verified by: `spd_mount_reads_bounded`.

#### ALFS-SPD-06

On W-LOG and W-HR no flash operation stalls longer than one page program.

- Priority: 1.
- Source: UC-1, UC-2. A page program measures at most 0.46 ms on the chips
  (`docs/FLASH_PROBE.md`).
- Measure: simulated time of each operation, at the typical times of 4.3.
- Pass: at most 0.4 ms. Fail: more.
- Verified by: `spd_wlog_longest_stall`.

#### ALFS-SPD-07

The processor time of a write or read, apart from its flash operations,
grows only with the bytes the call passes, plus a constant; a step that
follows a link reads one page header, and a search for a free page reads
at most the headers of the pages between where it starts and the page it
takes.

- Priority: 1.
- Source: UC-1.
- Measure: bytes read by the same writes and syncs into a reserved file on
  areas of 32 and 2,048 sectors.
- Pass: the same. Fail: a term in the area's size.
- Verified by: `spd_cpu_linear`.

#### ALFS-SPD-08

Opening a file for reading reads its catalog entry and its first page.
Opening it for writing reads its catalog entry and each page header of its
list once, to know where the reservation ends. Reopening a log not closed,
after a mount, also reads each page's byte count once, to find where
writing goes on and the size so far, and the page found there whole.

- Priority: 1.
- Source: UC-3, UC-6; SPD-04: without `ALFS_GROW`, a write takes only the
  bytes the reservation holds.
- Measure: bytes read by opens of files of 1, 10 and 100 pages.
- Pass: at most the catalog sector and a page for a read; the catalog
  sector, the frame block and a header for each page for a write; and a
  byte count for each page and one page more for a reopen. Fail: more.
- Verified by: `spd_open_reads_bounded`.

#### ALFS-SPD-09

Every loop alfs runs ends within a bound set by the area's page count, the
bytes the call passes, or the catalog's entries, whatever the flash holds.
A list of links that returns to a page already passed ends there.

- Priority: 1.
- Source: "A corrupted data block is less important than a broken exec
  loop."
- Measure: every call made on fuzzed areas (SAFE-10); review.
- Pass: every call returns. Fail: any that does not.
- Verified by: `safe_fuzz`; review.

#### ALFS-SPD-10

Allocating R pages performs R + 1 programs, the catalog entry and each
page's header, the first page's with the frame block; and erases only
sectors holding no live page, only as many as the reservation needs past
the free pages there are.

- Priority: 1.
- Source: the user: "the full linked list of pages erased and created."
- Measure: programs and erases during allocations into an area cleaned,
  and into one holding dead pages.
- Pass: as stated. Fail: more.
- Verified by: `if_alloc`.

### 6.2 Flash life (LIFE)

#### ALFS-LIFE-01

An erase takes only a sector holding no live page; never a sector that
reads erased.

- Priority: 2.
- Source: the brief ("preserving the flash memory").
- Measure: under W-RING and W-CFG, each erase classified by the sector's
  content before it.
- Pass: as stated. Fail: an erase of a sector holding a live page, or of
  one that reads erased.
- Verified by: `life_erase_only_unused`.

#### ALFS-LIFE-02

The bytes programmed are what the files need: each page of data, its data,
a 2-byte byte count and a 4-byte check; each page allocated, a 14-byte
header, and the first page 36 bytes more of its frame block; a close, 10
bytes of close fields and a 4-byte zero link; a page taken past the
reservation, its header and a 4-byte link; a catalog entry, 44 bytes.

- Priority: 2.
- Source: the brief; `docs/SPEC.md`.
- Measure: bytes the simulator programs during W-LOG and W-HR, against
  the sum of those figures from the calls' own counts.
- Pass: equal. Fail: more.
- Verified by: `life_write_amplification`.

#### ALFS-LIFE-03

On W-LOG into a file allocated to fit it, the erases of its allocation,
its close and the cleanup after it is deleted are no more than the sectors
its pages fill, plus one.

- Priority: 2.
- Source: the brief; UC-1.
- Measure: erases over the file's life against its pages divided by the
  pages a sector holds.
- Pass: as stated. Fail: more.
- Verified by: `life_wlog_erases`.

#### ALFS-LIFE-04

Wear is level: allocation takes free pages forward around the area from
where it last stopped, so under W-RING the erase counts of the good
sectors differ by at most 2.

- Priority: 2.
- Source: UC-10; 100K-cycle endurance (4.3).
- Measure: each sector's erase count at the end of W-RING.
- Pass: largest minus smallest at most 2. Fail: more.
- Verified by: `life_wear_level`.

#### ALFS-LIFE-05

A close erases each sector of the reservation that holds no live page; the
unused pages of a sector that also holds a live page stay, dead, until a
cleanup finds the sector holds no live page.

- Priority: 2.
- Source: the user: "the close will ... erase all the unused pages", and
  "erase requires a sector or a block and not simply a page."
- Measure: files closed short of their reservations, across sector
  boundaries and within one.
- Pass: the sectors holding no live page erased; every other sector's
  bytes as they were. Fail: otherwise.
- Verified by: `if_close`.

### 6.3 Safety (SAFE)

#### ALFS-SAFE-01

Once a close returns, every byte of the file reads back after a power loss
at any later operation.

- Priority: 3.
- Source: UC-7, UC-11, UC-12.
- Measure: W-LOG and W-CFG with power lost after operation k, for every k,
  under every behaviour of 4.4; remount; read every file closed before the
  loss.
- Pass: every byte, in order. Fail: any missing or wrong.
- Verified by: `safe_powerloss_durable`.

#### ALFS-SAFE-02

A power loss while a file is written loses at most the bytes not yet in a
page programmed: after a sync, fewer than one page's data; after a flush,
none written before it.

- Priority: 3.
- Source: the user: "sync the complete pages, and wait for more data" and
  "a force flush can leave a partial page and the following page will
  continue the file." UC-7, FLT-LOG-06: a flush each second keeps the loss
  under a second.
- Measure: as SAFE-01, for the file being written, synced each second in
  one run and flushed each second in another, counting bytes lost.
- Pass: the file reads a prefix of what was written, no shorter than the
  complete pages synced, or than every byte flushed. Fail: shorter, or a
  wrong byte.
- Verified by: `safe_powerloss_bound`.

#### ALFS-SAFE-03

A read never returns a byte of a page whose data check fails. The page is
skipped and counted, and reading goes on at the next page of the list.

- Priority: 3.
- Source: UC-7; METASTABLE and the FLIP behaviours.
- Measure: one bit flipped in a page's byte count, data or check.
- Pass: the page's bytes are not returned, one failure is counted, the
  pages after it read. Fail: a damaged byte returned.
- Verified by: `safe_no_false_data`.

#### ALFS-SAFE-04

After a mount, writing a file that was not closed goes on at the first
page of its list whose byte count, data and data check read erased. A page holding any
data, whole or torn, stays as it is, and the reader skips it if its check
fails. A page a power loss tore that reads erased is written as erased; its
data check then shows any damage.

- Priority: 3.
- Source: the user: "If the page looks erased after a reboot, it can only
  be treated as erased. ... If there is partial data, then it can be left
  alone and the next frame in the allocated sequence get the next data."
- Measure: power lost at every operation of writing a file, under every
  behaviour; remount, reopen, write, close, read.
- Pass: every byte read is one written, in order; what is lost is the
  bytes of the cut page and those not yet in a complete page. Fail: a byte
  read that was not written, or out of order.
- Verified by: `safe_reopen_after_loss`.

#### ALFS-SAFE-05

alfs programs data only into pages that read erased, in sectors whose
erase it checked by reading every byte back as 1s: that read is what shows
an erase finished.

- Priority: 3.
- Source: the rulings; the user: "Erases are finshed when a read returns
  1's for all pages."; ERASENOOP.
- Measure: ERASENOOP sectors met by a cleanup and an allocation.
- Pass: no page of such a sector in a file. Fail: any.
- Verified by: `safe_write_only_verified_erased`.

#### ALFS-SAFE-06

A failed data program is counted, and its bytes are counted lost; writing
goes on at the next page of the list. A failed header program during an
allocation leaves that page out of the list. A program is checked by
reading it back.

- Priority: 3.
- Source: UC-9. "A corrupted data block is less important than a broken
  exec loop": no retry repeats a stall.
- Measure: PROGERROR, PROGNOOP and PROGFLIP sectors under W-LOG and
  allocation.
- Pass: every byte reads back or is counted lost; nothing is programmed
  twice. Fail: otherwise.
- Verified by: `safe_prog_failure`.

#### ALFS-SAFE-07

A sector that fails an erase, or its check after one, is not used again in
this mount; the area's free pages shrink by it.

- Priority: 3.
- Source: UC-10.
- Measure: ERASEERROR and ERASENOOP sectors, and wear-out under W-RING.
- Pass: counted bad and never given to a file. Fail: otherwise.
- Verified by: `safe_bad_sector`.

#### ALFS-SAFE-08

A failed read is returned to the caller, with every handle's position and
every other state unchanged, so that a retry reads the same bytes.

- Priority: 3.
- Source: A-7.
- Measure: a read that fails once, in each mount, open and read.
- Pass: the error, then the right bytes on retry. Fail: otherwise.
- Verified by: `safe_read_retry`.

#### ALFS-SAFE-09

alfs programs and erases only inside its area. A configuration whose area
does not lie inside the chip is refused before any operation. An area
holding no catalog fails to mount with `ALFS_ENOFS` and is not written.

- Priority: 3.
- Source: UC-8.
- Measure: areas holding random data, blank areas, an area past the chip's
  end.
- Pass: as stated. Fail: any write outside the area, or any write at
  mount.
- Verified by: `safe_foreign_area`.

#### ALFS-SAFE-10

Every call finishes, without a crash, on any content of the area: links out
of the area, into the catalog, back to a page already passed, or to a page
of another frame.

- Priority: 3.
- Source: UC-10; crafted images.
- Measure: fuzzed areas under a sanitizer: every call.
- Pass: every call returns; no sanitizer error. Fail: otherwise.
- Verified by: `safe_fuzz`.

#### ALFS-SAFE-11

An allocation is atomic: after a power loss during it, the filename names
no file, or a file whose every reserved page is in its list.

- Priority: 3.
- Source: UC-14. The catalog entry is programmed first, then the pages from
  the last to the first: the file exists once its first page's header and
  frame block are on the flash.
- Measure: power lost at every operation of allocations, under every
  behaviour; remount eight times; open, write, close, read.
- Pass: as stated, and the frame ID never given to another file. Fail:
  otherwise.
- Verified by: `safe_alloc_atomic`.

#### ALFS-SAFE-12

A close is atomic: until it returns, after a power loss the file reads as
not closed, every page it holds, or as closed, whole.

- Priority: 3.
- Source: UC-7. The close fields carry their own check and are programmed
  after the last data and before the zero link.
- Measure: power lost at every operation of a close, under every
  behaviour; eight remounts reading the file.
- Pass: as stated each time. Fail: a closed file that does not read whole.
- Verified by: `safe_close_atomic`.

#### ALFS-SAFE-13

A delete programs 00h over the filename, in the catalog entry and in the
frame block, and leaves the pages as they are. Once it returns, the file
never reads again after any power loss; until then it reads whole or not
at all.

- Priority: 3.
- Source: the user: "Erasing a file is simply writing 0's over the
  filename. The rest of the data can be left alone unless we need to scrub
  the file system."
- Measure: power lost at every operation of a delete, under every
  behaviour; eight remounts.
- Pass: as stated. Fail: a file that comes back, or reads in part.
- Verified by: `safe_delete_atomic`.

#### ALFS-SAFE-14

A file is valid when its pages are valid. A read of a closed file that
skipped a page, or found its list ending before its page count, returns
`ALFS_ECHECK` when it reaches the end.

- Priority: 3.
- Source: the user: "A file is valid if its pages are valid." UC-12: a
  program partly wrong is worse than one missing, and a configuration file
  is read whole.
- Measure: one bit flipped in a closed file's page; a closed file's list
  cut short.
- Pass: every good byte read, then `ALFS_ECHECK`. Fail: the end reached
  with no error.
- Verified by: `safe_file_check`.

#### ALFS-SAFE-15

A link is followed only to a page inside the area, past the catalog, whose
header check passes and which names the same frame and the next position;
otherwise the list ends there, and the failure is counted.

- Priority: 3.
- Source: UC-10: a damaged link must not lead a read into another file.
- Measure: links bent to another frame's page, a wrong position, the
  catalog, and past the area.
- Pass: the list ends at the bent link; no byte of another page returned.
  Fail: otherwise.
- Verified by: `safe_links_checked`.

#### ALFS-SAFE-16

Rewriting the catalog is atomic: the spare catalog sector takes the live
entries and becomes the catalog only when complete; after a power loss at
any point, every file reads as before.

- Priority: 3.
- Source: UC-10. Section 8, D-3: the user named "a catalog sector"; one
  sector cannot be rewritten without a moment when it holds nothing.
- Measure: power lost at every operation of a rewrite, under every
  behaviour; remount; read every file.
- Pass: every file whole. Fail: any missing.
- Verified by: `safe_catalog_rewrite_atomic`.

### 6.4 Simplicity (SIMP)

#### ALFS-SIMP-01

The store is C99: `alfs.h` (the shared types), `alfs_flash.c` and
`alfs_flash.h` (the checked flash operations), `alfs_fs.c` and
`alfs_fs.h`. It uses no dynamic memory, no recursion, no floating point,
and no library beyond `<stdint.h>`, `<stddef.h>`, `<stdbool.h>` and
`<string.h>`.

- Priority: S.
- Verified by: `make target`; review.

#### ALFS-SIMP-02

The store is at most 6 KiB on a Cortex-M0+ (Thumb, `-Os`).

- Priority: S.
- Source: the brief. An estimate, until there is code to measure.
- Verified by: `make target`.

#### ALFS-SIMP-03

RAM does not grow with the area, and all of it is the caller's: the
instance at most 96 bytes and a scratch page; a writer at most 48 bytes and
its block, of at least one page; a reader at most 40 bytes and its block, of
at least one page.

- Priority: S.
- Source: the user: "Applications bring their ram so ram allocations in
  ALFS should not be an issue."
- Verified by: `simp_ram`; `make target`.

#### ALFS-SIMP-04

No call uses more than 320 bytes of stack, the driver's own frames apart.

- Priority: S.
- Source: pyro's smallest task stack is 2 kB.
- Verified by: `make target`.

#### ALFS-SIMP-05

The on-disk format is specified, every byte, in `docs/SPEC.md`, and has one
version.

- Priority: S.
- Verified by: review.

### 6.5 Interface (IF)

#### ALFS-IF-01

The driver is three functions: read bytes, program bytes within one page,
and erase one sector. Each returns when its work is done, or fails; a read
sees every program and erase before it.

- Priority: S.
- Verified by: the simulator implements exactly these.

#### ALFS-IF-02

The area is configured: the sector and page size (powers of two, page at
least 128 bytes, sector at least four pages), its start and length
(sector-aligned, at least four sectors, at most 65,535 pages), and the
chip's size. alfs programs a byte at most once between erases, except with
00h: a link a close ends, a filename a delete blocks out.

- Priority: S.
- Verified by: `if_geometry`; every test (the simulator refuses a second
  program of a byte unless it settles it).

#### ALFS-IF-03

The on-disk format is little-endian and read and written byte by byte.

- Priority: S.
- Verified by: `if_endian`.

#### ALFS-IF-04

The driver programs exactly the bytes alfs passes it, and no others.

- Priority: S.
- Source: the probe: a page padded with the erase value takes 3.2 times as
  long on the BY25Q64ES (`docs/FLASH_PROBE.md` section 3).
- Verified by: review of each driver.

#### ALFS-IF-05

A file is found by its filename: 1 to 31 bytes, none 00h, compared byte
for byte. A filename names at most one file; it converts to the file's
frame ID, its descriptor, which every later call on the open file uses.

- Priority: S.
- Source: the user: "Files are opened/closed/allocd by filename. The
  filename converts to a frame id which is essentially the file
  descripter."
- Verified by: `if_names`.

#### ALFS-IF-06

`alfs_file_alloc` takes a filename and a reserved size in bytes, and
returns the frame ID. It reserves the pages the size needs and links them
in a list, the last page's link left erased; it writes the frame block with
the page count and size left erased. The reserved size is not
recorded: the list is the reservation. A filename that names a file is
refused with `ALFS_EEXIST`.

- Priority: S.
- Source: the user: "The file_alloc will accept the file name, and reserved
  file size." and "The linked list is assembled and that is sufficient."
- Verified by: `if_alloc`.

#### ALFS-IF-07

`alfs_file_open` opens a file by filename for writing: one writer a file.
`ALFS_GROW` lets writes go on past the reservation into free pages
(IF-08); without it they stop there (SPD-04). A file not closed is written
from the first page of its list whose byte count, data and data check read
erased (SAFE-04). A closed file is final: opening it for writing is refused
with `ALFS_EEXIST`.

- Priority: S.
- Source: the user: "If the file has not been closed, it can be opened."
  and "We should not support PATCH."
- Verified by: `if_reopen_unclosed`, `if_names`.

#### ALFS-IF-08

A write copies the caller's bytes into the block; when the block is full,
every complete page's data it holds is programmed, page by page, along the
list, and the rest stays in the block. The write returns when every byte it
was given is in flash or in the block. Past the reservation, a file opened
with `ALFS_GROW` links free pages while there are any; one opened without
it takes only the bytes its reservation holds, so its close never wants a
page.

- Priority: S.
- Source: the user's words on writes and on growth, section 1.
- Verified by: `if_write_pages`, `if_grow`.

#### ALFS-IF-09

A sync programs every complete page's data the block holds; the rest stays
in the block until more bytes complete its page, a flush, or the close.

- Priority: S.
- Source: the user: "sync the complete pages, and wait for more data."
- Verified by: `if_write_pages`.

#### ALFS-IF-10

A close programs what the block holds, the last page partial, then the
close fields, then 00h into the last page's link, and then erases the
sectors of the reservation that hold no live page.

- Priority: S.
- Source: the user: "the close will update the page-count value mark the
  last page address link with 0's, and erase all the unused pages."
- Verified by: `if_close`.

#### ALFS-IF-11

A file is read from its start, by filename, closed or not, page by page,
each page's byte count giving its data: a closed file to its page count;
one not closed to the first page of its list whose byte count reads
erased, or the list's end.

- Priority: S.
- Source: UC-6: a log is read back, and may be read while it is written or
  after a power loss cut it short.
- Verified by: `if_read`, `if_flush`.

#### ALFS-IF-12

`alfs_file_delete` removes a file by filename (SAFE-13). A file open for
writing or reading is refused with `ALFS_EOPEN`. A configuration file, a
Lua program among them, is written again by deleting its file and writing
a new one of the same filename.

- Priority: S.
- Source: the user: "A simple LUA update will block out the filename and
  write a new file." and "configuration (open, close, read whole file,
  write whole file)."
- Verified by: `if_delete`.

#### ALFS-IF-13

Listing the files reports each one's frame ID, filename, whether it is
closed, and, once closed, its page count and size, in growing frame ID.

- Priority: S.
- Verified by: `if_list`.

#### ALFS-IF-14

A frame ID is unique for the life of the area, grows with each file
allocated, and is never given again, even after a delete or a power loss
during an allocation.

- Priority: S.
- Source: the frame ID is the file's descriptor (IF-05): a stale one must
  not name another file.
- Verified by: `if_ids_never_reused`.

#### ALFS-IF-15

`alfs_fs_cleanup` erases up to a given number of sectors, one a step, each
one holding no live page and not reading erased, and checks each erased;
it returns `ALFS_CLEAN` when none is left.

- Priority: S.
- Source: the user: "A filesystem 'cleanup' operation is available to
  pre-erase all unused pages sector by sector."
- Verified by: `if_cleanup`.

#### ALFS-IF-16

`alfs_fs_format` erases the catalog sectors and writes an empty catalog;
every other sector then holds no file, for a cleanup to erase.

- Priority: S.
- Verified by: `if_format_mount`.

#### ALFS-IF-17

A mount is refused with `ALFS_EOPEN` while any file is open. An allocation,
a delete or a cleanup leaves every open file as it was.

- Priority: S.
- Verified by: `if_refusals`.

#### ALFS-IF-18

A flush programs everything the block holds: the complete pages, and the
rest as a partial page with its byte count and check. The file goes on in
the next page of the list.

- Priority: S.
- Source: the user: "a force flush can leave a partial page and the
  following page will continue the file."
- Verified by: `if_flush`.

### 6.6 Observability and errors (OBS)

#### ALFS-OBS-01

alfs reports the area's live, free and dead pages, the catalog's entries
used and its capacity, and its counts of programs, erases, failed
programs, failed erases, failed checks and bytes lost. The report reads
every page header, and counts no failure of its own.

- Priority: 3.
- Source: UC-5, UC-9, UC-10.
- Verified by: `obs_status`.

#### ALFS-OBS-02

Every error a call can return is documented with the state it leaves and
what the caller should do: retry, rebuild (format), or fail.

- Priority: 3.
- Verified by: review of `alfs.h` and `alfs_fs.h`.

#### ALFS-OBS-03

alfs reports whether the area is clean, no dead page left, so the caller
can hold a launch until all unused flash is erased.

- Priority: 3.
- Source: UC-5; the first ruling.
- Verified by: `obs_clean`.

---

## 7. Traceability

| Use case | Requirements |
|---|---|
| UC-1 flight log | SPD-01, SPD-02, SPD-06, SPD-07, LIFE-02, LIFE-03, IF-08, IF-09, IF-18 |
| UC-2 high-rate log | SPD-02, SPD-06, LIFE-02 |
| UC-3 restart after a reset | SPD-05, SPD-08, SAFE-04, IF-07 |
| UC-4 newest flights | IF-12, IF-13, IF-15, SAFE-13 |
| UC-5 erase before flight | IF-15, LIFE-01, OBS-03 |
| UC-6 read back | SAFE-03, SAFE-08, SAFE-15, IF-11 |
| UC-7 power cut | SAFE-01, SAFE-02, SAFE-04, SAFE-12, IF-18 |
| UC-8 wrong geometry | SAFE-09, IF-02 |
| UC-9 write failures | SAFE-06, OBS-01 |
| UC-10 unattended flash | LIFE-04, SAFE-07, SAFE-10, SAFE-16, SPD-09, OBS-01, OBS-02 |
| UC-11 configuration | SAFE-01, SAFE-13, IF-12 |
| UC-12 Lua programs | SAFE-01, SAFE-13, SAFE-14, IF-12 |
| UC-13 many tasks | out of scope until the interface settles (section 2) |
| UC-14 allocate before flight | SPD-01, SPD-10, SAFE-11, IF-06 |

---

## 8. Open decisions

Each is the user's to make. `experiments/v6-frames/EVALUATION.md` gives
the reasons and the options.

- **D-1. A close erases.** Each erase stalls 35 to 800 ms (4.3). A close on
  the pad, or in flight, stalls the flight loop for every unused sector.
- **D-2. A page that reads erased after a reboot is written as erased.** A
  page a power loss tore that reads erased is written over; its data check
  catches the damage, and the page is lost.
- **D-3. The catalog is two sectors.** The user named one; one sector
  cannot be rewritten, when its entries run out, without a moment when it
  holds no catalog.
- **D-4. A configuration update deletes, then writes.** Between the two,
  the file exists under no filename; after a power loss there, it is gone.

---

## 9. Sources

- `docs/datasheets/` and its `README.md`: the four flash parts and the
  RP2040.
- `docs/FLASH_PROBE.md`: what the probe measures on the parts themselves.
- The littlefs fork (github.com/n9wxu/littlefs), branch `v3-integration`
  at 58367256: `bd/lfs3_emubd.h` (the fault model), `lfs3.h` (A-3),
  `benches/bench_wlog.toml` (W-LOG).
- The pyro firmware (github.com/n9wxu/pyro_fw) at v2.2.8: `DECISIONS.md`
  DD-073, DD-074, DD-086, DD-098; `REQUIREMENTS.md` FLT-LOG-06;
  `docs/outstanding_tasks.md` L4, C6; `src/flash_op.c`.
- The user's statements quoted in section 1.
