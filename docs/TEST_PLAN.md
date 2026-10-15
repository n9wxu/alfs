# alfs test plan

**Status: draft 2, 2026-10-14.** Written after `docs/REQUIREMENTS.md` and
before the code. Draft 2 adds the tests of requirements draft 2. Every test passes on the first implementation. Every requirement names its test there; this document
says how the tests work and what each one does.

---

## 1. How to run

| Command | What |
|---|---|
| `make test` | the host suite, under the address and undefined-behaviour sanitizers |
| `make test T=safe_` | the tests whose names contain `safe_` |
| `make target` | the Cortex-M0+ budgets: code size (SIMP-02), structure sizes (SIMP-03), stack frames (SIMP-04) |
| `make tests-compile` | the tests compiled alone, before the code exists |
| `make probe` | the flash probe, an RP2040 program that runs from RAM (section 6) |
| `probe/run_probe.py` | runs the probe on attached boards and saves what it measured |

A run passes when every test passes, no sanitizer reports, and the
target checks pass.

---

## 2. The simulated flash (`tests/sim.c`)

A serial NOR chip held in RAM, with the geometry of the W25Q128JV by
default: 4,096-byte sectors, 256-byte pages, an optional 64 KB large erase,
erase value 0xFF. Its contents start random, as a used chip's are.

**It enforces the rules alfs lives by.** Each break is counted as a
violation, and every test fails on any:

- a program that crosses a page (SPD-03);
- a prog unit programmed twice between erases (IF-02, the rulings),
  unless the prog unit is 1 and every byte of the second program is the
  byte the programs before it meant, or 00h (settling a torn program, A-9);
- a program into a page that an interrupted operation touched (SAFE-04),
  with the same exception;
- a second erase started while one is suspended (SPD-05).

**It injects the faults of requirements section 4.4:**

- **Power loss.** `sim_power_after(n, behaviour)` interrupts the (n+1)th
  operation from now with one of ATOMIC, SOMEBITS, MOSTBITS, METASTABLE and
  TORNTAIL, as the littlefs fork's emulated device defines them, or
  FLICKER, the probe's early abort (requirements 4.4), and then turns the
  chip off: every later operation fails until `sim_reboot`. Unsettled bits
  read randomly until their sector is erased or a program settles them.
- **Bad sectors.** `sim_mkbad(sector, mode)`: PROGERROR, ERASEERROR,
  READERROR, PROGNOOP, ERASENOOP, PROGFLIP, READFLIP.
- **Wear-out.** `wear_limit`: a sector erased that often fails its next
  erase.
- **A read that fails once,** then succeeds (A-7): `read_fail_once`.
- **A suspended erase.** `busy_calls`: each erase takes that many calls,
  answering `ALFS_BUSY` until the last.
- **Bit flips** at any address: `sim_flip`.

**A program is an AND** (A-8): each byte becomes its old value AND the
data. A torn program's bit that reads either way (METASTABLE) is one its
data clears, as the probe found (P8); it settles at 0 when a program gives
it 0 (A-9), and stays unsettled when given 1. With a prog unit over 1 a
torn program leaves no such bit: the part corrects one bit a unit, which is
why it has that unit.

**It measures.** Totals and per-call counts of operations, programs,
erases and bytes read; the simulated time of each call at the typical
times of requirements 4.3 (0.4 ms a program, 45 ms an erase, 150 ms a
large erase); programs per page; erases per sector; programs per sector;
and erases of a sector that held only its stamp (LIFE-01).

---

## 3. The rig (`tests/test_alfs.c`)

Every alfs call goes through one of three wrappers, which hold it to the
speed requirements as it runs:

| Wrapper | For | Holds it to |
|---|---|---|
| `CALL` | every call but an erase step | at most one operation, and no erase (SPD-01, SPD-02); records the longest stall (SPD-08) |
| `ESTEP`, `FORMAT` | `alfs_erase_step`, `alfs_format` | at most one erase and one program (SPD-02) |

`DONE` ends every test: it fails the test on any flash violation or any
call that broke its bound. So SPD-01 to SPD-03 and IF-02 are checked by
every test, not only the ones named for them.

The region starts one sector into the chip (one large block, where large
erases are tested), so that no address is ever taken as an offset.

