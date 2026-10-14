# alfs requirements

alfs, the append-log flash store: a small filesystem for append-only logs
on serial NOR flash.

**Status: draft 1, 2026-10-14.** Requirements come first, then the tests
that verify them (`docs/TEST_PLAN.md`), then the code. No code exists yet.

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

**The order of priority.** Every requirement carries one:

1. **Speed (SPD):** no call ever stalls its caller for longer than one
   flash program, and no append ever waits for an erase.
2. **Flash life (LIFE):** every byte programmed and every sector erased
   is one the log needed.
3. **Safety (SAFE):** what was synced survives power loss and faults, as
   far as 1 and 2 allow.

Where two requirements conflict, the higher priority wins. Two examples
the requirements below settle this way: a program that fails in the append
path is not retried in the same call (SPD-02 over SAFE-06), and a chunk a
power cut tore is abandoned, not repaired (SPD over SAFE).

**Simplicity (SIMP)** is how the three are met: less code runs faster,
writes less and has fewer ways to fail. Its requirements are budgets.

---

## 2. Scope

**In scope:**

- Append-only streams on raw serial NOR flash: create, append, sync,
  close, reopen to append after a reset, read sequentially, delete whole.
- Several streams open at once (a flight log beside a high-rate log).
- Erasing ahead of need, in steps the caller schedules, and reporting how
  much is ready.
- Power loss at any moment; the bad-sector and power-loss behaviours of
  section 4.4.
- A region of one flash chip shared with executing code: alfs owns only
  the region it is given.

**Out of scope:**

- Writing into the middle of a stream, truncating, renaming, directories.
  Configuration files belong in a general filesystem (littlefs).
