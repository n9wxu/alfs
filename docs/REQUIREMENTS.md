# alfs requirements

alfs, the append-log flash store: a small filesystem of named, versioned
files on serial NOR flash, built for append-only logs.

**Status: draft 3, 2026-10-15.** Requirements come first, then the tests
that verify them (`docs/TEST_PLAN.md`), then the code. Draft 2 added what
the flash probe found and the changes of `docs/V2_STRATEGY.md`. Draft 3
gives files names and versions, makes alfs a typical filesystem served by
one task that does every flash operation, and drops resuming a file after
a reset. Requirements it changes say so, with the reason; those it retires
keep their numbers.

---

## 1. Purpose and priorities

The brief, 2026-10-14:

> "Clean sheet a focused flash filesystem optimized for append logging and
> preserving the flash memory. ... Use the pyro lessons as use-cases. Use
> the flash behavior data from littlefs. Requirements then tests then code.
> Prefer speed then long flash life then safety. A corrupted data block is
> less important than a broken exec loop. Simplicity will keep the code
> fast and flash overhead cheap."

And two rulings given the same day for the pyro flight computer, which
alfs adopts as its founding rules:

> "Fast writes require pre-erase. Before flight, we must ensure all unused
> flash is erased."
>
> "writing to a dirty sector is not allowed, we must only write to erased
> sectors. It does not matter if the write is metadata or document data.
> Erase before write adds significant delay to the append operations."

The user, 2026-10-15, on the shape of draft 3:

> "Give ALFS a freertos task for the flash ops. Writes collect in the
> writer function and then spill to the flash ops task when enough data is
> collected. Reads fetch from the flash ops task and return the data.
> Serializing in the flash ops task ensures no collisions and allows each
> operation to be simple loops."
>
> "files need filenames. The primary use will be append only logging, but
> config files are still present. All files will be accessed by name."
>
> "The user interface to ALFS is a typical file system. Files will be
> created, used, deleted as required. The application will handle
> rotations, deletions, erase, etc."
>
> On writes: "Write will block until the write is covered. The writer may
> not be a ram based system so XIP and writes are not allowed. ...
> Applications bring their ram ... A write will bring data and a ram block.
> The write will be placed in the ram block by alfs. If the ram block is
> full, it will be sent to the flash ops. When flash ops are over, the
> remaining data will be put in the ram block. If all data is consumed, the
> writer will return to the application."
>
> On config files: "add a version number metadata and 'replace' the whole
> file but with a version number bump. All versions appear in a directory
> listing. Delete is explicit on version numbers."

And three choices the user made the same day: erasing happens only when
the application asks; a version number is alfs's identity of the file,
never reused; after a reset the application starts a new version of a
file rather than resuming the old one.

**The order of priority.** Every requirement carries one:

1. **Speed (SPD):** no flash operation stalls the system for longer than
   one page program, and no write ever waits for an erase.
2. **Flash life (LIFE):** every byte programmed and every sector erased
   is one the files needed.
3. **Safety (SAFE):** what was synced survives power loss and faults, as
   far as 1 and 2 allow.

Where two requirements conflict, the higher priority wins. Two examples
the requirements below settle this way: a chunk a power cut tore is
abandoned, not repaired (SPD over SAFE); and a write that finds no ready
sector fails at once rather than erasing one (SPD-04 over SAFE-02).

**Simplicity (SIMP)** is how the three are met: less code runs faster,
writes less and has fewer ways to fail. Its requirements are budgets.

---

## 2. Scope

**In scope:**

- Files on raw serial NOR flash, found by name and version: create,
  write, sync, close, read sequentially, list, delete one version.
- Several files open at once (a flight log beside a high-rate log, a
  config file saved while a log is written).
- One task that performs every flash operation for the application's
  tasks, on FreeRTOS; and the same core, without the task, for a caller
  with no RTOS.
- Erasing when the application asks, and reporting how much is ready.
- Power loss at any moment; the bad-sector and power-loss behaviours of
  section 4.4.
- A region of one flash chip shared with executing code: alfs owns only
  the region it is given.

**Out of scope:**

- Writing into the middle of a file, truncating, renaming, directories. A
  name may hold a `/`, but no directory exists.
- Reopening a file to append to it. Draft 2 resumed a file after a reset
  (SAFE-04 as it then stood); draft 3 starts a new version instead, the
  user's choice of 2026-10-15.
