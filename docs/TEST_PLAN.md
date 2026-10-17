# alfs test plan

**Status: draft 6.** Every requirement in `docs/REQUIREMENTS.md` names its
test; this document says how the tests work and what each one does. The
tests are written against `alfs_fs.h` and `docs/SPEC.md`; the store
(`alfs_fs.c`) is not written yet, so `make tests-compile` is what runs.

---

## 1. How to run

| Command | What |
|---|---|
| `make tests-compile` | the tests compiled alone, against the header |
| `make test` | the host suite on 8 simulator seeds, under the address and undefined-behaviour sanitizers |
| `make test T=safe_` | the tests whose names contain `safe_` |
| `make test SEEDS="7"` | on the seeds given |
| `make soak` | the suite on 100 seeds, without the sanitizers |
| `make target` | the Cortex-M0+ budgets: code size (SIMP-02), structure sizes (SIMP-03), stack frames (SIMP-04) |
| `make probe` | the flash probe, an RP2040 program that runs from RAM (section 7) |
| `probe/run_probe.py` | runs the probe on attached boards and saves what it measured |

A run passes when every test passes on every seed, no sanitizer reports,
and the target checks pass.

**Seeds.** Every random choice the simulator makes comes from one seed:
the chip's starting contents, which bit a torn operation tears, and what a
bit that reads either way reads. The test binary takes its seed from
`ALFS_SEED`. `make test` runs seeds 1 to 8 and `make soak` 1 to 100.

---

## 2. The simulated flash (`tests/sim.c`)

A serial NOR chip held in RAM, with the geometry of the W25Q128JV by
default: 4,096-byte sectors, 256-byte pages, erase value 0xFF. Its
contents start random, as a used chip's are.

**It enforces the rules alfs lives by.** Each break is counted as a
violation, and every test fails on any:

- a program that crosses a page (SPD-03);
- a byte programmed again between erases (IF-02, the rulings), unless it is
  given the value the programs before it meant, or 00h (settling, A-9);
