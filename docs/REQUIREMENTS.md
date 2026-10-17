# alfs requirements

alfs, the append-log flash store: on serial NOR flash, an object area of
named objects, each written and read whole, and a log area where logs are
appended around a ring of sectors.

**Status: draft 4.** Requirements come first, then the tests that verify
them (`docs/TEST_PLAN.md`), then the code.

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

The user, on the layout:

> "Lets allocate a config area for configuration objects and the remaining
> area becomes the circular log storage. configuration objects are stored
> slowly so they could be read-modify-write from the config area. LUA
> programs are separate objects. Since the UI is simply GET and PUT of
> whole files, we are effectively making an object store."

The user, on the flash task and on writes:

> "Give ALFS a freertos task for the flash ops. Writes collect in the
> writer function and then spill to the flash ops task when enough data is
> collected. Reads fetch from the flash ops task and return the data.
> Serializing in the flash ops task ensures no collisions and allows each
> operation to be simple loops."
>
> "The application will handle rotations, deletions, erase, etc."
>
> On writes: "Write will block until the write is covered. The writer may
> not be a ram based system so XIP and writes are not allowed. ...
> Applications bring their ram ... A write will bring data and a ram block.
> The write will be placed in the ram block by alfs. If the ram block is
> full, it will be sent to the flash ops. When flash ops are over, the
> remaining data will be put in the ram block. If all data is consumed, the
> writer will return to the application."

And the user's choices: in the log area, erasing happens only when the
application asks; after a reset the application starts a new log rather
than resuming the old one.

**The two areas.**
- **The object area** holds configuration objects and Lua programs, each
  under its name. An object is replaced whole (PUT) and read whole (GET).
  Objects are saved on the ground and slowly, so a save may erase.
- **The log area** holds the logs, appended at one place that moves
  around a ring of sectors. Nothing in it erases unless the application
  asks.

**The order of priority.** Every requirement carries one:

1. **Speed (SPD):** no flash operation of the log stalls the system for
   longer than one page program, and no log write ever waits for an
   erase.
2. **Flash life (LIFE):** every byte programmed and every sector erased
   is one the logs and objects needed.
3. **Safety (SAFE):** what was synced or committed survives power loss
   and faults, as far as 1 and 2 allow.

Where two requirements conflict, the higher priority wins. Two examples
the requirements below settle this way: a chunk a power cut tore is
abandoned, not repaired (SPD over SAFE); and a write that finds no ready
sector fails at once rather than erasing one (SPD-04 over SAFE-02).

**Simplicity (SIMP)** is how the three are met: less code runs faster,
writes less and has fewer ways to fail. Its requirements are budgets.

---

## 2. Scope

**In scope:**

- **Objects** on raw serial NOR flash, found by name: put (replace
  whole), get (read whole), delete, list.
- **Logs** in a ring of sectors: start, append to one or more streams of
  the newest log, sync, read a stream sequentially, list, and delete a
  log by erasing it.
- One task that performs every flash operation of both areas for the
  application's tasks, on FreeRTOS; and the same code, without the task,
  for a caller with no RTOS.
- Erasing the log area when the application asks, and reporting how much
  of each area is ready.
- Power loss at any moment; the bad-sector and power-loss behaviours of
  section 4.4.
- A chip shared with executing code: alfs owns only the two areas it is
  given.

**Out of scope:**

- Writing into the middle of an object or a log, appending to an object,
  renaming, directories. A name may hold a `/`, but no directory exists.