**Rows.** A 22-byte row carries its sequence number and a CRC-32, like
littlefs's W-LOG rows, so a test can tell good data from bad without
alfs's help.

**Workloads** (requirements 4.5):

- `log_run`: W-LOG. Rows at 1 or 50 a second, appended every 200 ms,
  synced every second, and an erase step every 20 ms when asked. It records
  every byte alfs took and how many a successful sync covered.
- W-HR: 480 bytes every 20 ms, synced every second.
- `ring`: W-RING. W-LOG at 50 rows a second, a new stream every `every`
  rows, the oldest deleted while fewer than two streams' worth of sectors
  are ready, an erase step every 20 ms, until every sector has been erased
  a number of times. Run on a 32-sector region so it cycles quickly.

---

## 4. The tests

| Test | Requirement | What it does |
|---|---|---|
| `if_geometry` | IF-02 | Formats and mounts with prog units 1, 16 and 256; refuses eight bad geometries and a bad erase value with no operation; refuses to mount with a prog unit or region size other than the format's. |
| `if_endian` | IF-03, SIMP-05 | Checks the stamp's and a claim's bytes against `docs/SPEC.md`: the magic, version 2, the geometry, the generation, erase count and sequence floor as little-endian words, the claim's fields, the CRCs. |
| `if_list_streams` | IF-04 | Five streams, one deleted: listed in growing identity, the deleted one missing, the same after a remount. |
| `basic_round_trip` | (all) | Seven sizes, 1 byte to 20,000, three prog units: written, read, remounted, read. |
| `spd_no_erase_outside_erase_step` | SPD-01 | W-LOG at both rates and three prog units, two minutes, with erase steps. |
| `spd_one_operation_per_call` | SPD-02 | W-HR for 30 seconds. |
| `spd_enospc_without_erase` | SPD-04 | Fills the ready sectors: `ALFS_ENOSPC` with no operation, from append and sync; one erase step later the held bytes are written and read back. |
| `spd_busy_erase_resumes` | SPD-05 | Erases that take five calls: format and erase steps return `ALFS_BUSY` 31 times and finish 8 sectors. |
| `spd_mount_reads_bounded` | SPD-06 | Four streams on 128 sectors: a mount reads at most 60 bytes a sector and does no operation. |
| `spd_reopen_reads_bounded` | SPD-07 | 3,000 one-byte syncs: reopening reads at most 60 bytes a sector and one sector more, and does no operation. |
| `spd_wlog_longest_stall` | SPD-08 | Ten minutes of W-LOG at 50 rows a second, three prog units: no call stalls longer than one program. |
| `spd_cpu_linear` | SPD-09 | The same appends and syncs on 32 and 2,048 sectors: the same reads, and about the same processor time. |
| `life_erase_only_dirty` | LIFE-01 | W-RING for three cycles: no erase of a sector that held only its stamp. |
| `life_write_amplification` | LIFE-02 | Ten minutes of W-LOG at both rates and three prog units, against the bound computed from the simulator's own counts. |
| `life_wlog_erases` | LIFE-03 | Ten minutes of W-LOG at 50 rows a second, deleted and cleaned: erases at most the sectors the log filled, plus one. Prints the count beside littlefs v2.11.3's 807. |
| `life_wear_level` | LIFE-04 | W-RING until every sector is erased ten times: counts within 2. |
| `life_erase_count_persists` | LIFE-05 | W-RING, a remount: the least and greatest counts alfs reports equal the simulator's. |
| `life_partial_program_cap` | LIFE-06 | 1,000 three-byte syncs: at most 16 programs a page, and at most 4 when configured so. |
| `life_large_erase` | LIFE-07 | Four 64 KB blocks, all dirty but the formatted sector: three large erases and 16 sector erases. |
| `safe_powerloss_durable` | SAFE-01 | For each behaviour and prog units 1 and 16, power lost at every operation of a 20-second W-LOG with erase steps; remount; every synced byte reads back, nothing beyond what was appended; resume; then remount, erase steps and read, twice. Each read must be a prefix of what was appended before the loss, at least what was synced, then a prefix of what was appended after the resume, at least what was synced since: bytes never synced may be lost, or come back later in their place. |
| `safe_powerloss_bound` | SAFE-02 | The same, with a second power loss during the resume in every third case. |
| `safe_no_program_after_torn` | SAFE-04 | The same runs, for the simulator's check that nothing programs a page a power loss touched. |
| `safe_no_false_data` | SAFE-03 | 200 trials of one bit flipped in a chunk: what reads back is the stream with at most one run cut out, and one failure counted when one was. |
| `safe_append_only_verified_ready` | SAFE-05 | An ERASENOOP sector is found out by its erase step and never programmed. Power lost in erase steps is covered by the power-loss runs. |
| `safe_prog_failure` | SAFE-06 | The sector being written goes PROGERROR, PROGNOOP or PROGFLIP: every row reads back once, none lost. With no sector that programs: two retries, then `ALFS_ELOST` and the bytes counted. |
| `safe_bad_sector` | SAFE-07 | ERASEERROR and ERASENOOP sectors are counted bad, found bad again after a remount, never programmed. Wear-out under W-RING: logging goes on until the flash is worn. |
| `safe_read_retry` | SAFE-08 | Reads that fail once, in mount, open and read: each retried call gets the right bytes. |
| `safe_foreign_region` | SAFE-09 | Random and blank regions: `ALFS_ENOFS`. Another sector size, and a region past the chip: `ALFS_EGEOMETRY`. No operation in any. |
| `safe_fuzz_mount_read` | SAFE-10 | 400 images with bits flipped, headers randomised, headers copied, or everything random but one stamp: mount, list and read every stream, under the sanitizers. |
| `safe_delete_atomic` | SAFE-11 | Power lost in a delete, each behaviour: the stream whole or gone. A delete that returned survives a remount; its sectors are dirty. |
| `spd_read_next_bounded` | SPD-10 | Two streams of 200 sectors written together on 512 sectors, and one alone: reading one of the pair reads at most one header more a sector than reading the one alone. |
| `life_resume_waste` | LIFE-08 | A stream reopened after its last chunk ends at each of 40 positions across a sector: bytes lost under two pages, and one program (the claim) before the first chunk. |
| `safe_delete_reclaim` | SAFE-11 | Power lost in a delete under METASTABLE, a remount, then power lost at every operation of the erase steps that reclaim it, and a remount: the stream whole or gone. The dead mark is programmed again before the first of its sectors is erased. |
| `safe_resume_torn_claim` | SAFE-04 | Power lost, under METASTABLE, in the claim of a stream's next sector; remount until that claim reads invalid; resume and fill two sectors; then eight remounts with erase steps between: every synced byte reads back, in order, and nothing from the torn sector. |
| `if_identity_never_reused` | IF-04 | Power lost, under METASTABLE, in a stream's first claim; remount; a new stream; eight remounts: no two streams share an identity, and the new one reads back whole. |
| `safe_missing_sector` | SAFE-12 | The third of five sectors of a stream made dirty: the other four read in order, one failure counted. |
| `obs_status` | OBS-01 | Counts against the simulator's, before and after logging and a remount. |
| `obs_clean` | OBS-03 | Not clean through a format and its suspended erases, clean after, not after a delete, clean again after erase steps. |
| `simp_ram` | SIMP-03 | Structure sizes on the host, allowing for wider pointers; `make target` checks them exactly. |
| `make target` | SIMP-01, SIMP-02, SIMP-04 | Builds `alfs.c` alone with `-std=c99 -Wall -Wextra -Werror -pedantic` for the Cortex-M0+; code at most 6,144 bytes; no chain of stack frames under a public call over 320 bytes, from GCC's call graph. |