- NAND, SD cards, eMMC: any medium with a write-back cache or its own
  translation layer. Their reordering (littlefs's OOO behaviour) is not
  modelled.
- A part whose prog unit is over one byte (one with error correction
  inside it): alfs relies on A-8 and A-9.
- Encryption and an unknown erase value.

---

## 3. Use cases

Each comes from the pyro flight computer (github.com/n9wxu/pyro_fw). The
references are to its `DECISIONS.md` (DD-nnn), its requirements
(`REQUIREMENTS.md`, FLT-LOG-nn) and its open tasks
(`docs/outstanding_tasks.md`).

| # | Use case | What pyro learned |
|---|---|---|
| UC-1 | **A flight log.** 22-byte rows at 1 to 50 a second, written by a task of a system whose flight loop runs every 20 ms, and synced every second. | Every flash program or erase on the RP2040 stops both cores (DD-074). littlefs 2.11.3 erased a block at every sync: 46 to 73 ms stops and 20 to 77 loop overruns a minute (DD-035, L2). Hence the rulings. |
| UC-2 | **A high-rate log.** Everything sensed, about 24 kB a second: 16.2 MB in a 668 s flight. | On MK1C-SD it goes to an SD card through a 32 kB RAM ring, into a file preallocated on the pad (DD-077). On NOR it is about 94 page programs a second. |
| UC-3 | **Restart after a reset in flight.** The board restarts and logs on into a new version of its log; the version cut short stays readable. | DD-086. The time from reset to the first sample is flight time lost. Changed 2026-10-15: draft 2 reopened the newest log and appended to it. |
| UC-4 | **Keep the newest flights.** Two, the oldest deleted by the application. | C6. |
| UC-5 | **Erase before flight.** On the bench, or paced on the pad; launch waits until it is done. | The rulings; C6's OK to LAUNCH gate. A 4 KB erase takes 35 to 45 ms typically and up to 800 ms (section 4.3). |
| UC-6 | **Read a file back.** Sequentially, for download, perhaps while another file is written. | `/api/flight.csv` renders the log as it reads. |
| UC-7 | **Power cut at any moment.** A battery lead lost at landing. | FLT-LOG-06: "no more than the last second shall be lost." |
| UC-8 | **The wrong geometry.** An image built for an 8 MB chip, run on a 2 MB one. | The flash wrapped, and the filesystem formatted over the bootloader (DD-098). |
| UC-9 | **Write failures.** A program that fails part-way. | A retry wrote part of a record twice and misaligned everything after it, and nothing counted the loss (L4). |
| UC-10 | **Unattended, soldered-in flash.** Replacing the chip loses everything; the unit may be out of reach. | From the littlefs work: degrade gracefully, shrink with bad sectors, stay readable at end of life, report health. |
| UC-11 | **Configuration files.** Saved on the ground, each save a new version of the same name; every version listed; the application deletes the old ones. | The user, 2026-10-15. pyro keeps its configuration and web pages in littlefs today. |
| UC-12 | **Many tasks, one flash.** The flight log from a storage task, configuration and downloads from a network task, each blocking only itself. | pyro runs FreeRTOS on both cores (DD-073); its flash operations go through one routine that stops both cores for one operation at a time (DD-074, `src/flash_op.c`). |

---

## 4. The flash model

### 4.1 Terms

- **region:** the sector-aligned range of the chip alfs is given.
- **sector:** the erase unit, a power of two (4,096 bytes on every part
  here).
- **page:** the most one program operation writes; a program never
  crosses a page boundary (256 bytes here).
- **prog unit:** the smallest unit the flash programs on its own. alfs
  requires one byte (A-2, A-8, A-9).
- **flash operation:** one call to the driver's program or erase. Reads are
  not operations: they do not take the chip out of execute-in-place.
- **stall:** the time one flash operation stops a system that executes
  from the same chip: on the RP2040, every core (section 4.3).
- **file:** a sequence of bytes written once, from its start, with a name
  and a version.
- **name:** 1 to 31 bytes the application chooses.
- **version:** the number alfs gives a file when it is created; with the
  name, it finds the file.
- **closed:** a file whose writer closed it: its size is recorded and it
  takes no more bytes.
- **block:** RAM the application gives with a file, where alfs gathers the
  bytes written until they fill it, and the bytes read.
- **chunk:** the bytes of one file that one program adds, checked as a
  unit.
- **sync:** a call after which every byte written so far is durable.
- **durable:** survives a power loss at any later moment.
- **flash task:** the one task that performs every flash operation of an
  instance (draft 3).
- **request:** what a call in the application's task sends the flash task,
  and waits on until it is done.
- **ready sector:** a sector whose erase completed and which nothing has
  written since: the only kind a write may take (the rulings).
- **dirty sector:** a sector that must be erased before it can be ready:
  it holds a deleted file, an interrupted erase, or anything not alfs's
  own.
- **bad sector:** one that fails its erase or programs (4.4).

### 4.2 Assumptions about the flash

| # | Assumption | Source |
|---|---|---|
| A-1 | A program moves bits only away from the erase value; only an erase moves them back. | Every NOR part |
| A-2 | A program of fewer bytes than a page, into erased bytes, leaves the page's other bytes as they were, and a page may be programmed so at least 16 times between erases. | All four datasheets (`docs/datasheets/README.md`); none states a limit. The probe programmed pages 2,048 times (A-8) |
| A-3 | A program interrupted by power loss changes no byte outside its own page. | The littlefs fork assumes the same of a prog (`lfs3.h`, `gc_preerase_count`) |
| A-4 | An erase interrupted by power loss changes no byte outside its own sector, and may leave that sector in any state, including one that reads as erased but holds bits that read randomly. | littlefs's METASTABLE behaviour (4.4) |
| A-5 | Operations complete in the order issued; there is no write-back cache. | Raw serial NOR |
| A-6 | The erase value is known and fixed: 0xFF on every part here. | The datasheets |
| A-7 | A read can fail, as at low supply voltage, and succeed when retried. | The littlefs work's error-handling principle |
| A-8 | A program into bytes already programmed clears exactly the bits that are 0 in its data and changes no other bit of the page. A page takes at least 2,048 programs between erases. | No datasheet states it: the Winbond parts' Page Program writes "previously erased (FFh) memory locations" (W25Q16JV p. 34, W25Q128JV p. 37), and the XT25F128F and BY25Q64ES sheets say nothing. Measured by the probe on 2026-10-14 (`docs/FLASH_PROBE.md`): it held on the W25Q128JV, two XT25F128F and two BY25Q64ES; the W25Q16JV is not yet tested |
| A-9 | A program that a power cut interrupts never clears a bit its data left 1. Programming the intended bytes again over it leaves them reading as intended on every later read, and changes no other byte. | No datasheet states it. Measured by the probe's P8, which interrupts programs with the chips' software reset (66h then 99h; W25Q128JV p. 60, XT25F128F p. 41, BY25Q64ES p. 30) as a stand-in for a power cut (`docs/FLASH_PROBE.md` section 4). Held on the W25Q128JV and two XT25F128F, 2026-10-14, including bits torn into reading either way; the BY25Q64ES finishes an operation despite the reset, so is untested, as is the W25Q16JV |

A-8 and A-9 are measured, not read. A part with error correction inside
the chip breaks them, so a new part is probed before alfs runs on it.

A-9 lets alfs complete a program a power cut may have left half done, by
programming the same bytes again: a sector's stamp, each time a claim is
programmed (SAFE-04); a dead mark before its file is reclaimed (SAFE-11);
a close record before an older version of its name is deleted (SAFE-14).

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
26 ms (BY25Q64ES) and 47 ms (W25Q128JV). The requirements keep the
datasheets' figures.

### 4.4 Faults

Power-loss behaviours, from the littlefs fork's emulated block device
(`REQUIREMENTS.md` 5.5 at `v3-integration`; `bd/lfs3_emubd.h`), with
FLICKER added. The interrupted operation:

| Name | Program | Erase |
|---|---|---|
| ATOMIC | is not applied | is not applied |
| SOMEBITS | one bit of its first byte that its data clears is programmed | the old data stays and one bit flips |
| MOSTBITS | is applied, then one bit it cleared reads erased again | is applied, then one bit anywhere flips |
| METASTABLE | is applied, then one bit it cleared reads randomly until the sector is erased or a program clears it | is applied, then one bit of the sector reads randomly until erased |
| TORNTAIL | its first byte stays as it was, a run of random length after it is programmed, and one bit the run cleared reads erased again | is atomic |
| FLICKER | is not applied, but one bit its data clears reads randomly | is not applied, but one bit of the sector reads randomly |

FLICKER is not littlefs's: it is the state the probe left on the
XT25F128F by aborting a program early, a page that reads erased but for a
bit that reads either way (`docs/FLASH_PROBE.md` section 4).

Every torn program moves only bits its data moves (A-9). Amended
2026-10-15: SOMEBITS, MOSTBITS and TORNTAIL as littlefs defines them flip
any bit, including one the data left as it was. A program cannot do that
on NOR flash, and since alfs programs a sector's stamp again with each
claim, those flips lost synced data in 5 of 100 test seeds; limited to
bits the data moves, none did.

littlefs's sixth, OOO (writes since the last sync revert), needs a write
cache and is out of scope (A-5).

Bad-sector behaviours, from the same device: PROGERROR (a program fails and
writes nothing), ERASEERROR (an erase fails and erases nothing), READERROR
(reads fail), PROGNOOP (a program silently does nothing), ERASENOOP (an
erase silently does nothing, and programs do nothing), PROGFLIP (a program
lands with one bit wrong), READFLIP (reads flip one bit half the time).
And wear-out: a sector goes bad after a number of erases.

Faults the littlefs device could not inject, which alfs's tests must:
a torn erase that reads as erased (A-4), a read that fails once then
succeeds (A-7), and a failure confined to some offsets of a sector.

### 4.5 Workloads

Measurements use these, on the W25Q128JV's geometry (4,096-byte sectors,
256-byte pages) with an 8 MiB region, timed with the typical figures of
4.3, and with a block of one page unless a workload says otherwise.