- Reopening a log to append after a reset. The application starts a new
  log (the user's choice); the rest of the cut-short log's last sector
  stays unused until that log is erased.
- An object larger than half the object area.
- NAND, SD cards, eMMC: any medium with a write-back cache or its own
  translation layer. The reordering such a cache makes is not modelled.
- A part whose prog unit is over one byte (one with error correction
  inside it): alfs relies on A-8 and A-9.
- An erase value other than 0xFF, and encryption.

---

## 3. Use cases

Each comes from the pyro flight computer (github.com/n9wxu/pyro_fw). The
references are to its `DECISIONS.md` (DD-nnn), its requirements
(`REQUIREMENTS.md`, FLT-LOG-nn) and its open tasks
(`docs/outstanding_tasks.md`).

| # | Use case | Need |
|---|---|---|
| UC-1 | **A flight log.** 22-byte rows at 1 to 50 a second, written by a task of a system whose flight loop runs every 20 ms, and synced every second. | Every flash program or erase on the RP2040 stops both cores (DD-074), and an erase takes 35 to 800 ms (section 4.3), many loop periods. Hence the rulings. |
| UC-2 | **A high-rate log beside it.** Everything sensed, about 24 kB a second, written by another task into the same flight's log. | On NOR it is about 94 page programs a second. |
| UC-3 | **Restart after a reset in flight.** The board restarts and logs on into a new log; the log cut short stays readable. | The time from reset to the first sample is flight time lost (DD-086). |
| UC-4 | **Keep the newest flights.** Two, the oldest erased by the application. | C6. |
| UC-5 | **Erase before flight.** On the bench, or paced on the pad; launch waits until it is done. | The rulings; C6's OK to LAUNCH gate. A 4 KB erase takes 35 to 45 ms typically and up to 800 ms (section 4.3). |
| UC-6 | **Read a log back.** Each stream sequentially, for download, perhaps while the newest log is written. | `/api/flight.csv` renders the log as it reads. |
| UC-7 | **Power cut at any moment.** A battery lead lost at landing. | FLT-LOG-06: "no more than the last second shall be lost." |
| UC-8 | **The wrong geometry.** An image built for an 8 MB chip, run on a 2 MB one. | Nothing may be written past the chip's end or outside the areas, where the bootloader and the code lie (DD-098). |
| UC-9 | **Write failures.** A program that fails part-way. | Nothing is written twice, and every byte lost is counted (L4). |
| UC-10 | **Unattended, soldered-in flash.** Replacing the chip loses everything; the unit may be out of reach. | Degrade gracefully, shrink with bad sectors, stay readable at end of life, report health. |
| UC-11 | **Configuration objects.** Saved on the ground, a few hundred bytes each, each save replacing the object whole; read whole at boot. | The user's words on the object area (section 1). pyro's settings are files of under 1 kB (`pins.ini`, `beep.ini`, `pad.mkr`). |
| UC-12 | **Lua programs.** Each its own object, saved whole from the web interface, read whole when loaded. | The user's words (section 1). |
| UC-13 | **Many tasks, one flash.** The flight log from a storage task, objects and downloads from a network task, each blocking only itself. | pyro runs FreeRTOS on both cores (DD-073); its flash operations go through one routine that stops both cores for one operation at a time (DD-074, `src/flash_op.c`). |

---

## 4. The flash model

### 4.1 Terms

- **area:** a sector-aligned range of the chip given to one store: the
  object area or the log area.
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
- **object:** a named sequence of bytes in the object area, replaced and
  read whole.
- **name:** 1 to 31 bytes the application chooses for an object.
- **half:** the object area is two halves. The **active** half holds the
  objects; the **spare** half is where the next put or delete writes the
  objects again.
- **commit:** the record that makes a half active, programmed last.
- **log:** the bytes of one flight in the log area, in one or more
  streams, found by its number.
- **log number:** the number alfs gives a log when it starts; unique for
  the life of the area, and growing.
- **stream:** the bytes of one writer within a log, marked by a tag the
  application chooses.
- **block:** RAM the application gives with a log writer or reader, where
  alfs gathers the bytes written until they fill it, and the bytes read.
- **chunk:** the bytes of one stream that one program adds, checked as a
  unit.
- **sync:** a call after which every byte written so far is durable.
- **durable:** survives a power loss at any later moment.
- **flash task:** the one task that performs every flash operation of
  both areas.
- **request:** what a call in the application's task sends the flash task,
  and waits on until it is done.
- **ready sector:** a log sector whose erase completed and which nothing
  has written since: the only kind a log write may take (the rulings).
- **dirty sector:** a log sector that must be erased before it can be
  ready: it holds an interrupted erase, a claim a power cut tore, or
  anything not alfs's own.
- **bad sector:** one that fails its erase or programs (4.4).

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

A-9 lets alfs complete a program a power cut may have left half done, by
programming the same bytes again: a log sector's stamp, each time a claim
is programmed (SAFE-04); a half's stamp, when a put or delete starts
writing it (SAFE-12); the active half's commit, before the spare half is
erased (SAFE-13).

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
256-byte pages), timed with the typical figures of 4.3, and with a block
of one page unless a workload says otherwise.

- **W-LOG:** 22-byte rows produced at 1 or 50 a second, written every
  200 ms, synced every second, for 10 minutes (the littlefs fork's
  `benches/bench_wlog.toml`).
- **W-HR:** 480 bytes written every 20 ms (24,000 a second), synced every
  second, for 10 minutes.
- **W-RING:** W-LOG at 50 rows a second, a new log every 10 minutes, and
  the oldest log erased whenever fewer than two logs' worth of sectors are
  ready; one erase step requested every 20 ms; until every sector has been
  erased at least 10 times.
- **W-CFG:** a configuration object of 300 bytes put again and again,
  beside a 4,096-byte Lua program object and a second configuration
  object that is deleted and put back.

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