**By review:** SPD-05 (no loop waits on the flash), SIMP-01 (the
libraries used), SIMP-05 (the spec covers every field), OBS-02 (every
error documented in `alfs.h`).

**Deferred until a target build exists:** SPD-09's 100 us on an RP2040
(`target_append_256`), and IF-03's big-endian build.

---

## 5. Known limits of the tests

- SIMP-04 sums frames along GCC's static call graph. alfs has no
  recursion and calls nothing through a pointer but the driver, whose
  frames the caller adds.
- The power-loss runs are linear: one loss at each operation, and a second
  in a third of them. littlefs's exhaustive schedules are not reproduced.
- Timing is simulated at typical figures. The worst-case figures of
  requirements 4.3 scale every stall by the same factor and change no
  pass or fail.

---

## 6. The flash probe (`probe/`)

Assumption A-8 is a property of each chip, so the probe tests it on the
chips. It also measures how long programs and erases take there.

**How it runs.** `probe/run_probe.py` puts an attached board into BOOTSEL
and loads `alfs_probe.uf2` into RAM. The probe never runs from flash,
since it erases and programs that flash. It drives the flash interface
directly, with its own commands: Read (03h), Page Program (02h) of any
length within a page, Sector Erase (20h), Read Status (05h) and JEDEC ID
(9Fh). A program's or erase's time is measured from the end of its command
to the moment the status register shows it done.

