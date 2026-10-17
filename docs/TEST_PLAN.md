# alfs test plan

**Status: draft 4.** Every requirement in `docs/REQUIREMENTS.md` names its
test; this document says how the tests work and what each one does.

---

## 1. How to run

| Command | What |
|---|---|
| `make test` | the stores' host suite on 8 simulator seeds, under the address and undefined-behaviour sanitizers |
| `make test T=safe_` | the tests whose names contain `safe_` |
| `make test SEEDS="7"` | on the seeds given |
| `make soak` | the stores' suite on 100 seeds, without the sanitizers |
| `make test-rt` | the flash task's suite on the FreeRTOS POSIX port; fetches FreeRTOS-Kernel V11.3.1 into `build/_deps` the first time |
| `make target` | the stores' Cortex-M0+ budgets: code size (SIMP-02), structure sizes (SIMP-03), stack frames (SIMP-04) |
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
default: 4,096-byte sectors, 256-byte pages, erase value 0xFF. Its
contents start random, as a used chip's are.

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
  requirements 4.3 (0.4 ms a program, 45 ms an erase), and the longest
  single operation in each call;
- programs per page, erases per sector and programs per sector;
- erases of a sector that held only its stamp (LIFE-01).

---

## 3. The rig (`tests/test_alfs.c`)

**The chip** holds a gap sector, the configuration store (three sectors),
the object area, the log area and another gap sector, in that order. The gaps hold random bytes that every test
checks are unchanged at its end (SAFE-09), and no address is ever taken
as an offset.

**The driver the stores get** is the simulator's, wrapped. The wrapper
counts, per call, the programs that start a sector (claims), the 12-byte
programs of 00h (zeroed claims), and the reads of a sector's first bytes
(headers).

**Every call goes through a wrapper,** which holds it to the speed
requirements as it runs:

| Wrapper | For | Holds it to |
|---|---|---|
| `CALL` | every call that may not erase | no erase (SPD-01); records the longest single operation (SPD-06) |
| `OCALL` | a put or delete | no erase when the spare half was ready before it (SPD-01, LIFE-01) |
| `KCALL` | a configuration set or delete | at most one erase, and only when the head moved to another sector (SPD-01, SPD-10) |
| `LERASE`, `OERASE`, `OFORMAT` | `alfs_log_erase`, `alfs_obj_erase`, `alfs_obj_format` | none; the erase count is the test's to check |

`DONE` ends every test: it fails the test on any flash violation, any call
that broke its bound, or any change to a gap sector. So SPD-01, SPD-03,
IF-02 and SAFE-09 are checked by every test, not only the ones named for
them.

**Rows.** A 22-byte row carries its sequence number and a CRC-32, so a
test can tell good data from bad without alfs's help.

**Workloads** (requirements 4.5):

- `log_run`: W-LOG. Rows at 1 or 50 a second, written every 200 ms, synced
  every second, and an erase step every 20 ms when asked. It records every
  byte alfs took and how many a successful sync covered.
- W-HR: 480 bytes every 20 ms, synced every second.
- `ring`: W-RING. W-LOG at 50 rows a second, a new log every `every` rows,
  the oldest erased while fewer than two logs' worth of sectors are
  ready, an erase step every 20 ms, until every sector has been erased a
  number of times. Run on a 32-sector area so it cycles quickly.
- `kv_step` and `cfg_step`: W-CFG. A script of sets and deletes of ten
  keys, each value's bytes a function of a generation number; and of puts
  and deletes of a 4,096-byte and a 300-byte program.

---

## 4. The stores' tests

