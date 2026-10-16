# alfs test plan

**Status: draft 3.** Every requirement in `docs/REQUIREMENTS.md` names its
test; this document says how the tests work and what each one does.

---

## 1. How to run

| Command | What |
|---|---|
| `make test` | the core's host suite on 8 simulator seeds, under the address and undefined-behaviour sanitizers |
| `make test T=safe_` | the tests whose names contain `safe_` |
| `make test SEEDS="7"` | on the seeds given |
| `make soak` | the core's suite on 100 seeds, without the sanitizers |
| `make test-rt` | the flash task's suite on the FreeRTOS POSIX port; fetches FreeRTOS-Kernel V11.3.1 into `build/_deps` the first time |
| `make target` | the core's Cortex-M0+ budgets: code size (SIMP-02), structure sizes (SIMP-03), stack frames (SIMP-04) |
| `make target-rt` | the task layer's code size and stack, built against the kernel's generic Cortex-M0 port |
| `make tests-compile` | the tests compiled alone, before the code exists |
| `make probe` | the flash probe, an RP2040 program that runs from RAM (section 7) |
| `probe/run_probe.py` | runs the probe on attached boards and saves what it measured |

A run passes when every test passes on every seed, no sanitizer reports,
and the target checks pass.

**Seeds.** Every random choice the simulator makes comes from one seed:
- the chip's starting contents;
- which bit a torn operation tears;
- what a bit that reads either way reads.

One seed reaches only some of the ways a torn bit can read, so every test
runs on several. The test binary takes its seed from `ALFS_SEED`. `make
test` runs seeds 1 to 8 and `make soak` 1 to 100. A failure prints its
seed, so it can be run again alone.

---

## 2. The simulated flash (`tests/sim.c`)

A serial NOR chip held in RAM, with the geometry of the W25Q128JV by
default: 4,096-byte sectors, 256-byte pages, an optional 64 KB large erase,
erase value 0xFF. Its contents start random, as a used chip's are.

**It enforces the rules alfs lives by.** Each break is counted as a
violation, and every test fails on any:

- a program that crosses a page (SPD-03);
- a byte programmed again between erases (IF-02, the rulings), unless it is
  given the value the programs before it meant, or 00h (settling, A-9);