- NAND, SD cards, eMMC: any medium with a write-back cache or its own
  translation layer. Their reordering (littlefs's OOO behaviour) is not
  modelled.
- Encryption and an unknown erase value.
- Threads: the caller serialises calls on one instance.

---

## 3. Use cases

Each comes from the pyro flight computer (github.com/n9wxu/pyro_fw). The
references are to its `DECISIONS.md` (DD-nnn), its requirements
(`REQUIREMENTS.md`, FLT-LOG-nn) and its open tasks
(`docs/outstanding_tasks.md`).

| # | Use case | What pyro learned |
|---|---|---|
| UC-1 | **A flight log.** 22-byte rows at 1 to 50 a second, appended by a task of a system whose flight loop runs every 20 ms, and synced every second. | Every flash program or erase on the RP2040 stops both cores (DD-074). littlefs 2.11.3 erased a block at every sync: 46 to 73 ms stops and 20 to 77 loop overruns a minute (DD-035, L2). Hence the rulings. |
| UC-2 | **A high-rate log.** Everything sensed, about 24 kB a second: 16.2 MB in a 668 s flight. | On MK1C-SD it goes to an SD card through a 32 kB RAM ring, into a file preallocated on the pad (DD-077). On NOR it is about 94 page programs a second. |
| UC-3 | **Resume after a reset in flight.** The board restarts, reopens the newest log and appends to it. | DD-086. The time from reset to the first sample is flight time lost. |
| UC-4 | **Keep the newest flights.** Two, the oldest dropped. | C6. |
| UC-5 | **Erase before flight.** On the bench, or paced on the pad; launch waits until it is done. | The rulings; C6's OK to LAUNCH gate. A 4 KB erase takes 35 to 45 ms typically and up to 800 ms (section 4.3). |
| UC-6 | **Read a flight back.** Sequentially, for download, perhaps while another stream is written. | `/api/flight.csv` renders the log as it reads. |
| UC-7 | **Power cut at any moment.** A battery lead lost at landing. | FLT-LOG-06: "no more than the last second shall be lost." |
| UC-8 | **The wrong geometry.** An image built for an 8 MB chip, run on a 2 MB one. | The flash wrapped, and the filesystem formatted over the bootloader (DD-098). |
| UC-9 | **Write failures.** A program that fails part-way. | A retry wrote part of a record twice and misaligned everything after it, and nothing counted the loss (L4). |
| UC-10 | **Unattended, soldered-in flash.** Replacing the chip loses everything; the unit may be out of reach. | From the littlefs work: degrade gracefully, shrink with bad sectors, stay readable at end of life, report health. |

---

## 4. The flash model

### 4.1 Terms

- **region:** the sector-aligned range of the chip alfs is given.
- **sector:** the erase unit, a power of two (4,096 bytes on every part
  here).
- **page:** the most one program operation writes; a program never
  crosses a page boundary (256 bytes here).
- **prog unit:** the smallest aligned unit the flash programs on its own,
  1 byte to a page. alfs programs each prog unit at most once between
  erases: the rulings' "only write to erased sectors", at the finest grain
  the flash allows. A part with error correction over 16-byte units has a
  prog unit of 16; the four parts here, 1 (A-2), since a driver that must
  program whole pages pads them with the erase value, which leaves
  programmed bits as they are (A-1).
- **flash operation:** one call to the driver's program or erase. Reads are
  not operations: they do not take the chip out of execute-in-place.
- **call:** one call of an alfs function.
- **stall:** the time one flash operation stops a system that executes
  from the same chip: on the RP2040, every core (section 4.3).
- **stream:** an append-only sequence of bytes, what alfs stores; a file.
- **chunk:** the bytes of one stream that one program adds, checked as a
  unit.
- **sync:** a call after which everything appended so far is durable.
- **durable:** survives a power loss at any later moment.
- **ready sector:** a sector whose erase completed and which nothing has
  written since: the only kind an append may write (the rulings).
- **dirty sector:** a sector that must be erased before it can be ready:
  it holds a deleted stream, an interrupted erase, or anything not alfs's
  own.
- **bad sector:** one that fails its erase or programs (4.4).

### 4.2 Assumptions about the flash

| # | Assumption | Source |
|---|---|---|
| A-1 | A program moves bits only away from the erase value; only an erase moves them back. | Every NOR part |
| A-2 | A program of fewer bytes than a page, into erased bytes, leaves the page's other bytes as they were, and a page may be programmed so at least 16 times between erases. | All four datasheets (`docs/datasheets/README.md`); none states a limit |
| A-3 | A program interrupted by power loss changes no byte outside its own page. | The littlefs fork assumes the same of a prog (`lfs3.h`, `gc_preerase_count`) |
| A-4 | An erase interrupted by power loss changes no byte outside its own sector, and may leave that sector in any state, including one that reads as erased but holds bits that read randomly. | littlefs's METASTABLE behaviour (4.4) |
| A-5 | Operations complete in the order issued; there is no write-back cache. | Raw serial NOR |
| A-6 | The erase value is known and fixed: 0xFF on every part here. | The datasheets |
| A-7 | A read can fail, as at low supply voltage, and succeed when retried. | The littlefs work's error-handling principle |

### 4.3 Timing and endurance

| Part (pyro board) | Page program typ / max | 4 KB erase typ / max | 64 KB erase typ / max | Endurance |
|---|---|---|---|---|
| W25Q16JV (MK1B) | 0.4 / 3 ms | 45 / 400 ms | 150 / 2,000 ms | 100K cycles, minimum |
| W25Q128JV (MK1A) | 0.4 / 3 ms | 45 / 400 ms | 150 / 2,000 ms | 100K, minimum |
| XT25F128F (MK1C) | 0.4 / 2 ms | 40 / 800 ms | 250 / 1,600 ms | 100,000, minimum |
| BY25Q64ES (MK1B-8MB) | 0.45 / 2.4 ms | 35 / 300 ms | 180 / 2,000 ms | 100k, typical |

Pages are cited in `docs/datasheets/README.md`. All four can suspend an
erase, so a driver can return before an erase ends and resume it later.
On the RP2040 a program or erase stops every core that runs code from the
same chip for its whole duration (`rp2040-datasheet_2025-02-20.pdf`
p. 136).

### 4.4 Faults

Power-loss behaviours, from the littlefs fork's emulated block device
(`REQUIREMENTS.md` 5.5 at `v3-integration`; `bd/lfs3_emubd.h`). The
interrupted operation:

| Name | Program | Erase |
|---|---|---|
| ATOMIC | is not applied | is not applied |
| SOMEBITS | one bit of its first prog unit is programmed | the old data stays and one bit flips |
| MOSTBITS | is applied, then one bit of its first prog unit flips | is applied, then one bit anywhere flips |
| METASTABLE | is applied, then one bit it wrote reads randomly until the sector is erased | is applied, then one bit of the sector reads randomly until erased |
| TORNTAIL | its first prog unit stays erased, a run of random length after it is programmed, and one bit of that run flips | is atomic |

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
4.3.

- **W-LOG:** littlefs's logging workload, so the figures compare with its
  (the fork's `benches/bench_wlog.toml`): 22-byte rows produced at 1 or 50 a
  second, appended every 200 ms, synced every second, for 10 minutes. Prog
  unit 1, 16 or 256.
- **W-HR:** 480 bytes appended every 20 ms (24,000 a second), synced every
  second, for 10 minutes.
- **W-RING:** W-LOG at 50 rows a second, a new stream every 10 minutes, and
  the oldest stream deleted whenever fewer than two streams' worth of
  sectors are ready; one erase step every 20 ms; until every sector has been
  erased at least 10 times.

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

No call erases, except an erase step (`alfs_erase_step`) and format.

- Priority: 1.
- Source: the rulings; UC-1.
- Measure: erases the simulator counts during each call, under W-LOG,
  W-HR and W-RING, with power loss at every operation and every bad-sector
  behaviour.
- Pass: zero in every call other than those two. Fail: any.
- Verified by: `spd_no_erase_outside_erase_step`.

#### ALFS-SPD-02

No call performs more than one flash operation, except an erase step, which
performs at most one erase and one program.

- Priority: 1.
- Source: UC-1; the stall of 4.3.
- Measure: operations per call, as SPD-01.
- Pass: at most one (an erase step: one erase and one program). Fail: more.
- Verified by: `spd_one_operation_per_call`.

#### ALFS-SPD-03

Every program is of one page or less, at a page-aligned boundary or within
one page.

- Priority: 1.
- Source: 4.1; a page program is the shortest stall (4.3).
- Measure: the simulator rejects a program that crosses a page.
- Pass: none crosses. Fail: any.
- Verified by: every test (the simulator asserts it).

#### ALFS-SPD-04

An append that cannot be placed without an erase returns `ALFS_ENOSPC` at
once, keeps what it had buffered, and performs no flash operation.

- Priority: 1.
- Source: the rulings.
- Measure: a stream appended until no sector is ready.
- Pass: `ALFS_ENOSPC`, no operation in that call, and after an erase step
  makes a sector ready the buffered bytes are written. Fail: an erase, a
  wait, or a lost byte.
- Verified by: `spd_enospc_without_erase`.

#### ALFS-SPD-05

alfs never waits: it does not poll, spin or sleep, in any call. A driver
erase may return `ALFS_BUSY` to say it continues, and the erase step then
returns `ALFS_BUSY` and resumes the erase when called again.

- Priority: 1.
- Source: UC-1; pyro's rule that the loop is the only clock; a driver that
  slices an erase with suspend and resume (4.3).
- Measure: a driver that returns `ALFS_BUSY` n times per erase; code review
  for loops whose exit depends on the flash.
- Pass: each erase step returns `ALFS_BUSY` until the driver finishes, then
  stamps the sector; no loop waits on the flash. Fail: otherwise.
- Verified by: `spd_busy_erase_resumes`; review.

#### ALFS-SPD-06

Mounting reads at most 32 bytes of each sector of the region, twice, and
performs no flash operation.

- Priority: 1.
- Source: UC-3.
- Measure: bytes the simulator reads during `alfs_mount`.
- Pass: at most 64 times the number of sectors. Fail: more, or any
  operation.
- Verified by: `spd_mount_reads_bounded`.

#### ALFS-SPD-07

Opening the newest stream to append, after a mount, reads at most one
sector's bytes more than mounting does, and performs no flash operation.

- Priority: 1.
- Source: UC-3.
- Measure: bytes read by `alfs_open` for append on a stream whose last
  sector is full of short chunks.
- Pass: at most one sector's size, plus 32 bytes for each sector it steps
  over. Fail: more.
- Verified by: `spd_reopen_reads_bounded`.

#### ALFS-SPD-08

On W-LOG and W-HR the longest stall of any call is one page program.

- Priority: 1.
- Source: UC-1, UC-2; littlefs v2.11.3's 96.6 ms and v3's 49.6 to 53.9 ms
  on W-LOG (the fork's `REQUIREMENTS.md`, Appendix B.1).
- Measure: simulated time of each call's operations, at the typical times
  of 4.3.
- Pass: at most 0.4 ms (one W25Q128JV page program). Fail: more.
- Verified by: `spd_wlog_longest_stall`.

#### ALFS-SPD-09

The processor time of a call, apart from its flash operation, grows only
with the bytes the call passes, plus a constant.

- Priority: 1.
- Source: UC-1.
- Measure: on the host, instructions executed per call (by an instruction
  counter or cycle-accurate model) against bytes passed; on an RP2040 at
  125 MHz, the time of an append and sync of 256 bytes.
- Pass: on the RP2040, at most 100 us for 256 bytes; on the host, linear
  in bytes with no term in region size. Fail: otherwise.
- Verified by: `spd_cpu_linear` (host); `target_append_256` (RP2040,
  deferred until a target build exists).

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

The bytes programmed are at most the bytes appended, plus 12 bytes a
program with each program rounded up to the prog unit, plus 64 bytes or
three prog units a sector, whichever is more. For syncs of n bytes, n no
more than a page, that is (n + 12) / n.

- Priority: 2.
- Source: the brief; littlefs v3 programs about 768 bytes of metadata for a
  16-byte append and sync with prog unit 256 (the fork's Appendix B.2).
- Measure: bytes the simulator programs, against the bound computed from
  its own counts of programs and sectors used, under W-LOG (22 bytes a
  sync at 1 row a second, 1,100 at 50) and W-HR (24,000).
- Pass: within the bound, with the prog unit rounding of each chunk added.
  Fail: above it.
- Verified by: `life_write_amplification`.

#### ALFS-LIFE-03

On W-LOG, alfs erases no more sectors than the bytes it programmed fill.

- Priority: 2.
- Source: littlefs v2.11.3 erases 58.7 to 80.7 sectors a minute on W-LOG,
  v3 2.8 to 48.5 (the fork's Appendix B.1).
- Measure: erases over 10 minutes against the bytes programmed divided by
  the sector size, with deletes and erase steps as W-RING's.
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
  parts do.
- Measure: programs per page the simulator counts, under W-LOG at 1 row a
  second with prog unit 1.
- Pass: at most the configured number. Fail: more.
- Verified by: `life_partial_program_cap`.

#### ALFS-LIFE-07

Where a driver offers a larger erase (64 KB), an erase step uses it for a
run of dirty sectors that fills one aligned block.

- Priority: 2 (SHOULD).
- Source: UC-5: a 64 KB erase is about 6 times faster than 16 sector
  erases (4.3; `rp2040-datasheet_2025-02-20.pdf` p. 135).
- Measure: erase steps on a region whose every sector is dirty.
- Pass: one large erase per aligned block. Fail: sector erases there.
- Verified by: `life_large_erase`.

### 6.3 Safety (SAFE)

#### ALFS-SAFE-01

Every byte a successful sync made durable reads back intact after a power
loss at any later operation, under every power-loss behaviour of 4.4.

- Priority: 3.
- Source: UC-7.
- Measure: W-LOG and W-HR with power lost after operation k, for every k
  (littlefs's `-Plinear`), and for a sample of pairs; remount; read every
  stream.
- Pass: every synced byte reads back, in order. Fail: any missing or wrong.
- Verified by: `safe_powerloss_durable`.

#### ALFS-SAFE-02

A power loss loses at most the bytes appended since the last successful
sync.

- Priority: 3.
- Source: UC-7, FLT-LOG-06.
- Measure: as SAFE-01, counting bytes lost.
- Pass: at most those appended since the last sync. Fail: more.
- Verified by: `safe_powerloss_bound`.

#### ALFS-SAFE-03

A read never returns a byte from a chunk whose check failed. A failed
check makes the chunk's length untrustworthy too, so the read skips to the
next page, counts one failure, and reads on.

- Priority: 3.
- Source: UC-7; METASTABLE and the FLIP behaviours.
- Measure: streams with bits flipped in chunk headers, payloads and
  checks.
- Pass: no damaged byte returned; one failure counted for each page
  skipped; the chunks of the pages after it read. Fail: otherwise.
- Verified by: `safe_no_false_data`.

#### ALFS-SAFE-04

After a mount, a stream's first program is to a page no interrupted program
can have touched.

- Priority: 3.
- Source: A-3; TORNTAIL and METASTABLE. Its cost is at most one page per
  mount.
- Measure: the simulator flags a program into a page an interrupted
  operation touched.
- Pass: none. Fail: any.
- Verified by: `safe_no_program_after_torn`.

#### ALFS-SAFE-05

alfs appends only into a sector whose erase is known to have completed and
whose erased state it verified on the ground.

- Priority: 3.
- Source: the rulings; A-4; ERASENOOP.
- Measure: power loss during erase steps, and ERASENOOP sectors.
- Pass: no append lands in a sector whose erase was interrupted or did
  nothing. Fail: any.
- Verified by: `safe_append_only_verified_ready`.

#### ALFS-SAFE-06

A failed program loses none of the bytes it carried: they are written to
a fresh sector in a later call, since a page that fails is a sector that is
failing. After a configured number of failures in a row (default 3) they
are dropped and counted. A program is checked by reading it back.

- Priority: 3.
- Source: UC-9.
- Measure: PROGERROR, PROGNOOP and PROGFLIP sectors in the path of W-LOG.
- Pass: every byte is written or counted as lost; nothing is written twice;
  each call still does at most one operation (SPD-02). Fail: otherwise.
- Verified by: `safe_prog_failure`.

#### ALFS-SAFE-07

A sector that fails an erase, or its check after one, is never used again,
and the region's capacity shrinks by it.

- Priority: 3.
- Source: UC-10.
- Measure: ERASEERROR and ERASENOOP sectors, and wear-out, under W-RING.
- Pass: excluded and counted; after a remount it is dirty again, and its
  next erase step finds it bad again before anything is written to it.
  Marking it on the flash would mean programming bytes already programmed.
  Fail: an append or a stamp lands in it.
- Verified by: `safe_bad_sector`.

#### ALFS-SAFE-08

A failed read is returned to the caller, with the stream's position and
every other state unchanged, so that a retry reads the same bytes.

- Priority: 3.
- Source: A-7.
- Measure: a read that fails once.
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

Mount and read finish, without a crash, on any content of the region.

- Priority: 3.
- Source: UC-10; littlefs's crafted images.
- Measure: fuzzed regions (random sectors, valid headers with random
  fields), under a sanitizer.
- Pass: every call returns; no sanitizer error. Fail: otherwise.
- Verified by: `safe_fuzz_mount_read`.

#### ALFS-SAFE-11

A delete is atomic: once it returns, the stream is gone after any power
loss; before it returns, the stream is whole.

- Priority: 3.
- Source: UC-4.
- Measure: power loss at every operation of a delete; remount.
- Pass: the stream is whole or gone, never partial. Fail: partial.
- Verified by: `safe_delete_atomic`.

### 6.4 Simplicity (SIMP)

#### ALFS-SIMP-01

One C99 source file and one header, with no dynamic memory, no recursion,
no floating point, and no library beyond `<stdint.h>`, `<stddef.h>`,
`<stdbool.h>` and `<string.h>`.

- Priority: S.
- Measure: the build and a review.
- Pass: as stated. Fail: otherwise.
- Verified by: `simp_build` (the build compiles `alfs.c` alone, with
  `-std=c99 -Wall -Wextra -Werror -pedantic`); review.

#### ALFS-SIMP-02

The code is at most 4 KiB on a Cortex-M0+ (Thumb, `-Os`).

- Priority: S.
- Source: the brief ("Simplicity will keep the code fast and flash
  overhead cheap"). A budget, to be revisited with a measurement of
  littlefs on the same build.
- Measure: `arm-none-eabi-size` of `alfs.o`, text plus data.
- Pass: at most 4,096 bytes. Fail: more.
- Verified by: `simp_code_size`.

#### ALFS-SIMP-03

RAM does not grow with the region: an instance is at most 64 bytes, and an
open stream at most one page plus 32 bytes, both supplied by the caller.

- Priority: S.
- Source: pyro's Lua boards have about 13.5 kB of SRAM spare (measured
  2026-10-14, pyro's `docs/flash_stall_options.md` section 6.3), and a
  region of 8 MiB must not cost more than a small one.
- Measure: `sizeof` of the instance and stream structures, page buffers
  excluded.
- Pass: at most 64 and 32 bytes. Fail: more.
- Verified by: `simp_ram`.

#### ALFS-SIMP-04

No call uses more than 256 bytes of stack.

- Priority: S.
- Measure: `-fstack-usage` on the Cortex-M0+ build, worst path.
- Pass: at most 256. Fail: more.
- Verified by: `simp_stack`.

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
erase one sector (which may return `ALFS_BUSY`), and an optional larger
erase.

- Priority: S.
- Verified by: the simulator implements exactly these.

#### ALFS-IF-02

The geometry is configured: sector and page size (powers of two, sector at
least four pages), prog unit (a power of two up to a page), the region's
start and length (sector-aligned), the chip's size, and the erase value.
alfs never programs a prog unit twice between erases.

- Priority: S.
- Verified by: `if_geometry` (every combination the tests use is
  accepted; misaligned or oversized ones are refused); every test (the
  simulator refuses a program into a prog unit already programmed).

#### ALFS-IF-03

The on-disk format is little-endian and read and written byte by byte, so
any host reads any image.

- Priority: S.
- Verified by: `if_endian` (an image written on the host reads back under
  a big-endian build).

#### ALFS-IF-04

A stream is identified by a 32-bit tag the caller chooses (a flight number)
and an identity alfs assigns, unique for the life of the region.

- Priority: S.
- Source: UC-4, UC-6.
- Verified by: `if_list_streams`.

### 6.6 Observability and errors (OBS)

#### ALFS-OBS-01

alfs reports, at any moment: sectors ready, dirty and bad; bytes appendable
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
- Verified by: review of `alfs.h`.

#### ALFS-OBS-03

alfs reports whether any dirty sector remains, so the caller can hold a
launch until all unused flash is erased.

- Priority: 3.
- Source: UC-5; the first ruling.
- Verified by: `obs_clean` (false while a dirty sector exists, true once the
  erase steps have cleared them all).

---

## 7. Traceability

| Use case | Requirements |
|---|---|
| UC-1 flight log | SPD-01 to SPD-05, SPD-08, SPD-09, LIFE-02, LIFE-03 |
| UC-2 high-rate log | SPD-02, SPD-03, SPD-08, LIFE-02 |
| UC-3 resume | SPD-06, SPD-07, SAFE-04 |
| UC-4 newest flights | IF-04, SAFE-11 |
| UC-5 erase before flight | SPD-05, LIFE-01, LIFE-07, SAFE-05, OBS-01, OBS-03 |
| UC-6 read back | SAFE-03, SAFE-08, IF-04 |
| UC-7 power cut | SAFE-01, SAFE-02, SAFE-04 |
| UC-8 wrong geometry | SAFE-09, IF-02 |
| UC-9 write failures | SAFE-06, OBS-01 |
| UC-10 unattended flash | LIFE-04, LIFE-05, SAFE-07, SAFE-10, OBS-01, OBS-02 |

---

## 8. Sources

- `docs/datasheets/` and its `README.md`: the four flash parts and the
  RP2040.
- The littlefs fork (github.com/n9wxu/littlefs), branch `v3-integration`
  at 58367256: `REQUIREMENTS.md` sections 4-5 and Appendix B,
  `benches/bench_wlog.toml`, `bd/lfs3_emubd.h`, `lfs3.h`.
- The pyro firmware (github.com/n9wxu/pyro_fw) at v2.2.8: `DECISIONS.md`
  DD-035, DD-074, DD-077, DD-086, DD-098; `REQUIREMENTS.md` FLT-LOG-06;
  `docs/outstanding_tasks.md` L2, L4, C6;
  `docs/flash_stall_options.md`.