- **W-LOG:** littlefs's logging workload, so the figures compare with its
  (the fork's `benches/bench_wlog.toml`): 22-byte rows produced at 1 or 50 a
  second, written every 200 ms, synced every second, for 10 minutes.
- **W-HR:** 480 bytes written every 20 ms (24,000 a second), synced every
  second, for 10 minutes.
- **W-RING:** W-LOG at 50 rows a second, a new version of the log every 10
  minutes, and the oldest deleted whenever fewer than two logs' worth of
  sectors are ready; one erase step requested every 20 ms; until every
  sector has been erased at least 10 times.
- **W-CFG:** a configuration file of 300 bytes saved as a new version,
  then every older version of it deleted, again and again (UC-11).

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

Nothing erases but an erase the application asks for (`alfs_erase`) and
format.

- Priority: 1.
- Source: the rulings; UC-1; the user, 2026-10-15 (erase only when asked).
- Measure: erases the simulator counts during each call, under W-LOG,
  W-HR and W-RING, with power loss at every operation and every bad-sector
  behaviour.
- Pass: zero in every call other than those two. Fail: any.
- Verified by: `spd_no_erase_outside_erase`.

#### ALFS-SPD-02

A write, sync or close that programs X bytes of a file performs at most
ceil(X / (page − 6)) + 2 programs, plus one for each sector it moves into,
one for each failed program it retries (SAFE-06), and one to zero the
fresh sector (SAFE-04 rule 3). A write programs bytes only when the block
fills, and then all but less than a page's room: X is at most the block's
size B plus the bytes the write passes.

- Priority: 1.
- Source: UC-1, UC-2; the stall of 4.3. Amended 2026-10-15: draft 2 did one
  operation a call and left the pacing to the caller; draft 3's write
  blocks until its bytes are covered (the user's words, section 1), so it
  bounds the burst instead. The application chooses B to fit its slack: a
  sync of a full 4,096-byte block is about 20 programs, 8 ms at the
  typical time.
- Measure: programs the simulator counts during each call, against the
  bytes the call took into and left in the block, with writes of 480 and 22
  bytes and blocks of one page and of 4,096 bytes.
- Pass: within the bound. Fail: more.
- Verified by: `spd_write_burst_bounded`.

#### ALFS-SPD-03

Every program is of one page or less, at a page-aligned boundary or within
one page.

- Priority: 1.
- Source: 4.1; a page program is the shortest stall (4.3).
- Measure: the simulator rejects a program that crosses a page.
- Pass: none crosses. Fail: any.
- Verified by: every test (the simulator asserts it).

#### ALFS-SPD-04

A write or sync that finds no ready sector returns `ALFS_ENOSPC`, keeps
the bytes it holds in the file's block, and erases nothing.

- Priority: 1.
- Source: the rulings.
- Measure: a file written until no sector is ready.
- Pass: `ALFS_ENOSPC`, no erase, and after an erase makes a sector ready
  the bytes held are written. Fail: an erase, or a lost byte. Amended
  2026-10-15: draft 2 also required no flash operation in that call; a
  draft 3 write may already have programmed part of its bytes.
- Verified by: `spd_enospc_without_erase`.

#### ALFS-SPD-05

Retired 2026-10-15. It said alfs never waits and passes on a driver's
`ALFS_BUSY` from a suspended erase. In draft 3 each request is a loop that
returns when its flash work is done (the user's words, section 1), and a
driver that slices an erase does so inside its erase (IF-01).

#### ALFS-SPD-06

Mounting reads at most 60 bytes of each sector of the region, and performs
no flash operation.

- Priority: 1.
- Source: UC-3.
- Measure: bytes the simulator reads during `alfs_mount`.
- Pass: at most 60 times the number of sectors. Fail: more, or any
  operation. Amended 2026-10-14 from 60 to 68 for version 2's stamp floor
  and sector number; 2026-10-15 back to 60: version 3's claim drops the
  4-byte tag (names replace it) and holds 16 bytes.
- Verified by: `spd_mount_reads_bounded`.

#### ALFS-SPD-07

Retired 2026-10-15 with reopening a file to append (section 2).

#### ALFS-SPD-08

On W-LOG and W-HR no flash operation stalls longer than one page program.

- Priority: 1.
- Source: UC-1, UC-2; littlefs v2.11.3's 96.6 ms and v3's 49.6 to 53.9 ms
  on W-LOG (the fork's `REQUIREMENTS.md`, Appendix B.1). The user,
  2026-10-14: "≤1ms writes is OK so the chunk size can be larger". A page
  program measured at most 0.46 ms on the chips (`docs/FLASH_PROBE.md`),
  so chunks stay up to a page; the datasheets' maximums (2 to 3 ms) are
  above 1 ms. Amended 2026-10-15: each operation, where draft 2 measured
  each call, which was then one operation.
- Measure: simulated time of each operation, at the typical times of 4.3.
- Pass: at most 0.4 ms (one W25Q128JV page program). Fail: more.
- Verified by: `spd_wlog_longest_stall`.

#### ALFS-SPD-09

The processor time of a write, sync or read, apart from its flash
operations, grows only with the bytes the call passes, plus a constant.

- Priority: 1.
- Source: UC-1.
- Measure: on the host, bytes read and processor time per call against
  bytes passed, on regions of 32 and 2,048 sectors; on an RP2040 at
  125 MHz, the time of a write and sync of 256 bytes.
- Pass: on the RP2040, at most 100 us for 256 bytes; on the host, no term
  in region size. Fail: otherwise. The exceptions read each sector's
  header once: create (which finds a ready sector), opening by name,
  listing, delete, and a write that takes a new sector after a mount.
- Verified by: `spd_cpu_linear` (host); `target_write_256` (RP2040,
  deferred until a target build exists).

#### ALFS-SPD-10

Reading on from one sector of a file to its next reads the headers of
at most the sectors between them, in the direction alfs allocates, and
the next one's.

- Priority: 1.
- Source: UC-6; v1 read every header in the region at each sector change
  of a file written alongside another (`docs/V2_STRATEGY.md` 4.2).
- Measure: headers read by `alfs_read` at each sector change, reading back
  one of two files written together, and of one file written alone.
- Pass: at most the sectors between, plus one. Fail: more.
- Verified by: `spd_read_next_bounded`.

#### ALFS-SPD-11

Every loop alfs runs ends within a bound set by the region's sector count,
the bytes the call passes, or the configured retries, whatever the flash
holds.

- Priority: 1.
- Source: draft 3 makes each operation a loop (section 1), and a loop whose
  end depends on what the flash reads is a hung task when the flash is
  damaged. "A corrupted data block is less important than a broken exec
  loop."
- Measure: every call made on fuzzed regions (SAFE-10); review of each
  loop's bound.
- Pass: every call returns. Fail: any that does not.
- Verified by: `safe_fuzz_mount_read`; review.

### 6.2 Flash life (LIFE)

#### ALFS-LIFE-01

alfs erases a sector only when it is dirty, never one that is ready.

- Priority: 2.
- Source: the brief ("preserving the flash memory").
- Measure: under W-RING, each erase is classified by the sector's state
  before it.
- Pass: every erase was of a dirty sector. Fail: any of a ready one.
- Verified by: `life_erase_only_dirty`.

#### ALFS-LIFE-02

The bytes programmed are at most the bytes written, plus 6 bytes a
program, plus 72 bytes for each sector a file takes, plus 128 bytes a
file.

- Priority: 2.
- Source: the brief; littlefs v3 programs about 768 bytes of metadata for a
  16-byte append and sync with prog unit 256 (the fork's Appendix B.2).
  Amended 2026-10-15: a sector costs its stamp (26 bytes) and a claim
  program that writes the stamp again with the dead mark and the claim
  (43); a file's first sector adds its name, its close record and the marks
  programmed again (SPEC section 1). Draft 2's bound counted prog units
  over one byte.
- Measure: bytes the simulator programs, against the bound computed from
  its own counts of programs, sectors and files, under W-LOG (22 bytes a
  sync at 1 row a second, 1,100 at 50) and W-HR (24,000).
- Pass: within the bound. Fail: above it.
- Verified by: `life_write_amplification`.

#### ALFS-LIFE-03

On W-LOG, alfs erases no more sectors than the bytes it programmed fill.

- Priority: 2.
- Source: littlefs v2.11.3 erases 58.7 to 80.7 sectors a minute on W-LOG,
  v3 2.8 to 48.5 (the fork's Appendix B.1).
- Measure: erases over 10 minutes against the bytes programmed divided by
  the sector size, after the log is deleted and every sector erased.
- Pass: erases at most the sectors filled, plus one. Fail: more.
- Verified by: `life_wlog_erases`.

#### ALFS-LIFE-04

Wear is level: under W-RING, the erase counts of the good sectors differ
by at most 2.

- Priority: 2.
- Source: UC-10; 100K-cycle endurance (4.3).
- Measure: each sector's erase count at the end of W-RING.
- Pass: largest minus smallest at most 2. Fail: more.
- Verified by: `life_wear_level`.

#### ALFS-LIFE-05

Each sector's erase count survives its erase, so wear is known after any
number of reboots.

- Priority: 2.
- Source: UC-10.
- Measure: erase counts reported after W-RING and a remount, against the
  simulator's own counts.
- Pass: equal, except a sector whose erase a power loss separated from the
  recording of its count, which takes the largest count in the region: at
  least its true count minus one while wear is level (LIFE-04). Fail:
  otherwise.
- Verified by: `life_erase_count_persists`.

#### ALFS-LIFE-06

A page is programmed at most a configured number of times between erases
(default 16); a chunk that would exceed it starts the next page.

- Priority: 2.
- Source: A-2: no datasheet here limits partial programs, but other NOR
  parts do. Amended 2026-10-15: a file's first page holds no data, and
  takes four programs and at most two more in each mount (SPEC 1.5), so no
  program of it is held back for its marks.
- Measure: programs per page the simulator counts, under W-LOG at 1 row a
  second.
- Pass: at most the configured number. Fail: more.
- Verified by: `life_partial_program_cap`.

#### ALFS-LIFE-07

Where a driver offers a larger erase (64 KB), an erase uses it for a run
of dirty sectors that fills one aligned block.

- Priority: 2 (SHOULD).
- Source: UC-5: a 64 KB erase is about 6 times faster than 16 sector
  erases (4.3; `rp2040-datasheet_2025-02-20.pdf` p. 135).
- Measure: erases on a region whose every sector is dirty.
- Pass: one large erase per aligned block. Fail: sector erases there.
- Verified by: `life_large_erase`.

#### ALFS-LIFE-08

Retired 2026-10-15 with reopening a file to append (section 2). It bounded
the bytes a resume left unused. A reset now leaves the rest of the
unclosed file's last sector unused until the file is deleted: at most one
sector.

### 6.3 Safety (SAFE)

#### ALFS-SAFE-01

Every byte a successful sync or close made durable reads back intact after
a power loss at any later operation, under every power-loss behaviour of
4.4.

- Priority: 3.
- Source: UC-7.
- Measure: W-LOG and W-HR with power lost after operation k, for every k
  (littlefs's `-Plinear`), and for a sample of pairs; remount; read every
  file.
- Pass: every synced byte reads back, in order. Fail: any missing or wrong.
- Verified by: `safe_powerloss_durable`.

#### ALFS-SAFE-02

A power loss loses at most the bytes written since the last successful
sync.

- Priority: 3.
- Source: UC-7, FLT-LOG-06.
- Measure: as SAFE-01, counting bytes lost.
- Pass: at most those written since the last sync. Fail: more.
- Verified by: `safe_powerloss_bound`.

#### ALFS-SAFE-03

A read never returns a byte from a chunk whose check failed. A failed
check makes the chunk's length untrustworthy too, so the read skips to the
next page, counts one failure, and reads on.

- Priority: 3.
- Source: UC-7; METASTABLE and the FLIP behaviours.
- Measure: files with bits flipped in chunk headers, payloads and checks.
- Pass: no damaged byte returned; one failure counted for each page
  skipped; the chunks of the pages after it read. Fail: otherwise.
- Verified by: `safe_no_false_data`.

#### ALFS-SAFE-04

After a mount, nothing synced shares a page with bits a power loss may
have left half programmed, and no claim or stamp can decay into
reclaiming what was synced:

1. alfs never writes again to a file a mount found unclosed: the
   application starts a new version (section 2).
2. Every program of a claim starts at the sector's first byte and programs
   the stamp again with the bytes it reads (A-9).
3. After a mount, the first sector after the sector of the newest valid
   claim whose stamp is current and whose claim is neither valid nor zero,
   as the mount found them, is not used: when an allocation reaches it,
   alfs programs 00h over its claim, and an erase reclaims it.

- Priority: 3.
- Source: A-3, A-9; METASTABLE and FLICKER (4.4). The interrupted program
  was the next chunk of a file, a claim, or a stamp. Rule 2: a stamp that
  an erase's power loss left half programmed can read valid at one mount
  and invalid at a later one. Rule 3: a claim program torn at its start
  leaves a sector that reads ready but holds a bit that reads either way;
  it is the first ready sector from where the allocation cursor stood,
  after the newest claim. Amended 2026-10-15: rules 1 to 3 of draft 2
  (programming a resumed file's claim again, skipping two pages, clearing
  claims ahead) went with resuming; rule 1 is new, and draft 2's rules 4
  and 5 are now 2 and 3. Seeds other than the suite's one fixed seed found
  the resume reading its last claim twice and building on the second read
  (2026-10-15): a fault that went with it.
- Measure: the simulator flags a program of a byte an interrupted
  operation changed (every behaviour but ATOMIC), unless it is given what
  the programs before it meant, or 00h; the power-loss runs remount, start
  a new version, write, and read again, with erases between.
- Pass: no flag, and every read as SAFE-01 requires. Fail: otherwise.
- Verified by: `safe_no_program_after_torn`, `safe_powerloss_durable`.

#### ALFS-SAFE-05

alfs writes only into a sector whose erase is known to have completed and
whose erased state it verified on the ground.

- Priority: 3.
- Source: the rulings; A-4; ERASENOOP. The probe's P9: an erase aborted at
  a tenth (W25Q128JV) or half (XT25F128F) of its time already reads blank,
  so a blank check alone cannot show an erase completed; the stamp,
  programmed after the erase returns done, does (`docs/FLASH_PROBE.md`
  section 4).
- Measure: power loss during erases, and ERASENOOP sectors.
- Pass: no write lands in a sector whose erase was interrupted or did
  nothing. Fail: any.
- Verified by: `safe_write_only_verified_ready`.

#### ALFS-SAFE-06

A failed program loses none of the bytes it carried: in the same call they
are written to a fresh sector, since a page that fails is a sector that is
failing. After a configured number of failures in a row (default 3) they
are dropped and counted. A program is checked by reading it back.

- Priority: 3.
- Source: UC-9. Amended 2026-10-15: draft 2 retried in a later call, to
  keep one operation a call (SPD-02 as it then stood).
- Measure: PROGERROR, PROGNOOP and PROGFLIP sectors in the path of W-LOG.
- Pass: every byte is written or counted as lost; nothing is written twice.
  Fail: otherwise.
- Verified by: `safe_prog_failure`.

#### ALFS-SAFE-07

A sector that fails an erase, or its check after one, is never used again,
and the region's capacity shrinks by it.

- Priority: 3.
- Source: UC-10.
- Measure: ERASEERROR and ERASENOOP sectors, and wear-out, under W-RING.
- Pass: excluded and counted; after a remount it is dirty again, and its
  next erase finds it bad again before anything is written to it. Marking
  it on the flash would mean programming bytes already programmed. Fail: a
  write or a stamp lands in it.
- Verified by: `safe_bad_sector`.

#### ALFS-SAFE-08

A failed read is returned to the caller, with the file's position and
every other state unchanged, so that a retry reads the same bytes.

- Priority: 3.
- Source: A-7.
- Measure: a read that fails once, in mount, open and read.
- Pass: the error, then the right bytes on retry. Fail: otherwise.
- Verified by: `safe_read_retry`.

#### ALFS-SAFE-09

alfs never writes a region it has not formatted: a mount of a region with
no alfs sector, or whose geometry differs from the caller's, fails and
writes nothing.

- Priority: 3.
- Source: UC-8.
- Measure: regions holding random data, another filesystem, a region
  formatted for another sector size or region size; a region larger than
  the chip.
- Pass: `ALFS_ENOFS` or `ALFS_EGEOMETRY`, and no operation. Fail: any
  write.
- Verified by: `safe_foreign_region`.

#### ALFS-SAFE-10

Every call finishes, without a crash, on any content of the region.

- Priority: 3.
- Source: UC-10; littlefs's crafted images. Amended 2026-10-15: draft 2
  named mount and read; draft 3's calls are loops (SPD-11), so every call
  is held to it.
- Measure: fuzzed regions (random sectors, valid headers with random
  fields), under a sanitizer: mount, list, open, read, create, write,
  close, delete and erase.
- Pass: every call returns; no sanitizer error. Fail: otherwise.
- Verified by: `safe_fuzz_mount_read`.

#### ALFS-SAFE-11

A delete is atomic: once it returns, the version is gone after any power
loss; before it returns, the version is whole. An erase reads each sector
again just before erasing it, deciding from one read of its file's dead
mark: a sector that now reads live is left for later, and before any
sector of a deleted file is erased the file's dead mark is programmed
again with 00h (A-9), so a mark a power loss left half programmed cannot
read as live once the reclaim has begun.

- Priority: 3.
- Source: UC-4; METASTABLE: a half-programmed mark could read deleted at
  one mount and live at the next, after some sectors were erased. Amended
  2026-10-14 (`docs/V2_STRATEGY.md` 4.4). The second read: a torn claim
  that read invalid when the erase chose its sector, and valid when it
  read again, made a live file look deleted, and the mark programmed
  deleted it (found by `safe_powerloss_bound`).
- Measure: power loss at every operation of a delete; remount; then power
  loss at every operation of the erases that reclaim it; remount; for
  several simulator seeds, since one seed hid a fault here (2026-10-15).
- Pass: the version is whole or gone, never partial. Fail: partial.
- Verified by: `safe_delete_atomic`, `safe_delete_reclaim`.

#### ALFS-SAFE-12

A file's sectors are numbered in order. A number missing when reading is
counted as one failed check, and reading goes on with the next sector
present.

- Priority: 3.
- Source: UC-6; v1 passed over a lost sector silently
  (`docs/V2_STRATEGY.md` 4.2).
- Measure: a sector of a file made dirty, then the file read.
- Pass: every other sector's chunks read, in order, and one failure
  counted. Fail: otherwise.
- Verified by: `safe_missing_sector`.

#### ALFS-SAFE-13

A create is atomic: after a power loss during it, the new version is
absent, or present and empty. One whose name a power loss left half
programmed lists without a name, and is deleted by its version.

- Priority: 3.
- Source: UC-11; draft 3 programs a file's first sector when it is created,
  so that an empty file exists (SPEC 3).
- Measure: power loss at the create's program under each behaviour; eight
  remounts.
- Pass: no version is listed with a name other than the one created, none
  holds bytes, and every later create gets a new version (IF-07). Fail:
  otherwise.
- Verified by: `safe_create_atomic`.

#### ALFS-SAFE-14

A close is atomic: after a power loss during it, the file is unclosed or
closed with its true size. Before deleting a version, alfs completes the
close record of the newest version of the same name above it whose close
record is not erased (A-9), so a close a power loss left half programmed
cannot read unclosed once an older version is gone. A close whose record
will not program releases the file unclosed.

- Priority: 3.
- Source: UC-11. A close record torn by METASTABLE can read closed at one
  mount and unclosed at the next; had the application seen it closed and
  deleted the version before, a later mount would find no closed version
  of its configuration. First written as "the newest closed version": the
  delete's own read of the record could find it unclosed after the
  application's listing found it closed (`safe_config_replace`, amended
  2026-10-15). A close record's place is fixed in the file's first
  sector, so one that fails cannot move to a fresh sector (SAFE-06).
- Measure: power loss at every operation of W-CFG; after each, eight
  remounts.
- Pass: whenever a closed version of the file existed before the loss,
  opening its newest closed version (IF-09) reads a whole configuration
  after every remount. Fail: otherwise.
- Verified by: `safe_close_atomic`, `safe_config_replace`.

### 6.4 Simplicity (SIMP)

#### ALFS-SIMP-01

The core is one C99 source file and one header (`alfs.c`, `alfs.h`), with
no dynamic memory, no recursion, no floating point, and no library beyond
`<stdint.h>`, `<stddef.h>`, `<stdbool.h>` and `<string.h>`. The task layer
is one more of each (`alfs_rtos.c`, `alfs_rtos.h`), which adds only the
FreeRTOS kernel's headers.

- Priority: S.
- Source: amended 2026-10-15 for the flash task; the core stays usable
  without an RTOS (section 2).
- Measure: the build and a review.
- Pass: as stated. Fail: otherwise.
- Verified by: `make target` (compiles `alfs.c` alone, with
  `-std=c99 -Wall -Wextra -Werror -pedantic`), `make target-rt`; review.

#### ALFS-SIMP-02

The core is at most 7 KiB on a Cortex-M0+ (Thumb, `-Os`), under half of
littlefs's, and the task layer at most 1.5 KiB.

- Priority: S.
- Source: the brief ("Simplicity will keep the code fast and flash
  overhead cheap"). littlefs 2.11.3 built the same way (`-Os`,
  `-mcpu=cortex-m0plus`, its debug, warning, error and assert output off,
  no malloc) is 15,456 bytes. Draft 1 guessed 4 KiB before any code
  existed; the first build was 5.4 KiB, 5.3 after its duplication was
  removed. Amended 2026-10-14 from 6 KiB: version 2 was 6,845 bytes, 1,544
  more than version 1, nearly all for the rules that settle what a power
  loss half programmed (measured 2026-10-15: 1,508 bytes). Draft 3 removes
  resuming (about 800 bytes), the erase-step state machine and prog units
  over one byte, and adds names, versions and the close record. The task
  layer's figure is an estimate until it is built.
- Measure: `arm-none-eabi-size` of `alfs.o` and `alfs_rtos.o`, text plus
  data; the task layer compiled against the kernel's generic Cortex-M0
  port.
- Pass: at most 7,168 and 1,536 bytes. Fail: more.
- Verified by: `make target`, `make target-rt`.

#### ALFS-SIMP-03

RAM does not grow with the region, and all of it is the caller's: an
instance is at most 96 bytes and a scratch page; a file handle at most 48
bytes and its block, of at least one page; the task layer, the task's
stack and control block, the queue's storage, and for each request about
100 bytes on the calling task's stack.

- Priority: S.
- Source: pyro's Lua boards have about 13.5 kB of SRAM spare (measured
  2026-10-14, pyro's `docs/flash_stall_options.md` section 6.3), and a
  region of 8 MiB must not cost more than a small one. The user,
  2026-10-15: "Applications bring their ram so ram allocations in ALFS
  should not be an issue." Amended 2026-10-15: draft 2's stream was 32
  bytes and a buffer of a page plus 53; a file handle now keeps a list
  link and its size, and the scratch page lets a block be any size.
- Measure: `sizeof` of the instance and handle structures, blocks and
  scratch excluded.
- Pass: at most 96 and 48 bytes. Fail: more.
- Verified by: `simp_ram`; `make target`.

#### ALFS-SIMP-04

No core call uses more than 320 bytes of stack, and the flash task's
deepest chain no more than 512 bytes, the driver's own frames apart.

- Priority: S.
- Source: draft 1 guessed 256 before any code existed; the first build's
  deepest chain was 292 bytes. pyro's smallest task stack is 2 kB.
  Amended 2026-10-15 for the flash task, whose stack holds the core's
  deepest chain under the task's loop.
- Measure: the deepest chain of frames under each public call and under
  the task's function, from GCC's call graph (`-fcallgraph-info=su`) on
  the Cortex-M0+ build. A host build's stacks say nothing here: the
  FreeRTOS POSIX port runs each task on a thread.
- Pass: at most 320 and 512. Fail: more.
- Verified by: `make target`, `make target-rt`.

#### ALFS-SIMP-05

The on-disk format is specified, every byte, in `docs/SPEC.md`, and has one
version.

- Priority: S.
- Measure: review against the code.
- Pass: every field the code reads or writes is in the spec. Fail: any not.
- Verified by: review.

### 6.5 Interface (IF)

#### ALFS-IF-01

The driver is four functions: read bytes, program bytes within one page,
erase one sector, and an optional larger erase. Each returns when its work
is done, or fails; a driver that must wait for the system to allow a flash
operation waits, and fails only on a fault of the flash. A read sees every
program and erase before it.

- Priority: S.
- Source: amended 2026-10-15: draft 2's erase could return `ALFS_BUSY` to
  be called again (SPD-05, retired). pyro's flash routine refuses an
  operation it cannot start within 500 ms (`src/flash_op.c`); to alfs a
  refusal is a failed program, and the sector would be abandoned.
- Verified by: the simulator implements exactly these.

#### ALFS-IF-02

The geometry is configured: sector and page size (powers of two, page at
least 128 bytes, sector at least four pages), the region's start and length
(sector-aligned), the chip's size, and the erase value. alfs programs a
byte at most once between erases, except to settle what a power loss may
have left half programmed (A-8, A-9): a sector's stamp again with each
claim, a dead mark again with 00h, a close record again with the bytes it
reads, and 00h over a claim (SAFE-04 rule 3).

- Priority: S.
- Source: amended 2026-10-15: draft 2 configured a prog unit up to a page
  and settled nothing when it was over one byte; draft 3 requires one byte
  (section 2). The page holds a file's first sector's records (SPEC 1).
- Verified by: `if_geometry` (every combination the tests use is
  accepted; misaligned or oversized ones are refused); every test (the
  simulator refuses a program into a byte already programmed unless it is
  the byte its programs meant, or 00h).

#### ALFS-IF-03

The on-disk format is little-endian and read and written byte by byte, so
any host reads any image.

- Priority: S.
- Verified by: `if_endian` (an image written on the host reads back under
  a big-endian build).

#### ALFS-IF-04

A file is found by its name and its version.

- Priority: S.
- Source: the user, 2026-10-15: "All files will be accessed by name."
  Amended 2026-10-15: draft 2 found a stream by an identity and a 32-bit
  tag the caller chose.
- Verified by: `if_names`, `if_list_files`.

#### ALFS-IF-05

The driver programs exactly the bytes alfs passes it, and no others. On
the BY25Q64ES a page padded with the erase value takes 3.2 times as long as
its 16 programmed bytes sent alone (`docs/FLASH_PROBE.md` section 3).

- Priority: S.
- Source: the probe, 2026-10-14; pyro's flash path programs whole pages
  (`src/littlefs_driver.c`, `prog_size = FLASH_PAGE_SIZE`).
- Verified by: review of each driver; the simulator counts the bytes of
  each program.

#### ALFS-IF-06

A name is 1 to 31 bytes, none of them 00h, compared byte for byte. A `/`
is an ordinary byte.

- Priority: S.
- Source: the user, 2026-10-15: "files need filenames." pyro's longest
  littlefs path today is 15 bytes (`/www/index.html`).
- Verified by: `if_names` (names of 1 and 31 bytes; 0 and 32 bytes, and one
  holding 00h, refused).

#### ALFS-IF-07

A file's version is the identity alfs gives it at create: unique for the
life of the region, growing with each create of any name, and never used
again, even after a delete or a create a power loss tore.

- Priority: S.
- Source: the user, 2026-10-15 (versions as global identities, never
  reused). A version counted per name would be used again after its
  newest version was deleted, and twice after a torn create read invalid
  at one mount and valid at a later one. The identity has both covered:
  each stamp records the next sequence, and a mount takes two more than
  the largest in any claim or stamp, three when it finds a torn claim
  (SPEC 1.1). Two alone gave a torn create's version again when the create
  came straight after a mount (found 2026-10-15, seeds 16 and 30).
- Verified by: `if_version_never_reused`.

#### ALFS-IF-08

Listing reports every version of every file, in growing version order:
its name, its version, whether it is closed, and its size, which is
unknown until it is closed.

- Priority: S.
- Source: the user, 2026-10-15: "All versions appear in a directory
  listing." The size of an unclosed file would need every chunk read.
- Verified by: `if_list_files`.

#### ALFS-IF-09

Opening a file to read with version 0 opens the newest closed version of
its name; with a version, that version, closed or not.

- Priority: S.
- Source: UC-11: the configuration a power loss interrupted mid-save is
  not the one to load; UC-3: the log a reset cut short is read by its
  version.
- Verified by: `if_open_version0`.

#### ALFS-IF-10

A delete removes one version, named by its name and version. It is refused
while any handle has that version open, and mount and format are refused
while any handle is open.

- Priority: S.
- Source: the user, 2026-10-15: "Delete is explicit on version numbers."
  A reader of a deleted version could read on into a sector erased and
  taken by another file.
- Verified by: `if_delete_open_refused`.

#### ALFS-IF-11

A file takes bytes only through the handle that created it, until that
handle closes it.

- Priority: S.
- Source: section 2 (no reopening to append); the user's "replace the whole
  file but with a version number bump".
- Verified by: `if_closed_read_only`.

#### ALFS-IF-12

A write copies the caller's bytes into the file's block; when the block is
full, its bytes are programmed, all but less than a page's room; the write
returns when every byte it was given is in flash or in the block. A sync
programs what the block holds.

- Priority: S.
- Source: the user, 2026-10-15, quoted in section 1. Keeping the last part
  of a page in the block packs the file into whole-page chunks between
  syncs.
- Verified by: `basic_round_trip`, `spd_write_burst_bounded`.

### 6.6 Observability and errors (OBS)

#### ALFS-OBS-01

alfs reports, at any moment: sectors ready, dirty and bad; bytes writable
without an erase; the least and greatest erase counts; and its counts of
programs, erases, failed programs, failed erases, failed checks and bytes
lost.

- Priority: 3.
- Source: UC-5 (the gate), UC-9, UC-10.
- Verified by: `obs_status` (each count against the simulator's).

#### ALFS-OBS-02

Every error a call can return is documented with the state it leaves and
what the caller should do: retry, rebuild (format), or fail.

- Priority: 3.
- Source: the littlefs work's error-handling principle (UC-10).
- Verified by: review of `alfs.h` and `alfs_rtos.h`.

#### ALFS-OBS-03

alfs reports whether any dirty sector remains, so the caller can hold a
launch until all unused flash is erased.

- Priority: 3.
- Source: UC-5; the first ruling.
- Verified by: `obs_clean` (false while a dirty sector exists, true once
  erases have cleared them all).

### 6.7 The flash task (RT)

Added 2026-10-15, from the user's words in section 1. The task layer
serves one instance of the core; every requirement above holds through
it.

#### ALFS-RT-01

One task performs every flash operation of an instance; the application's
tasks reach the flash only through requests to it, served one at a time.

- Priority: 1.
- Source: "Serializing in the flash ops task ensures no collisions and
  allows each operation to be simple loops."
- Measure: the test driver records the task each operation runs in, and
  whether two overlap.
- Pass: every operation in the flash task, none overlapping, under
  several writers, readers and erases at once. Fail: otherwise.
- Verified by: `rt_serialises`.

#### ALFS-RT-02

Every request blocks the task that made it until the flash task has done
it. A write returns when every byte it was given is in flash or in the
block; a read returns bytes the flash task fetched.

- Priority: 1.
- Source: the user's words on writes, section 1.
- Verified by: `rt_round_trip`, `rt_read_while_writing`.

#### ALFS-RT-03

A write whose bytes all fit in the block, without filling it, sends no
request; nor does a read served from what the block already holds.

- Priority: 1.
- Source: "Writes collect in the writer function and then spill to the
  flash ops task when enough data is collected."
- Measure: requests counted during writes smaller than the block's room.
- Pass: none. Fail: any.
- Verified by: `rt_fast_path`.

#### ALFS-RT-04

The task layer allocates nothing: the task's stack and control block, the
queue's storage and every request's memory are the caller's. A request
signals its completion with a semaphore on the caller's stack, never a
task notification.

- Priority: S.
- Source: pyro allocates statically and has no heap
  (`configSUPPORT_DYNAMIC_ALLOCATION` 0); its storage task waits on its own
  task notification, which alfs must not consume.
- Verified by: the build against a kernel configured with no dynamic
  allocation; review.

#### ALFS-RT-05

The flash task's priority and core affinity are the caller's; alfs changes
no task's priority.

- Priority: 1.
- Source: pyro's DD-074: a task that ran whole file requests at the
  priority of its flash routine would stop the flight task for all of them;
  its driver raises the priority for one operation at a time. Under
  pyro's scheduling, a task left free to run on the flight task's core can
  delay it.
- Verified by: review; `rt_round_trip` (the task runs at the priority
  given).

#### ALFS-RT-06

Before the scheduler starts, a request runs directly in the caller. A
request made from the flash task itself (a driver calling back into alfs)
or from an interrupt, and a call with a handle from a task other than the
one that opened it, are refused and change nothing.

- Priority: 3.
- Source: pyro formats and mounts before its scheduler starts; a request
  from the flash task to itself would wait forever.
- Verified by: `rt_before_scheduler`, `rt_reentry_refused`.

#### ALFS-RT-07

An erase of n steps is n requests, so other requests are served between
its steps.

- Priority: 1.
- Source: UC-5: erasing a 2,048-sector region takes about 90 s; held as
  one request it would block every file for that long (pyro's storage
  task is judged stalled after 5 s).
- Measure: a write requested during a long erase.
- Pass: it completes before the erase does. Fail: otherwise.
- Verified by: `rt_erase_interleaves`.

---

## 7. Traceability

| Use case | Requirements |
|---|---|
| UC-1 flight log | SPD-01 to SPD-04, SPD-08, SPD-09, LIFE-02, LIFE-03, IF-12 |
| UC-2 high-rate log | SPD-02, SPD-03, SPD-08, LIFE-02 |
| UC-3 restart after a reset | SPD-06, SAFE-04, IF-07, IF-09 |
| UC-4 newest flights | IF-07, IF-10, SAFE-11 |
| UC-5 erase before flight | SPD-01, LIFE-01, LIFE-07, SAFE-05, OBS-01, OBS-03, RT-07 |
| UC-6 read back | SAFE-03, SAFE-08, SAFE-12, SPD-10, IF-08, IF-09 |
| UC-7 power cut | SAFE-01, SAFE-02, SAFE-04, SAFE-13, SAFE-14 |
| UC-8 wrong geometry | SAFE-09, IF-02 |
| UC-9 write failures | SAFE-06, OBS-01 |
| UC-10 unattended flash | LIFE-04, LIFE-05, SAFE-07, SAFE-10, SPD-11, OBS-01, OBS-02 |
| UC-11 configuration files | IF-04, IF-06 to IF-11, SAFE-13, SAFE-14 |
| UC-12 many tasks | RT-01 to RT-07 |

---

## 8. Sources

- `docs/datasheets/` and its `README.md`: the four flash parts and the
  RP2040.
- `docs/FLASH_PROBE.md`: what the probe measured on the parts themselves.
- The littlefs fork (github.com/n9wxu/littlefs), branch `v3-integration`
  at 58367256: `REQUIREMENTS.md` sections 4-5 and Appendix B,
  `benches/bench_wlog.toml`, `bd/lfs3_emubd.h`, `lfs3.h`.
- The pyro firmware (github.com/n9wxu/pyro_fw) at v2.2.8: `DECISIONS.md`
  DD-035, DD-073, DD-074, DD-077, DD-086, DD-098; `REQUIREMENTS.md`
  FLT-LOG-06; `docs/outstanding_tasks.md` L2, L4, C6;
  `docs/flash_stall_options.md`; `src/flash_op.c`;
  `src/rtos/FreeRTOSConfig.h`.
- FreeRTOS-Kernel V11.3.1: the RP2040 SMP port pyro uses, and the POSIX
  port the host tests of the task layer run on.
- The user's statements of 2026-10-14 and 2026-10-15, quoted in section 1.