- a program of a byte that an interrupted operation touched (SAFE-04),
  with the same exception. A torn program touches only the bytes it was
  given (A-3; in the probe's P4 no neighbouring byte changes).

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
  requirements 4.3 (0.4 ms a program, 45 ms an erase, 150 ms a large
  erase), and the longest single operation in each call;
- programs per page, erases per sector and programs per sector;
- erases of a sector that held only its stamp (LIFE-01).

---

## 3. The rig (`tests/test_alfs.c`)

Every alfs call goes through one of two wrappers, which hold it to the
speed requirements as it runs:

| Wrapper | For | Holds it to |
|---|---|---|
| `CALL` | every call but an erase or a format | no erase (SPD-01); records the longest single operation (SPD-08) |
| `ERASE`, `FORMAT` | `alfs_erase`, `alfs_format` | none; the erase count is the test's to check |

`DONE` ends every test: it fails the test on any flash violation or any
call that broke its bound. So SPD-01, SPD-03 and IF-02 are checked by every
test, not only the ones named for them.

The region starts one sector into the chip (one large block, where large
erases are tested), so that no address is ever taken as an offset.

**Rows.** A 22-byte row carries its sequence number and a CRC-32, so a
test can tell good data from bad without alfs's help.

**Workloads** (requirements 4.5):

- `log_run`: W-LOG. Rows at 1 or 50 a second, written every 200 ms, synced
  every second, and an erase step every 20 ms when asked. It records every
  byte alfs took and how many a successful sync covered.
- W-HR: 480 bytes every 20 ms, synced every second.
- `ring`: W-RING. W-LOG at 50 rows a second, a new version every `every`
  rows, the oldest deleted while fewer than two logs' worth of sectors are
  ready, an erase step every 20 ms, until every sector has been erased a
  number of times. Run on a 32-sector region so it cycles quickly.
- `cfg_save`: W-CFG. A 300-byte configuration, each byte a function of a
  generation number, created as a new version of `config`, closed, and
  then every older version deleted.

---

## 4. The core's tests

| Test | Requirement | What it does |
|---|---|---|
| `sim_torn_obeys_a9` | 4.4 | Each power-loss behaviour, 500 torn programs of random data into erased bytes and over programmed ones: no bit the data left 1 ever reads 0, on any of 16 reads. |
| `if_geometry` | IF-02 | Formats and mounts; refuses seven bad geometries (a page under 128 bytes among them) and a bad erase value with no operation; refuses to mount with another sector or region size. |
| `if_endian` | IF-03, SIMP-05 | Checks a stamp's, a first claim's, a name's, a close record's and a chunk's bytes against `docs/SPEC.md`: the magic, version 3, the geometry, the words little-endian, the checks. |
| `if_names` | IF-04, IF-06 | Names of 1 and 31 bytes, and one with `/`, created, listed and opened, before and after a remount; 0 and 32 bytes, and none, refused with no operation; a name that is a prefix of another is not opened by it, nor is a version under another name. A C string cannot hold 00h, so that rule needs no test. |
| `if_list_files` | IF-08 | Three names, two versions of one, one deleted: listed in growing version, with names, closed flags and sizes; an unclosed file's size unknown; the same after a remount. |
| `if_version_never_reused` | IF-07 | Versions grow across names; deleting the newest version of a name and creating it again gives a new number; a create torn by FLICKER, METASTABLE or SOMEBITS, then eight remounts and a new create: no two files share a version. |
| `if_open_version0` | IF-09 | Versions 1 to 3 of a name, the newest left unclosed: version 0 opens the second; an explicit version opens the unclosed one. |
| `if_closed_read_only` | IF-11 | A write through a closed handle, or a read handle, is refused; there is no call that reopens a file for writing. |
| `if_delete_open_refused` | IF-10 | Delete refused while a reader or the writer has the version open; mount and format refused while any handle is open; all allowed once closed. |
| `if_empty_file` | IF-08, SAFE-13 | A file created and closed with no bytes lists closed with size 0 and reads 0 bytes, before and after a remount. |
| `basic_round_trip` | IF-12 | Seven sizes, 1 byte to 20,000, with blocks of one page and of 4,096 bytes: written, read, remounted, read. |
| `spd_no_erase_outside_erase` | SPD-01 | W-LOG at both rates, two minutes, with erase steps. |
| `spd_write_burst_bounded` | SPD-02, IF-12 | 30 seconds of 480-byte writes (W-HR) and of 22-byte writes, with blocks of one page and of 4,096 bytes: every write, sync and close within its program bound for the bytes it programmed. |
| `spd_enospc_without_erase` | SPD-04 | Fills the ready sectors: `ALFS_ENOSPC` with no erase, from write and sync; one erase step later the held bytes are written and read back. |
| `spd_mount_reads_bounded` | SPD-06 | Four files on 128 sectors: a mount reads at most 60 bytes a sector and does no operation. |
| `spd_wlog_longest_stall` | SPD-08 | Ten minutes of W-LOG at 50 rows a second: no operation longer than one program. |
| `spd_cpu_linear` | SPD-09 | The same writes and syncs on 32 and 2,048 sectors: the same reads, and about the same processor time. |
| `spd_read_next_bounded` | SPD-10 | Two files of 200 sectors written together on 512 sectors, and one alone: reading one of the pair reads at most one header more a sector than reading the one alone. |
| `life_erase_only_dirty` | LIFE-01 | W-RING for three cycles: no erase of a sector that held only its stamp. |
| `life_write_amplification` | LIFE-02 | Ten minutes of W-LOG at both rates, and W-HR, against the bound computed from the simulator's own counts. |
| `life_wlog_erases` | LIFE-03 | Ten minutes of W-LOG at 50 rows a second, deleted and erased: erases at most the sectors the log filled, plus one. Prints the count. |
| `life_wear_level` | LIFE-04 | W-RING until every sector is erased ten times: counts within 2. |
| `life_erase_count_persists` | LIFE-05 | W-RING, a remount: the least and greatest counts alfs reports equal the simulator's. |
| `life_partial_program_cap` | LIFE-06 | 1,000 three-byte syncs: at most 16 programs a page, and at most 4 when configured so. |
| `life_large_erase` | LIFE-07 | Four 64 KB blocks, all dirty but the formatted sector: three large erases and 16 sector erases. |
| `safe_powerloss_durable` | SAFE-01 | Each behaviour, power lost at every operation of a 20-second W-LOG with erase steps. Then the rest of the case runs as below. |
| `safe_powerloss_bound` | SAFE-02 | The same, with a second power loss after the restart in every third case. |
| `safe_no_program_after_torn` | SAFE-04 | The same runs, for the simulator's check that nothing programs a page a power loss touched. |
| `safe_no_false_data` | SAFE-03 | 200 trials of one bit flipped in a chunk: what reads back is the file with at most one run cut out, and one failure counted when one was. |
| `safe_write_only_verified_ready` | SAFE-05 | An ERASENOOP sector is found out by its erase and never programmed. Power lost in erases is covered by the power-loss runs. |
| `safe_prog_failure` | SAFE-06 | The sector being written goes PROGERROR, PROGNOOP or PROGFLIP: every row reads back once, none lost, in one call each. With no sector that programs: `ALFS_ELOST` after three tries, and the bytes counted. |
| `safe_bad_sector` | SAFE-07 | ERASEERROR and ERASENOOP sectors are counted bad, found bad again after a remount, never programmed. Wear-out under W-RING: logging goes on until the flash is worn. |
| `safe_read_retry` | SAFE-08 | Reads that fail once, in mount, open and read: each retried call gets the right bytes. |
| `safe_foreign_region` | SAFE-09 | Random and blank regions: `ALFS_ENOFS`. Another sector size, and a region past the chip: `ALFS_EGEOMETRY`. No operation in any. |
| `safe_fuzz_mount_read` | SAFE-10, SPD-11 | 400 images with bits flipped, headers randomised, headers copied, or everything random but one stamp: mount, list, open and read every file, create, write, close, delete one, erase until clean, under the sanitizers. |
| `safe_delete_atomic` | SAFE-11 | Power lost in a delete, each behaviour: the version whole or gone. A delete that returned survives a remount; its sectors are dirty. |
| `safe_delete_reclaim` | SAFE-11 | Power lost in a delete under FLICKER, remounts until it reads deleted, then power lost at every operation of the erases that reclaim it: the version whole or gone through eight remounts. |
| `safe_missing_sector` | SAFE-12 | The third of five sectors of a file made dirty: the other four read in order, one failure counted. |
| `safe_create_atomic` | SAFE-13 | Power lost in a create's program, each behaviour: after eight remounts, the version is absent, or empty and named as created, or nameless; a nameless one is deleted by its version. |
| `safe_close_atomic` | SAFE-14 | Power lost in a close's program, each behaviour: through eight remounts the file reads every byte written, and lists unclosed, or closed with its true size. |
| `safe_config_replace` | SAFE-14 | W-CFG with power lost at every operation of three saves, each behaviour. After each loss, eight remounts. Whenever a closed version existed before the loss, version 0 opens a whole configuration of a generation saved. |
| `obs_status` | OBS-01 | Counts against the simulator's, before and after logging and a remount. |
| `obs_clean` | OBS-03 | Not clean after a format, clean after erasing, not after a delete, clean again after erasing. |
| `simp_ram` | SIMP-03 | Structure sizes on the host, allowing for wider pointers; `make target` checks them exactly. |
| `make target` | SIMP-01, SIMP-02, SIMP-04 | Builds `alfs.c` alone with `-std=c99 -Wall -Wextra -Werror -pedantic` for the Cortex-M0+; code at most 7,168 bytes; no chain of stack frames under a public call over 320 bytes, from GCC's call graph. |

**The power-loss runs, after the loss.**
1. Remount; every synced byte of the log reads back, nothing beyond what
   was written.
2. A new version of the log is created and written, perhaps with a second
   loss.
3. Remount, erase and read both versions, twice. The old version must read
   a prefix of what was written before the loss, at least what was synced.
   The new one must read a prefix of what was written after the restart,
   at least what was synced since.

**By review:** SPD-11 (each loop's bound), SIMP-01 (the libraries used),
SIMP-05 (the spec covers every field), OBS-02 (every error documented in
`alfs.h` and `alfs_rtos.h`), RT-04 and RT-05.

**Deferred until a target build exists:** SPD-09's 100 us on an RP2040
(`target_write_256`), and IF-03's big-endian build.

---

## 5. The flash task's tests (`tests/test_rtos.c`)

They run on FreeRTOS-Kernel V11.3.1's POSIX port, single-core, with
`configSUPPORT_DYNAMIC_ALLOCATION` 0 as pyro builds it. The kernel is not
in this repository. `make test-rt` clones its tag into `build/_deps` under
the Makefile variable `ALFS_RT_KERNEL`. It deliberately does not read
`FREERTOS_KERNEL_PATH`, which may name another copy.

The driver they give alfs is the simulator's, wrapped. The wrapper records
which task each operation runs in and fails the test if two overlap or if
one runs outside the flash task once the scheduler is up.

| Test | Requirement | What it does |
|---|---|---|
| `rt_round_trip` | RT-02, RT-05 | Mount, create, write, close, open, read, delete, list, through requests; the flash task runs at the priority given. |
| `rt_serialises` | RT-01 | Three writer tasks and a reader on four files at once, with erase steps between: every file reads back whole; no operation outside the flash task, none overlapping. |
| `rt_fast_path` | RT-03 | Writes smaller than the block's room, and reads from what the block holds, send no request; the write that fills the block sends one. |
| `rt_before_scheduler` | RT-06 | Format, mount, create and write before the scheduler starts run directly, and the file reads back after it starts. |
| `rt_reentry_refused` | RT-06 | A driver that calls alfs from inside an operation is refused with no effect; a handle used from another task is refused. |
| `rt_erase_interleaves` | RT-07 | An erase of the whole region requested by one task; a write and sync requested by another meanwhile complete before the erase does. |
| `rt_read_while_writing` | RT-02 | One task writes and syncs a file while another reads it: the reader gets a prefix of what was synced, in order. |

**Not tested here:** priorities and core affinity under SMP. The POSIX port
has one core, so pyro's two-core scheduling is not reproduced.

---

## 6. Known limits of the tests

- SIMP-04 sums frames along GCC's static call graph. alfs has no
  recursion and calls nothing through a pointer but the driver, whose
  frames the caller adds.
- The power-loss runs are linear: one loss at each operation, and a second
  in a third of them. Schedules of three or more losses are not run.
- Timing is simulated at typical figures. The worst-case figures of
  requirements 4.3 scale every stall by the same factor and change no
  pass or fail.

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