- a program of a byte that an interrupted operation touched, with the
  same exception. A torn program touches only the bytes it was given (A-3;
  in the probe's P4 no neighbouring byte changes). A test of SAFE-04, which
  writes a page that reads erased as erased, sets `torn_ok`: such programs
  are then counted, not flagged.

**It injects the faults of requirements section 4.4:**

- **Power loss.** `sim_power_after(n, behaviour)` interrupts the (n+1)th
  operation from now, then turns the chip off: every later operation fails
  until `sim_reboot`.
  - The behaviours are ATOMIC, SOMEBITS, MOSTBITS, METASTABLE and TORNTAIL
    as the littlefs fork's emulated device defines them, limited to bits
    the program's data moves (A-9), and FLICKER, the probe's early abort.
  - Unsettled bits read randomly until their sector is erased or a program
    settles them.
- **Bad sectors.** `sim_mkbad(sector, mode)`: PROGERROR, ERASEERROR,
  READERROR, PROGNOOP, ERASENOOP, PROGFLIP, READFLIP.
- **Wear-out.** `wear_limit`: a sector erased that often fails its next
  erase.
- **A read that fails once,** then succeeds (A-7): `read_fail_once`.
- **Bit flips** at any address: `sim_flip`.

**A program is an AND** (A-8): each byte becomes its old value AND the
data. A torn program's bit that reads either way (METASTABLE, FLICKER) is
one its data clears, as in the probe's P8. It settles at 0 when a
program gives it 0 (A-9), and stays unsettled when given 1.

**It measures:**
- totals and per-call counts of operations, programs, erases and bytes
  read;
- the simulated time of each operation at the typical times of
  requirements 4.3 (0.4 ms a program, 45 ms an erase), and the longest
  single operation in each call;
- programs per page, erases per sector and programs per sector.

---

## 3. The rig (`tests/test_alfs.c`)

**The chip** holds a gap sector, the area, and another gap sector. The gaps
hold random bytes that every test checks are unchanged at its end
(SAFE-09).

**The driver alfs gets** is the simulator's, wrapped. The wrapper records
each program of the current call, its address and length, and counts every
erase of a sector that already read erased (LIFE-01).

**Every call goes through a wrapper,** which holds it to the requirements
as it runs:

| Wrapper | For | Holds it to |
|---|---|---|
| `CALL` | every call SPD-01 does not let erase | no erase (SPD-01); records the longest single operation (SPD-06) |
| `ECALL` | allocation, close, the open of a closed file, cleanup, format | none; the erases are the test's to check |

`DONE` ends every test: it fails the test on any flash violation, any call
that broke its bound, any erase of a sector that read erased, or any
change to a gap sector. So SPD-01, SPD-03, IF-02, LIFE-01 and SAFE-09 are
checked by every test.

**Byte checks** read the simulator's flash as `docs/SPEC.md` lays it out:
the catalog in use, an entry, a file's list by its links.

**Rows.** A W-LOG row is 22 bytes and carries its sequence number and a
CRC-32, so a test can tell good data from bad without alfs's help.

**Workloads** (requirements 4.5):

- `log_run`: W-LOG into a file opened for writing: rows at 1 or 50 a
  second, written every 200 ms, synced every second, and a cleanup step
  every 20 ms when asked. It records every byte alfs took and how many a
  successful sync covered.
- W-HR: 480 bytes every 20 ms, synced every second, into a reserved file.
- `ring`: W-RING. A file of its own each `every` rows, the oldest deleted
  while fewer than two files' worth of pages are free, a cleanup step every
  20 ms; each file read back whole after its close.
- `cfg_step`: W-CFG. Updates of a settings file and two Lua programs, each
  by blocking out the filename and writing a new file.

---

## 4. The tests

| Test | Requirement | What it does |
|---|---|---|
| `sim_torn_obeys_a9` | 4.4 | Each power-loss behaviour, 500 torn programs of random data into erased bytes and over programmed ones: no bit the data left 1 ever reads 0, on any of 16 reads. |
| `if_geometry` | IF-02 | Refuses eight bad geometries, at format and at mount, with no operation. An area formatted for 8 sectors does not mount as 6. |
| `if_format_mount` | IF-16 | A format: the catalog in sector 0, no file, 80 slots; the frame sectors not clean until a cleanup, then every page free; a mount reads and writes nothing. |
| `if_endian` | IF-03, SIMP-05 | A closed 300-byte file: the catalog's stamp and entry, both pages' headers and links, the frame block and its close fields, the data and its checks, against `docs/SPEC.md`. |
| `if_names` | IF-05 | Filenames of 1 and 31 bytes, and one with `/`, written and read, before and after a remount; 0 and 32 bytes, and none, refused with no operation; a prefix of a filename does not open it; a second allocation of a filename refused with `ALFS_EEXIST`. |
| `if_alloc` | IF-06, SPD-10 | An allocation into free pages: R + 1 programs, no erase; R pages linked, each with no data, the last link erased, the close fields erased. An allocation into an area of dead pages erases only what it needs, and a live file reads on. |
| `if_write_pages` | IF-08, IF-09, SPD-02 | 50-byte writes with blocks of one page and of a sector: every program a whole page's data and its check; none while the block has room; after each sync, the file reads its complete pages and the block holds less than a page. |
| `if_close` | IF-10, LIFE-05 | A 40-page reservation closed after 20: the close fields and the zero link; the sectors of unused pages that hold no live page erased, the unused pages of a sector holding live ones kept; the file reads, before and after a remount. |
| `if_grow` | SPD-04, IF-08 | Without `ALFS_GROW`: the reservation's bytes taken, then `ALFS_ENOSPC`, no erase, no page taken. With it: the list grows into free pages; and, when none is left, `ALFS_ENOSPC` with no erase, the bytes taken written after a cleanup frees a sector. |
| `if_reopen_unclosed` | IF-07 | Three pages synced and 100 bytes in the block, then a remount: the reopen goes on at the next page, and the file reads the synced pages and the new bytes. |
| `if_reopen_closed` | IF-07 | A closed file reopened with a sector's block (a page's is refused, with no operation), written on and closed: it reads old and new, and the file beside it in the rewritten sector reads whole. |
| `if_read` | IF-11 | Eight sizes, 0 to 20,000 bytes, written, read, remounted, read; a file not closed reads its complete pages. |
| `if_delete` | IF-12, SAFE-13 | A delete zeros the filename in the entry and the frame block, erases nothing; the file is gone after a remount; deletes of a file open for writing or reading refused; a Lua update by delete and write. |
| `if_list` | IF-13 | Three files, one not closed: frame IDs growing, filenames, closed flags, sizes and page counts, the same after a remount. |
| `if_ids_never_reused` | IF-14 | Allocations and deletes across remounts; an allocation's entry torn by FLICKER, METASTABLE and SOMEBITS, each followed by eight remounts and allocations: no frame ID twice. |
| `if_cleanup` | IF-15, LIFE-01 | A deleted file beside a live one: cleanup erases the sectors holding no live page, keeps the shared one, reads the live file whole, then `ALFS_CLEAN` with no erase. |
| `if_refusals` | IF-17 | A second open of a file and a mount refused while it is open; an allocation, a delete and a cleanup meanwhile leave it to be written on and closed whole. |
| `spd_no_erase_outside_erase` | SPD-01 | Two minutes of W-LOG at both rates into a reserved file, a remount, a reopen and more, a close; W-CFG; reads and status: no erase in any call `CALL` wraps. |
| `spd_write_programs_bounded` | SPD-02 | 30 seconds of W-HR with blocks of a page and a sector: every program a whole page; none when the block did not fill; at most the complete pages held; after a sync less than a page held. |
| `spd_mount_reads_bounded` | SPD-05 | A mount of a 128-sector area holding twenty files, some deleted: at most two sectors read, no operation. |
| `spd_wlog_longest_stall` | SPD-06 | Ten minutes of W-LOG at 50 rows a second: no operation longer than one program. |
| `spd_cpu_linear` | SPD-07 | The same writes and syncs into a reserved file on 34 and 2,048 sectors read the same bytes. |
| `spd_open_reads_bounded` | SPD-08 | Files of 1, 10 and 100 pages: an open for writing reads at most the catalog, a page and a header a page; for reading, the catalog and a page; a reopen after a remount, a page's data a page more. |
| `life_erase_only_unused` | LIFE-01 | W-RING for three cycles and W-CFG: every file read back whole after its close; no erase of a sector that read erased. |
| `life_write_amplification` | LIFE-02 | W-LOG at both rates and W-HR into reserved files: the bytes an allocation, the writes and syncs, and the close program equal the figures of LIFE-02. |
| `life_wlog_erases` | LIFE-03 | Ten minutes of W-LOG at 50 rows a second into a file allocated to fit: its allocation, close, delete and cleanup erase at most the sectors its pages fill, plus one. Prints the count. |
| `life_wear_level` | LIFE-04 | W-RING until every frame sector is erased ten times: counts within 2. |
| `safe_powerloss_durable` | SAFE-01 | Each behaviour, power lost at every operation of a 20-second W-LOG and its close, beside a closed settings file: through three remounts the settings file reads whole. |
| `safe_powerloss_bound` | SAFE-02 | The same runs: the log reads a prefix of what was written, at least the complete pages of the last sync. |
| `safe_reopen_after_loss` | SAFE-04 | Each behaviour, power lost at every operation of writing a file; reopened, written on, closed: what reads is the old bytes to some page's end, at least the synced complete pages, then the new bytes, perhaps but their first page. |
| `safe_no_false_data` | SAFE-03 | 200 trials of one bit flipped in a page's data or check: that page's bytes not returned, the rest in order, one failure counted. |
| `safe_write_only_verified_erased` | SAFE-05 | An ERASENOOP sector is found out by the cleanup's check and never given to a file, nor programmed. |
| `safe_prog_failure` | SAFE-06 | The sector being written goes PROGERROR, PROGNOOP or PROGFLIP: the bytes read and the bytes counted lost add up to those synced, and what reads is what was written with runs cut out. A PROGERROR sector met by an allocation's headers: the file is written and read whole. |
| `safe_bad_sector` | SAFE-07 | ERASEERROR and ERASENOOP sectors counted bad, never programmed, a file filling the rest; wear-out under W-RING: the area shrinks and writing goes on until it holds no file. |
| `safe_read_retry` | SAFE-08 | Reads that fail once, in the mount, the open and the reads: each retried call gets the right bytes. |
| `safe_foreign_area` | SAFE-09 | Random and blank areas: `ALFS_ENOFS`, no operation. An area past the chip: `ALFS_EGEOMETRY`. |
| `safe_fuzz` | SAFE-10, SPD-09 | 300 images with bits flipped, headers with valid checks and links to themselves, the catalog, past the area or back to the start, pages copied, or every frame page random: every call returns, under the sanitizers. |
| `safe_alloc_atomic` | SAFE-11 | Each behaviour, power lost at every operation of an allocation: through eight remounts the filename names no file or one whose every page is in its list, and one frame ID at most; then the file is written and read whole. |
| `safe_close_atomic` | SAFE-12 | Each behaviour, power lost at every operation of a close that erases: through eight remounts the file reads as not closed, its complete pages, or closed, whole. |
| `safe_delete_atomic` | SAFE-13 | Each behaviour, power lost at every operation of a delete: before it returns the file reads whole or not at all; once it returns, never again; the other file whole throughout. |
| `safe_frame_check` | SAFE-14 | A closed file with a byte changed and its page's check made to match: every byte read, then `ALFS_ECHECK`. |
| `safe_links_checked` | SAFE-15 | A link bent to another file's page, the catalog, past the area, or the file's own start: the read ends there with the bytes before it, a failure is counted, and the other file reads whole. |
| `safe_catalog_rewrite_atomic` | SAFE-16 | Each behaviour, power lost at every operation of the allocation that rewrites a full catalog: the three live files read whole through four remounts; once it returns, the other sector is the catalog. |
| `safe_reopen_closed_cut` | SAFE-17 | Each behaviour, power lost at every operation of reopening, writing and closing a closed file: a file outside the rewritten sector reads whole; the two inside read no byte they never held. |
| `obs_status` | OBS-01 | Live, free and dead pages, catalog entries and counts against the simulator's, after files are written and one deleted, and after a remount. |
| `obs_clean` | OBS-03 | Not clean after a format and after a delete; clean after a cleanup, a file filling its reservation, and a remount. |
| `simp_ram` | SIMP-03 | Structure sizes on the host, allowing for wider pointers; `make target` checks them exactly. |
| `make target` | SIMP-01, SIMP-02, SIMP-04 | Builds the store with `-std=c99 -Wall -Wextra -Werror -pedantic` for the Cortex-M0+; code at most the budget; no chain of stack frames under a public call over 320 bytes. |