**Where.** The last four sectors of the board's firmware-update download
slot, which no image reaches, unless `--addr` gives others. The probe
refuses to touch a sector that is not blank when it starts, and leaves
every sector it tested erased. Then it reboots the board into its own
firmware.

**What it does.** For each of 20 cycles, and in each test sector, the
probe erases the sector, times the erase, and checks that all 4,096 bytes
read back erased. It then fills the sector's 16 pages as below, keeping a
model of what every byte should hold: a programmed byte becomes its old
value AND the data, and nothing else changes.

| Test | Pages | What |
|---|---|---|
| P5, many programs a page | 0 | 256 one-byte programs into erased bytes, a different byte each time (case 1) |
| P1, bit stepping | 1-4 | Each byte programmed eight times, each clearing one more bit: pages 1-2 send the cumulative value (FE, FC, ... 00), pages 3-4 a mask with one bit clear, in a random order (case 2) |
| P2, AND | 5-8 | Each page programmed whole three times, with random A, then B, then C: it must read A AND B AND C |
| P6, padded pages | 9-10 | 64 updates a page sent as whole-page programs: random values at 16 random places, FFh everywhere else, as a driver that programs only whole pages would (A-1) |
| P4, over-programming | 11 | One byte programmed 00h 1,000 times; pages 10-12 checked after every 100 |
| P7, timing | 12-14 | Programs of 1, 2, 4, 8, 16, 64 and 128 bytes into erased bytes (page 12), one of 256 (page 13), and 1- and 4-byte reprograms of programmed bytes (page 14) |
| control | 15 | Never programmed: it must stay erased |

After every program the probe reads back the page it programmed and
compares it with the model. After every cycle it compares the whole
sector.

**Pass:** every read equals the model and every erase reads back erased.
A program that takes longer than 10 ms, or an erase longer than 2 s, also
fails (the datasheets' maximums are 3 ms and 800 ms). The probe reports
every program's time, grouped by its length and by whether any byte was
already programmed, as count, minimum, mean and maximum.

**The probe can fail.** `run_probe.py --selftest` makes the probe flip one
bit of its model after the first P2 program. That run must report exactly
that mismatch.

**P8, torn programs (A-9).** `run_probe.py --torn`. The chips abort a
program or erase under way when sent Enable Reset (66h) then Reset (99h)
(W25Q128JV p. 60, XT25F128F p. 41, BY25Q64ES p. 30), which tears it as a
power cut would. In each test sector, pages 0-7 take 256-byte programs and
pages 8-15 18-byte programs (a claim's length) of random data. Each is
aborted after a delay swept across the program's time: 0 to 500 us for 256
bytes, 0 to 120 us for 18, in 32 steps a cycle, offset by a quarter step
each cycle over 4 cycles. After each abort the probe waits 1 ms, reads the
page 16 times, and records whether the program had finished, not started,
or been torn; the bits that read both ways; and any bit the data left 1
that reads 0. Then it programs the same data again and reads the page 16
times more.

**Pass:** after the second program every read is the data, on every
trial; no bit the data left 1 ever read 0; the rest of the sector is as
the model says; and at least one program was torn, or the test proved
nothing.

**P9, torn erases (A-4, SAFE-05).** Each test sector is programmed 00h
throughout, then erased, and the erase aborted after a delay swept from
0 to 60 ms (64 trials). The probe reads the sector 16 times and counts the
trials whose sector read blank on the first read and not on some later
one: the hazard an erase step's single blank check is exposed to. Then it
erases the sector again and checks it blank. P9 measures; it passes when
every sector comes back blank.

**What it does not test:** whether case-2 data keeps (retention), a real
power cut (P8 and P9 abort by command, with the supply steady), or more
than one chip of each part.

Results go to `docs/probe_results/`, and `docs/FLASH_PROBE.md` summarises
them part by part.