Nothing erases in the log area but `alfs_log_erase`. Nothing erases in the
object area but `alfs_obj_erase`, `alfs_obj_format`, and a put or delete
that finds the spare half not ready.

- Priority: 1.
- Source: the rulings; UC-1; the user's choice that the log area erases
  only when the application asks; the user's words that objects "are
  stored slowly so they could be read-modify-write".
- Measure: erases the simulator counts during each call, under W-LOG,
  W-HR, W-RING and W-CFG, with power loss at every operation and every
  bad-sector behaviour.
- Pass: zero in every other call. Fail: any.
- Verified by: `spd_no_erase_outside_erase`.

#### ALFS-SPD-02

A log write or sync that programs X bytes performs at most
ceil(X / (page − 37)) + 2 programs, plus one for each failed program and
one to zero the fresh sector (SAFE-04 rule 3). A write programs bytes only
when the block fills, and then all but less than a page's room: X is at
most the block's size B plus the bytes the write passes.

- Priority: 1.
- Source: UC-1, UC-2; the stall of 4.3. A write blocks until its bytes are
  covered (the user's words, section 1), so the burst it makes is what is
  bounded. 37 bytes is the least room a page gives payload: a sector's
  first page holds the stamp and claim (30 bytes), and every chunk has a
  7-byte header. The application chooses B to fit its slack: a sync of a
  full 4,096-byte block is about 18 programs, 7 ms at the typical time.
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

A log write or sync that finds no ready sector returns `ALFS_ENOSPC`,
keeps the bytes it holds in the writer's block, and erases nothing. A write
may have programmed part of its bytes before it found no ready sector.

- Priority: 1.
- Source: the rulings.
- Measure: a log written until no sector is ready.
- Pass: `ALFS_ENOSPC`, no erase, and after an erase makes a sector ready
  the bytes held are written. Fail: an erase, or a lost byte.
- Verified by: `spd_enospc_without_erase`.

#### ALFS-SPD-05

Mounting the log area reads at most 30 bytes of each sector; mounting the
object area reads at most 29 bytes of each half. Neither performs a flash
operation.

- Priority: 1.
- Source: UC-3. 30 bytes are a log sector's stamp (18) and claim (12); 29
  are a half's stamp (16), begun mark (1) and commit (12).
- Measure: bytes the simulator reads during each mount.
- Pass: at most those. Fail: more, or any operation.
- Verified by: `spd_mount_reads_bounded`.

#### ALFS-SPD-06

On W-LOG and W-HR no flash operation stalls longer than one page program.

- Priority: 1.
- Source: UC-1, UC-2. The user: "≤1ms writes is OK so the chunk size can
  be larger". A page program measures at most 0.46 ms on the chips
  (`docs/FLASH_PROBE.md`), so chunks are up to a page; the datasheets'
  maximums (2 to 3 ms) are above 1 ms.
- Measure: simulated time of each operation, at the typical times of 4.3.
- Pass: at most 0.4 ms (one W25Q128JV page program). Fail: more.
- Verified by: `spd_wlog_longest_stall`.

#### ALFS-SPD-07

The processor time of a log write, sync or read, apart from its flash
operations, grows only with the bytes the call passes, plus a constant.

- Priority: 1.
- Source: UC-1.
- Measure: on the host, bytes read per call against bytes passed, on log
  areas of 32 and 2,048 sectors.
- Pass: no term in the area's size. Fail: otherwise. The exceptions read
  each sector's header once: opening a reader, listing, an erase step, a
  write that takes a sector, and a read that moves to the next sector
  (SPD-08).
- Verified by: `spd_cpu_linear`.

#### ALFS-SPD-08

Reading on from one sector of a log to its next reads the headers of at
most the sectors between them, in the direction alfs allocates, and the
next one's.

- Priority: 1.
- Source: UC-6. A read that scanned every header in the area at each
  sector change would slow with the area's size.
- Measure: headers read by `alfs_log_read` at each sector change, reading
  back a log of two streams.
- Pass: at most the sectors between, plus one. Fail: more.
- Verified by: `spd_read_next_bounded`.

#### ALFS-SPD-09

Every loop alfs runs ends within a bound set by the area's sector count,
the bytes the call passes, or the objects an area can hold, whatever the
flash holds.

- Priority: 1.
- Source: each operation is a loop (section 1), and a loop whose end
  depends on what the flash reads is a hung task when the flash is
  damaged. "A corrupted data block is less important than a broken exec
  loop."
- Measure: every call made on fuzzed areas (SAFE-10); review of each
  loop's bound.
- Pass: every call returns. Fail: any that does not.
- Verified by: `safe_fuzz`; review.

### 6.2 Flash life (LIFE)

#### ALFS-LIFE-01

An erase in the log area takes only a sector that is dirty or belongs to
the log being erased, never a ready one. A put or delete erases nothing
when the spare half is ready, and otherwise each of its sectors once.

- Priority: 2.
- Source: the brief ("preserving the flash memory").
- Measure: under W-RING, each erase is classified by the sector's state
  before it; under W-CFG, erases per put.
- Pass: as stated. Fail: an erase of a ready sector or a ready half.
- Verified by: `life_erase_only_dirty`.

#### ALFS-LIFE-02

The bytes a log write or sync programs are at most the bytes it takes from
its block, plus 7 bytes a program, plus 30 bytes for each sector it takes
and 12 for each claim it zeroes.

- Priority: 2.
- Source: the brief. A sector costs its stamp (18 bytes, programmed by the
  erase) and a claim program that writes the stamp again with the claim
  (30); a chunk costs its header (7). SPEC section 1.
- Measure: bytes the simulator programs during writes and syncs, against
  the bound computed from its own counts, under W-LOG (22 bytes a sync at
  1 row a second, 1,100 at 50) and W-HR (24,000).
- Pass: within the bound. Fail: above it.
- Verified by: `life_write_amplification`.

#### ALFS-LIFE-03

On W-LOG, alfs erases no more sectors than the bytes it programmed fill.

- Priority: 2.
- Source: the brief; UC-1.
- Measure: erases over 10 minutes against the bytes programmed divided by
  the sector size, after the log is erased.
- Pass: erases at most the sectors filled, plus one. Fail: more.
- Verified by: `life_wlog_erases`.

#### ALFS-LIFE-04

Wear is level: under W-RING, the erase counts of the good sectors differ
by at most 2.

- Priority: 2.
- Source: UC-10; 100K-cycle endurance (4.3). The ring takes sectors in
  order and the oldest log is erased first, so each sector is erased once
  a lap.
- Measure: each sector's erase count at the end of W-RING, from the
  simulator.
- Pass: largest minus smallest at most 2. Fail: more.
- Verified by: `life_wear_level`.

### 6.3 Safety (SAFE)

#### ALFS-SAFE-01

Every byte a successful sync or close made durable reads back intact after
a power loss at any later operation, under every power-loss behaviour of
4.4.

- Priority: 3.
- Source: UC-7.
- Measure: W-LOG with power lost after operation k, for every k, and a
  second loss in the log after the restart; remount; read every log.
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
- Measure: logs with bits flipped in chunk headers, payloads and checks.
- Pass: no damaged byte returned; one failure counted for each page
  skipped; the chunks of the pages after it read. Fail: otherwise.
- Verified by: `safe_no_false_data`.

#### ALFS-SAFE-04

After a mount, nothing synced shares a page with bits a power loss may
have left half programmed, and no claim or stamp can decay into
reclaiming what was synced:

1. After a mount, writing starts a new log in a sector of its own
   (section 2).
2. Every program of a claim starts at the sector's first byte and programs
   the stamp again, built from the configuration and the floor it reads
   (A-9).
3. After a mount, the first sector after the sector of the newest valid
   claim whose stamp is valid and whose claim is neither valid nor zero,
   as the mount found them, is not used: when allocation reaches it, alfs
   programs 00h over its claim, and an erase reclaims it.

- Priority: 3.
- Source: A-3, A-9; METASTABLE and FLICKER (4.4). The program a power loss
  interrupts is a chunk, a claim with its sector's first chunk, or a
  stamp. Rule 1: the torn chunk is in the cut-short log's last sector,
  which nothing writes again. Rule 2: a stamp that an erase's power loss
  left half programmed can read valid at one mount and invalid at a later
  one. Rule 3: a claim program torn at its start leaves a sector that reads
  ready but holds a bit that reads either way; it is the first ready
  sector from where allocation stood, after the newest claim.
- Measure: the simulator flags a program of a byte an interrupted
  operation changed (every behaviour but ATOMIC), unless it is given what
  the programs before it meant, or 00h; the power-loss runs remount, start
  a new log, write, and read again, with erases between.
- Pass: no flag, and every read as SAFE-01 requires. Fail: otherwise.
- Verified by: `safe_no_program_after_torn`, `safe_powerloss_durable`.

#### ALFS-SAFE-05

alfs writes only into a log sector, or a half, whose erase is known to
have completed and whose erased state it verified on the ground.

- Priority: 3.
- Source: the rulings; A-4; ERASENOOP. The probe's P9: an erase aborted at
  a tenth (W25Q128JV) or half (XT25F128F) of its time already reads blank,
  so a blank check alone cannot show an erase completed; the stamp,
  programmed after the erase returns done and the sector reads blank,
  does (`docs/FLASH_PROBE.md` section 4).
- Measure: power loss during erases, and ERASENOOP sectors, in both areas.
- Pass: no write lands in a sector whose erase was interrupted or did
  nothing. Fail: any.
- Verified by: `safe_write_only_verified_ready`.

#### ALFS-SAFE-06

A failed log program is counted and its bytes are dropped and counted;
the log goes on in a fresh sector, since a page that fails is a sector
that is failing. A program is checked by reading it back.

- Priority: 3.
- Source: UC-9. "A corrupted data block is less important than a broken
  exec loop": a retry would repeat a stall the flight loop already paid.
- Measure: PROGERROR, PROGNOOP and PROGFLIP sectors in the path of W-LOG.
- Pass: every byte reads back or is counted as lost; nothing is written
  twice. Fail: otherwise.
- Verified by: `safe_prog_failure`.

#### ALFS-SAFE-07

A log sector that fails an erase, or its check after one, is not used
again in this mount, and the area's capacity shrinks by it. A half whose
erase or check fails makes the put or delete return `ALFS_EPROG`, and the
active half stays the store.

- Priority: 3.
- Source: UC-10.
- Measure: ERASEERROR and ERASENOOP sectors, and wear-out, under W-RING
  and W-CFG.
- Pass: excluded and counted; after a remount it is dirty again, and its
  next erase finds it bad again before anything is written to it. Marking
  it on the flash would mean programming bytes already programmed. Every
  object reads as it did. Fail: a write or a stamp lands in it, or an
  object changes.
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

alfs programs and erases only inside its two areas. A configuration whose
area does not lie inside the chip is refused before any operation. A log
area holding anything else mounts with every such sector dirty, and an
object area holding no valid half fails to mount with `ALFS_ENOFS`; both
write nothing.

- Priority: 3.
- Source: UC-8.
- Measure: areas holding random data, an area configured for another
  sector size or area size, an area past the chip's end.
- Pass: as stated, and no operation. Fail: any write outside the areas,
  or any write at mount.
- Verified by: `safe_foreign_area`.

#### ALFS-SAFE-10

Every call finishes, without a crash, on any content of either area.

- Priority: 3.
- Source: UC-10; crafted images. Every call is a loop (SPD-09).
- Measure: fuzzed areas (random sectors, valid headers with random
  fields), under a sanitizer: every call of both stores.
- Pass: every call returns; no sanitizer error. Fail: otherwise.
- Verified by: `safe_fuzz`.

#### ALFS-SAFE-11

Once `alfs_log_erase` reports no sector of a log left, none of the log's
sectors reads again after any power loss. Until then, the log reads only
chunks written to it.

- Priority: 3.
- Source: UC-4. An erase takes the dirty sectors with the log's own, so a
  claim of the log that a power cut tore, which reads invalid at one
  moment and valid at a later one, is erased too.
- Measure: power loss at every operation of a log's erase; remount;
  remounts and reads of every log.
- Pass: as stated. Fail: a sector of an erased log, or another log's
  chunk, read.
- Verified by: `safe_log_erase`.

#### ALFS-SAFE-12

A put or delete is atomic: until it returns `ALFS_OK`, every object reads
whole as it was, or after a power loss, either as it was or as the put or
delete makes it; once it returns, as it makes it, after any power loss.

- Priority: 3.
- Source: UC-11, UC-12. A put writes every other object again into the
  spare half, then the new object, then the commit; a mount takes the half
  with the newer valid commit.
- Measure: power loss at every operation of W-CFG; after each, eight
  remounts and a read of every object.
- Pass: as stated. Fail: an object missing, partial, or mixed.
- Verified by: `safe_put_atomic`.

#### ALFS-SAFE-13

Before erasing the spare half, alfs programs the active half's commit
again (A-9).

- Priority: 3.
- Source: METASTABLE: a commit a power cut left half programmed can read
  valid at one mount and invalid at a later one. Had a put begun erasing
  the other half after the first, no valid half would remain.
- Measure: as SAFE-12, with the loss at the commit's program under each
  behaviour, then puts.
- Pass: as SAFE-12. Fail: otherwise.
- Verified by: `safe_put_atomic`.

#### ALFS-SAFE-14

A get returns no byte of an object whose check fails: opening it reads
the whole object and checks it first, and fails with `ALFS_ECHECK`. An
object's size is where the next object begins, so damage to a size hides
the objects after it in the half.

- Priority: 3.
- Source: UC-11, UC-12: a configuration or program partly wrong is worse
  than one missing, which the application replaces with its default.
- Measure: objects with bits flipped in their name, size, payload and
  check.
- Pass: no damaged byte returned: `ALFS_ECHECK` for the damaged object,
  or `ALFS_ENOENT` when its name is damaged; every object before it reads,
  and every object after it unless its size is damaged. Fail: a damaged
  byte returned.
- Verified by: `safe_obj_check`.

### 6.4 Simplicity (SIMP)

#### ALFS-SIMP-01

The stores are C99: `alfs.h` (what both share), `alfs_flash.c` and
`alfs_flash.h` (the checked flash operations both use), `alfs_log.c` and
`alfs_log.h`, `alfs_obj.c` and `alfs_obj.h`. They use no dynamic memory, no
recursion, no floating point, and no library beyond `<stdint.h>`,
`<stddef.h>`, `<stdbool.h>` and `<string.h>`. The task layer is one more
source and header (`alfs_rtos.c`, `alfs_rtos.h`), which adds only the
FreeRTOS kernel's headers.

- Priority: S.
- Source: the stores are usable without an RTOS (section 2); each file
  has one responsibility.
- Measure: the build and a review.
- Pass: as stated. Fail: otherwise.
- Verified by: `make target` (compiles each store alone, with
  `-std=c99 -Wall -Wextra -Werror -pedantic`), `make target-rt`; review.

#### ALFS-SIMP-02

The stores together are at most 4 KiB on a Cortex-M0+ (Thumb, `-Os`), and
the task layer at most 1.5 KiB.

- Priority: S.
- Source: the brief ("Simplicity will keep the code fast and flash
  overhead cheap").
- Measure: `arm-none-eabi-size` of `alfs_flash.o`, `alfs_log.o` and
  `alfs_obj.o`, and of `alfs_rtos.o`, text plus data; the task layer
  compiled against the kernel's generic Cortex-M0 port.
- Pass: at most 4,096 and 1,536 bytes. Fail: more.
- Verified by: `make target`, `make target-rt`.

#### ALFS-SIMP-03

RAM does not grow with the areas, and all of it is the caller's: each
instance at most 96 bytes and a scratch page; a log writer or reader at
most 32 bytes and its block, of at least one page; an object reader or put
at most 16 bytes; the task layer, the task's stack and control block, the
queue's storage, and for each request about 200 bytes on the calling
task's stack.

- Priority: S.
- Source: pyro's Lua boards have about 13.5 kB of SRAM spare (pyro's
  `docs/flash_stall_options.md` section 6.3), and an area of 8 MiB must
  not cost more than a small one. The user: "Applications bring their ram
  so ram allocations in ALFS should not be an issue."
- Measure: `sizeof` of each structure on the Cortex-M0+, blocks and
  scratch excluded.
- Pass: within those. Fail: more.
- Verified by: `simp_ram`; `make target`.

#### ALFS-SIMP-04

No store call uses more than 320 bytes of stack, and the flash task's
deepest chain no more than 512 bytes, the driver's own frames apart.

- Priority: S.
- Source: pyro's smallest task stack is 2 kB. The flash task's stack holds
  the stores' deepest chain under the task's loop.
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

The driver is three functions: read bytes, program bytes within one page,
and erase one sector. Each returns when its work is done, or fails; a
driver that must wait for the system to allow a flash operation waits, and
fails only on a fault of the flash. A read sees every program and erase
before it.

- Priority: S.
- Source: pyro's flash routine refuses an operation it cannot start within
  500 ms (`src/flash_op.c`); to alfs a refusal is a failed program, and the
  sector would be abandoned.
- Verified by: the simulator implements exactly these.

#### ALFS-IF-02

Each area is configured: the sector and page size (powers of two, page at
least 128 bytes, sector at least four pages), the area's start and length
(sector-aligned; the object area an even number of sectors), and the
chip's size. alfs programs a byte at most once between erases, except to
settle what a power loss may have left half programmed (A-8, A-9): a log
sector's stamp again with each claim, 00h over a claim (SAFE-04 rule 3), a
half's stamp again with its begun mark, and the active commit again
(SAFE-13).

- Priority: S.
- Source: the prog unit is one byte (section 2). A page holds a half's
  records and a log sector's header with a chunk.
- Verified by: `if_geometry` (every combination the tests use is
  accepted; misaligned or oversized ones are refused); every test (the
  simulator refuses a program into a byte already programmed unless it is
  the byte its programs meant, or 00h).

#### ALFS-IF-03

The on-disk format is little-endian and read and written byte by byte, so
any host reads any image.

- Priority: S.
- Verified by: `if_endian`.

#### ALFS-IF-04

The driver programs exactly the bytes alfs passes it, and no others. On
the BY25Q64ES a page padded with the erase value takes 3.2 times as long as
its 16 programmed bytes sent alone (`docs/FLASH_PROBE.md` section 3).

- Priority: S.
- Source: the probe.
- Verified by: review of each driver; the simulator counts the bytes of
  each program.

#### ALFS-IF-05

An object is found by its name: 1 to 31 bytes, none of them 00h, compared
byte for byte. A `/` is an ordinary byte.

- Priority: S.
- Source: the user: "configuration objects", "LUA programs are separate
  objects". pyro's longest file path is 15 bytes (`/www/index.html`).
- Verified by: `if_object_names` (names of 1 and 31 bytes; 0 and 32 bytes
  refused).

#### ALFS-IF-06

A put replaces an object whole: it names the object and its size, takes
its bytes in any number of writes, and commits. Until the commit, a get
reads what the object held before; a put of an object that does not exist
creates it. A delete removes an object. One put at a time: another is
refused with `ALFS_EOPEN` until the first commits or is abandoned.

- Priority: S.
- Source: the user: "the UI is simply GET and PUT of whole files". An
  HTTP PUT names its length (`Content-Length`) and arrives in pieces.
- Verified by: `if_put_replaces`, `if_put_one_at_a_time`.

#### ALFS-IF-07

A get reads an object from its start, in any number of reads, as it was
when opened, even when a put commits meanwhile. While any reader holds
the spare half, a put, delete or erase that would erase it is refused with
`ALFS_EOPEN`.

- Priority: S.
- Source: UC-12: a download of a program while it is replaced.
- Verified by: `if_get_snapshot`.

#### ALFS-IF-08

Listing the objects reports each one's name and size.

- Priority: S.
- Source: the user: "GET and PUT of whole files"; the web interface lists
  what it can get.
- Verified by: `if_object_list`.

#### ALFS-IF-09

A log's number is unique for the life of the area, grows with each log
started, and is never used again, even after the log is erased or a power
loss tears its first claim.

- Priority: S.
- Source: UC-3, UC-4: the application names flights by number. Each stamp
  records the next sequence, and a mount takes two more than the largest
  in any claim or stamp, three when it finds a torn claim (SPEC 1.1).
  Three, because a claim torn straight after a mount holds the sequence
  that mount gave, two more than the largest valid one.
- Verified by: `if_log_numbers_never_reused`.

#### ALFS-IF-10

A log holds one stream for each tag (0 to 255) the application writes. A
writer opens a stream of a new log, or of the newest log while that log is
being written in this mount; a new log starts only when no writer is
open. A reader reads one stream of one log, in the order written.

- Priority: S.
- Source: UC-1, UC-2: the flight log and the high-rate log are two
  writers of one flight. UC-3: after a reset, logging starts a new log
  (SAFE-04 rule 1).
- Verified by: `if_log_streams`, `if_log_new_after_mount`.

#### ALFS-IF-11

Listing the logs reports each one's number and how many sectors it holds,
in growing number order.

- Priority: S.
- Source: UC-4: the application erases the oldest.
- Verified by: `if_log_list`.

#### ALFS-IF-12

A log write copies the caller's bytes into the writer's block; when the
block is full, its bytes are programmed, all but less than a page's room;
the write returns when every byte it was given is in flash or in the
block. A sync programs what the block holds.

- Priority: S.
- Source: the user's words on writes, section 1. Keeping the last part of
  a page in the block packs the log into whole-page chunks between syncs.
- Verified by: `basic_round_trip`, `spd_write_burst_bounded`.

#### ALFS-IF-13

Erasing a log is refused with `ALFS_EOPEN` while it is being written, or
while any log reader is open. A mount of either area is refused while any
of its handles is open.

- Priority: S.
- Source: a reader of an erased log could read on into a sector taken by
  another log.
- Verified by: `if_log_refusals`.

### 6.6 Observability and errors (OBS)

#### ALFS-OBS-01

The log area reports, at any moment: sectors ready, dirty and bad; bytes
writable without an erase; the log being written; and its counts of
programs, erases, failed programs, failed erases, failed checks and bytes
lost. The object area reports the bytes its objects take and the bytes
it can hold, whether the spare half is ready, and the same counts.

- Priority: 3.
- Source: UC-5 (the gate), UC-9, UC-10.
- Verified by: `obs_status` (each count against the simulator's).

#### ALFS-OBS-02

Every error a call can return is documented with the state it leaves and
what the caller should do: retry, rebuild (format), or fail.

- Priority: 3.
- Source: UC-10: an unattended unit's software must know which of the
  three to do.
- Verified by: review of the headers.

#### ALFS-OBS-03

Each area reports whether it is clean, so the caller can hold a launch
until all unused flash is erased: the log area when no dirty sector
remains, the object area when its spare half is ready.

- Priority: 3.
- Source: UC-5; the first ruling.
- Verified by: `obs_clean`.

### 6.7 The flash task (RT)

The task layer serves one log area and one object area; every requirement
above holds through it.

#### ALFS-RT-01

One task performs every flash operation of both areas; the application's
tasks reach the flash only through requests to it, served one at a time.

- Priority: 1.
- Source: "Serializing in the flash ops task ensures no collisions and
  allows each operation to be simple loops."
- Measure: the test driver records the task each operation runs in, and
  whether two overlap.
- Pass: every operation in the flash task, none overlapping, under
  several writers, readers, puts and erases at once. Fail: otherwise.
- Verified by: `rt_serialises`.

#### ALFS-RT-02

Every request blocks the task that made it until the flash task has done
it. A log write returns when every byte it was given is in flash or in the
block; a read returns bytes the flash task fetched.

- Priority: 1.
- Source: the user's words on writes, section 1.
- Verified by: `rt_round_trip`, `rt_read_while_writing`, `rt_objects`.

#### ALFS-RT-03

A log write whose bytes all fit in the block, without filling it, sends no
request; nor does a log read served from what the block already holds.

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
- Source: pyro's DD-074: a task that ran whole requests at the priority of
  its flash routine would stop the flight task for all of them; its driver
  raises the priority for one operation at a time. Under pyro's
  scheduling, a task left free to run on the flight task's core can delay
  it.
- Verified by: review; `rt_round_trip` (the task runs at the priority
  given).

#### ALFS-RT-06

Before the scheduler starts, a request runs directly in the caller. A
request made from the flash task itself (a driver calling back into alfs)
or from an interrupt, and a call with a log handle from a task other than
the one that opened it, are refused and change nothing.

- Priority: 3.
- Source: pyro mounts before its scheduler starts; a request from the
  flash task to itself would wait forever. A log handle's block is filled
  and drained in its owner's task (RT-03).
- Verified by: `rt_before_scheduler`, `rt_reentry_refused`.

#### ALFS-RT-07

An erase of n steps is n requests, so other requests are served between
its steps.

- Priority: 1.
- Source: UC-5: erasing a 2,048-sector area takes about 90 s; held as one
  request it would block every log and object for that long (pyro's
  storage task is judged stalled after 5 s).
- Measure: a write requested during a long erase.
- Pass: it completes before the erase does. Fail: otherwise.
- Verified by: `rt_erase_interleaves`.

---

## 7. Traceability

| Use case | Requirements |
|---|---|
| UC-1 flight log | SPD-01 to SPD-04, SPD-06, SPD-07, LIFE-02, LIFE-03, IF-10, IF-12 |
| UC-2 high-rate log | SPD-02, SPD-03, SPD-06, LIFE-02, IF-10 |
| UC-3 restart after a reset | SPD-05, SAFE-04, IF-09, IF-10 |
| UC-4 newest flights | IF-09, IF-11, IF-13, SAFE-11 |
| UC-5 erase before flight | SPD-01, LIFE-01, SAFE-05, OBS-01, OBS-03, RT-07 |
| UC-6 read back | SAFE-03, SAFE-08, SPD-08, IF-10 |
| UC-7 power cut | SAFE-01, SAFE-02, SAFE-04, SAFE-12, SAFE-13 |
| UC-8 wrong geometry | SAFE-09, IF-02 |
| UC-9 write failures | SAFE-06, OBS-01 |
| UC-10 unattended flash | LIFE-04, SAFE-07, SAFE-10, SPD-09, OBS-01, OBS-02 |
| UC-11 configuration objects | IF-05 to IF-08, SAFE-12 to SAFE-14 |
| UC-12 Lua programs | IF-05 to IF-08, SAFE-12, SAFE-14 |
| UC-13 many tasks | RT-01 to RT-07 |

---

## 8. Sources

- `docs/datasheets/` and its `README.md`: the four flash parts and the
  RP2040.
- `docs/FLASH_PROBE.md`: what the probe measures on the parts themselves.
- The littlefs fork (github.com/n9wxu/littlefs), branch `v3-integration`
  at 58367256: `bd/lfs3_emubd.h` (the fault model), `lfs3.h` (A-3),
  `benches/bench_wlog.toml` (W-LOG).
- The pyro firmware (github.com/n9wxu/pyro_fw) at v2.2.8: `DECISIONS.md`
  DD-073, DD-074, DD-086, DD-098; `REQUIREMENTS.md` FLT-LOG-06;
  `docs/outstanding_tasks.md` L4, C6; `docs/flash_stall_options.md`;
  `src/flash_op.c`; `src/rtos/FreeRTOSConfig.h`; `src/pin_store.h`,
  `src/beep_store.h`, `src/flight_resume.h` (its settings files).
- FreeRTOS-Kernel V11.3.1: the RP2040 SMP port pyro uses, and the POSIX
  port the host tests of the task layer run on.
- The user's statements quoted in section 1.