| Test | Requirement | What it does |
|---|---|---|
| `sim_torn_obeys_a9` | 4.4 | Each power-loss behaviour, 500 torn programs of random data into erased bytes and over programmed ones: no bit the data left 1 ever reads 0, on any of 16 reads. |
| `if_geometry` | IF-02 | Refuses seven bad geometries for each area, and an object area of an odd sector count, with no operation. A log area mounted with another sector count finds every sector dirty; an object area mounted with another half size finds no store. |
| `if_endian` | IF-03, SIMP-05 | Checks a log sector's stamp, claim and chunk, and a half's stamp, begun mark, commit and object, against `docs/SPEC.md`: the magics, version 4, the geometry, the words little-endian, the checks. |
| `if_object_names` | IF-05 | Names of 1 and 31 bytes, and one with `/`, put and got, before and after a remount; 0 and 32 bytes, and none, refused with no operation; a prefix of a name does not get it. |
| `if_put_replaces` | IF-06 | A put of an object that exists: a get during the put reads the old bytes, after the commit the new. A delete, a delete of an object that does not exist, writing past the size, and committing short. The same after a remount. |
| `if_put_one_at_a_time` | IF-06 | A second put, and a delete, during a put are refused with `ALFS_EOPEN`; after an abandon the old object reads and a put succeeds. |
| `if_get_snapshot` | IF-07 | A reader opened before a put commits reads the old bytes to the end; a put, delete or erase that would erase its half meanwhile is refused with `ALFS_EOPEN`, and succeeds after it closes. |
| `if_object_list` | IF-08 | Three objects, one deleted and one put again: listed with names and sizes, the same after a remount. |
| `if_log_numbers_never_reused` | IF-09 | Numbers grow; erasing the newest log and remounting does not give its number again; a log's first claim, and its first chunk, torn by FLICKER, METASTABLE or SOMEBITS, each followed by eight remounts each starting a log: no number is given twice. |
| `if_log_streams` | IF-10 | Two writers of one log, tags 1 and 2, interleaved: each stream reads back its own bytes, a third tag reads none; the same after a remount. |
| `if_log_new_after_mount` | IF-10, SAFE-04 | After a remount, writing to the log written before is refused; a new log starts in a sector of its own, and the old one reads whole. |
| `if_log_list` | IF-11 | Three logs of different lengths: listed in growing number with the sectors each holds, against the simulator's flash. |
| `if_log_refusals` | IF-13 | Erasing the log being written, starting a log while a writer is open, erasing while a reader is open, and mounting with a handle open: each refused with `ALFS_EOPEN`, then allowed once closed. |
| `basic_round_trip` | IF-12 | Seven sizes, 1 byte to 20,000, with blocks of one page and of 4,096 bytes: written, read, remounted, read. |
| `spd_no_erase_outside_erase` | SPD-01 | W-LOG at both rates, two minutes, with erase steps; W-CFG with the spare made ready between puts. |
| `spd_write_burst_bounded` | SPD-02, IF-12 | 30 seconds of 480-byte writes (W-HR) and of 22-byte writes, with blocks of one page and of 4,096 bytes: every write, sync and close within its program bound for the bytes it programmed. |
| `spd_enospc_without_erase` | SPD-04 | Fills the ready sectors: `ALFS_ENOSPC` with no erase, from write and sync; after one sector of an older log is erased, the held bytes are written and read back. |
| `spd_mount_reads_bounded` | SPD-05 | A mount of a 128-sector log area holding logs reads at most 30 bytes a sector; of an object area holding objects, at most 29 bytes a half; of a configuration store holding pairs, at most 19 bytes a sector; none does an operation. |
| `spd_wlog_longest_stall` | SPD-06 | Ten minutes of W-LOG at 50 rows a second: no operation longer than one program. |
| `spd_cpu_linear` | SPD-07 | The same writes and syncs on 32 and 2,048 sectors read the same bytes. |
| `spd_read_next_bounded` | SPD-08 | A log written across dirty sectors left in its way: reading it reads at most one header for each of its sectors and each sector between them, plus two. The read that finds the log's end scans every header, and is not counted. |
| `life_erase_only_dirty` | LIFE-01 | W-RING for three cycles: no erase of a sector that held only its stamp. W-CFG: a put erases nothing when the spare is ready, and each sector of a half once when not. |
| `life_write_amplification` | LIFE-02 | Ten minutes of W-LOG at both rates, and W-HR, against the bound computed from the counts the driver wrapper keeps. |
| `life_wlog_erases` | LIFE-03 | Ten minutes of W-LOG at 50 rows a second, then the log erased: erases at most the sectors the log filled, plus one. Prints the count. |
| `life_wear_level` | LIFE-04 | W-RING until every sector is erased ten times: counts within 2. |
| `safe_powerloss_durable` | SAFE-01 | Each behaviour, power lost at every operation of a 20-second W-LOG with erase steps. Then the rest of the case runs as below. |
| `safe_powerloss_bound` | SAFE-02 | The same, with a second power loss after the restart in every third case. |
| `safe_no_program_after_torn` | SAFE-04 | The same runs, for the simulator's check that nothing programs a page a power loss touched. |
| `safe_no_false_data` | SAFE-03 | 200 trials of one bit flipped in a chunk of a two-stream log: what reads back is the stream with at most one page's chunks cut out, and one failure counted when one was. |
| `safe_write_only_verified_ready` | SAFE-05 | An ERASENOOP log sector is found out by its erase and never programmed; an ERASENOOP sector in the spare half makes a put fail and the store reads as it did. Power lost in erases is covered by the power-loss runs. |
| `safe_prog_failure` | SAFE-06 | The sector being written goes PROGERROR, PROGNOOP or PROGFLIP: the bytes read back and the bytes counted lost add up to the bytes written, and what reads back is what was written with runs cut out. |
| `safe_bad_sector` | SAFE-07 | ERASEERROR and ERASENOOP log sectors are counted bad, found bad again after a remount, never programmed. Wear-out under W-RING: logging goes on until the flash is worn. An ERASEERROR sector in the spare half: puts fail with `ALFS_EPROG`, every object reads. |
| `safe_read_retry` | SAFE-08 | Reads that fail once, in each mount, open and read of both areas: each retried call gets the right bytes. |
| `safe_foreign_area` | SAFE-09 | Random and blank areas: the log area mounts with every sector dirty, the object area with `ALFS_ENOFS`. An area past the chip: `ALFS_EGEOMETRY`. No operation in any. |
| `safe_fuzz` | SAFE-10, SPD-09 | 300 images of each area with bits flipped, headers randomised, headers copied, or everything random but one stamp: every call of both stores, under the sanitizers. |
| `safe_log_erase` | SAFE-11 | Power lost at every operation of erasing a log, each behaviour, and erasing finished after a remount: the log never reads again through eight remounts, before then reads only its own chunks, and the other log reads whole. A log with a claim torn by FLICKER is erased whole. |
| `safe_put_atomic` | SAFE-12, SAFE-13 | W-CFG with power lost at every operation, each behaviour. After each loss, eight remounts reading every object, then a put and eight more. Each object reads as its last committed put or delete made it, or as the one cut short would. |
| `safe_obj_check` | SAFE-14 | One bit flipped in an object's name, size, payload or check: no damaged byte returned; the objects before it read, and those after it unless its size was hit. |
| `if_kv_keys` | IF-14 | Keys of 1 and 31 bytes set and got, before and after a remount; 0 and 32 bytes refused; the longest value a page holds accepted, one byte more refused; an empty value deletes. |
| `if_kv_list` | IF-15 | Ten keys, some set again and some deleted: one listing, applied in order to a cache, gives each current pair with its value, the same after a remount; a set between two calls makes the next return `ALFS_EINVAL`. |
| `if_kv_full` | IF-16 | Pairs set until a set returns `ALFS_ENOSPC`: every key reads as before it. |
| `spd_kv_set_bounded` | SPD-10, SPD-01 | W-CFG's sets, with remounts: two programs and no erase while the head has room; one erase when it has none, and at the first set in a mount. |
| `life_kv_round_robin` | LIFE-05 | Sets until the head has gone round the store ten times: erase counts within 1. |
| `safe_kv_atomic` | SAFE-15, SAFE-16 | Power lost at every operation of W-CFG's sets and deletes, each behaviour. After each loss, eight remounts reading every key, then a set and eight more. Each key reads as its last returned set made it, or as the one cut short would. |
| `safe_kv_check` | SAFE-17 | Three pairs, one a page; one bit flipped in a pair's key, lengths, value or check: the pair is not returned; the others are. |
| `obs_status` | OBS-01 | Counts against the simulator's, for both areas, before and after logging, puts and a remount. |
| `obs_clean` | OBS-03 | The log area not clean when it holds a dirty sector, clean once erased; the object area not clean after a put, clean once its spare is ready. |
| `simp_ram` | SIMP-03 | Structure sizes on the host, allowing for wider pointers; `make target` checks them exactly. |
| `make target` | SIMP-01, SIMP-02, SIMP-04 | Builds each store alone with `-std=c99 -Wall -Wextra -Werror -pedantic` for the Cortex-M0+; code at most the budget; no chain of stack frames under a public call over 384 bytes, from GCC's call graph. |