**By review:** SPD-09 (each loop's bound), SIMP-01 (the libraries used),
SIMP-05 (the spec covers every field), OBS-02 (every error documented in
the headers), IF-01, IF-04.

---

## 5. The flash task

Out of scope until the interface settles (requirements, section 2).

---

## 6. Known limits of the tests

- The power-loss runs are linear: one loss at each operation. Schedules of
  two or more losses are not run.
- Timing is simulated at typical figures. The worst-case figures of
  requirements 4.3 scale every stall by the same factor and change no pass
  or fail.

---

## 7. The flash probe (`probe/`)

Assumption A-8 is a property of each chip, so the probe tests it on the
chips. It also measures how long programs and erases take there.

**How it runs.**
- `probe/run_probe.py` puts an attached board into BOOTSEL and loads
  `alfs_probe.uf2` into RAM. The probe never runs from flash, since it
  erases and programs that flash.
- It drives the flash interface directly, with its own commands: Read
  (03h), Page Program (02h) of any length within a page, Sector Erase
  (20h), Read Status (05h) and JEDEC ID (9Fh).
- A program's or erase's time is measured from the end of its command to
  the moment the status register shows it done.

**Where.** The last four sectors of the board's firmware-update download
slot, which no image reaches, unless `--addr` gives others. The probe
refuses to touch a sector that is not blank when it starts, and leaves
every sector it tested erased. Then it reboots the board into its own
firmware.

**What it does.** The probe runs 20 cycles. In each cycle, in each test
sector, it:
1. erases the sector and times the erase;
2. checks that all 4,096 bytes read back erased;
3. fills the sector's 16 pages as below, keeping a model of what every
   byte should hold: a programmed byte becomes its old value AND the data,
   and nothing else changes.

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

**P8, torn programs (A-9).** `run_probe.py --torn`.
- How it tears: the chips abort a program or erase under way when sent
  Enable Reset (66h) then Reset (99h) (W25Q128JV p. 60, XT25F128F p. 41,
  BY25Q64ES p. 30), which tears it as a power cut would.
- What it programs: in each test sector, pages 0-7 take 256-byte programs
  and pages 8-15 18-byte programs (a claim's length) of random data.
- When it aborts: after a delay swept across the program's time, 0 to
  500 us for 256 bytes and 0 to 120 us for 18. There are 32 steps a cycle,
  offset by a quarter step each cycle, over 4 cycles.
- What it records: after each abort the probe waits 1 ms and reads the
  page 16 times. It records whether the program had finished, not started,
  or been torn; the bits that read both ways; and any bit the data left 1
  that reads 0.
- Then it programs the same data again and reads the page 16 times more.

**Pass:**
- after the second program, every read is the data, on every trial;
- no bit the data left 1 ever read 0;
- the rest of the sector is as the model says;
- at least one program was torn, or the test proved nothing.

**P9, torn erases (A-4, SAFE-05).**
1. Each test sector is programmed 00h throughout, then erased, and the
   erase aborted after a delay swept from 0 to 60 ms (64 trials).
2. The probe reads the sector 16 times. It counts the trials whose sector
   read blank on the first read and not on some later one: the hazard a
   single blank check after an erase is exposed to.
3. It erases the sector again and checks it blank.

P9 measures; it passes when every sector comes back blank.

**What it does not test:** whether case-2 data keeps (retention), a real
power cut (P8 and P9 abort by command, with the supply steady), or more
than one chip of each part.

Results go to `docs/probe_results/`, and `docs/FLASH_PROBE.md` summarises
them part by part.