**The power-loss runs, after the loss.**
1. Remount; every synced byte of the log reads back, nothing beyond what
   was written.
2. A new log is started and written, perhaps with a second loss.
3. Remount, erase and read both logs, twice. The old log must read a
   prefix of what was written before the loss, at least what was synced.
   The new one must read a prefix of what was written after the restart,
   at least what was synced since.

**By review:** SPD-09 (each loop's bound), SIMP-01 (the libraries used),
SIMP-05 (the spec covers every field), OBS-02 (every error documented in
the headers), RT-04 and RT-05.

**Deferred until a target build exists:** IF-03's big-endian build.

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
| `rt_round_trip` | RT-02, RT-05 | Mount, start a log, write, sync, close, read, list, erase, through requests; the flash task runs at the priority given. |
| `rt_serialises` | RT-01 | Two writer tasks on two streams of a log, a reader, a task putting objects and setting pairs, with erase steps between: every stream, object and pair reads back whole; no operation outside the flash task, none overlapping. |
| `rt_fast_path` | RT-03 | Writes smaller than the block's room, and reads from what the block holds, send no request; the write that fills the block sends one. |
| `rt_before_scheduler` | RT-06 | Mount, erase, a log started and written, and a put, before the scheduler starts, run directly, and read back after it starts. |
| `rt_reentry_refused` | RT-06 | A driver that calls alfs from inside an operation is refused with no effect; a log handle used from another task is refused. |
| `rt_erase_interleaves` | RT-07 | An erase of the whole log area requested by one task; a write and sync requested by another meanwhile complete before the erase does. |
| `rt_read_while_writing` | RT-02 | One task writes and syncs a log while another reads it: the reader gets a prefix of what was synced, in order. |
| `rt_objects` | RT-02 | Put, get, list and delete through requests, with the spare made ready by erase steps; configuration pairs set, got, listed and deleted. |

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
