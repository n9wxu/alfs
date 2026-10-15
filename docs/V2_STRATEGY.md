# alfs v2: what the probe changes

**Status: accepted, 2026-10-14.** The user: "1. I accept the changes, 2.
≤1ms writes is OK so the chunk size can be larger. 3. run the torn write
test." So changes 1 to 4 go ahead, no catalog is built, chunks stay up to
a page (at most 0.46 ms measured), and P8 and P9 run before changes 3 and
4 rely on A-9.

**Built, 2026-10-14** (`docs/SPEC.md` version 2). P8 found A-9 holds on the
W25Q128JV and both XT25F128F; the BY25Q64ES could not be torn by command.
Building it, with the simulator's torn bits made to behave as the probe
found, showed four things the proposal missed, each now a rule in SAFE-04,
SAFE-11 or IF-04:

- a torn claim could later read valid, reuse a sequence or put old bytes
  after new: each stamp records a sequence floor, and a resumed stream
  clears ahead;
- a stamp an erase step left half programmed could decay: every claim
  program writes the stamp again;
- a claim torn at its start leaves a sector that reads ready: after a
  mount, the sector where the allocation cursor stood is not used;
- an erase step that chose a sector by one read and judged its stream
  deleted by another deleted a live stream: it now decides from one read.

The code grew from 5,301 to 6,845 bytes; SIMP-02 was raised to 7 KiB.
It follows `docs/FLASH_PROBE.md` and revises the recommendation of
`docs/CATALOG_EVALUATION.md`.

---

## 1. The question

The user, 2026-10-14:

> "How should ALFS be updated with this new information? What strategy
> minimizes writes, maximizes speed and maximizes safety"

**The answer in short.** alfs v1 already writes about 4% more than the
data it stores, and no catalog can lower that: a catalog only adds
programs and erases. So the strategy is to add no structure, and to spend
the new facts on four small changes:

1. **Program only the bytes being written** (speed). The driver sends a
   chunk's bytes and nothing more. 16 bytes sent alone take 45-78 us; the
   same 16 bytes in a page padded with FFh take 94-250 us. No format
   change.
2. **Number each sector within its stream** (speed, safety). Two bytes
   in the claim let the reader find a stream's next sector by a short scan
   forward, instead of reading every header in the region, and let it see
   a missing sector.
3. **Firm and resume** (writes, safety). After a reset, alfs programs the
   last sector's claim again with the same bytes, which completes any bit
   a power cut left half programmed. Then it continues two pages on in
   the same sector, instead of starting a new one: at most 512 bytes lost
   per reset, instead of up to 4 KB.
4. **Firm the dead mark** (safety). Before reclaiming a deleted stream,
   the erase step programs its dead mark to 00h again, so a power cut
   during the reclaim cannot bring the stream back half erased.

Changes 3 and 4 rest on a new assumption, A-9: programming the intended
bytes over a torn program completes it. The probe can test that on the
chips (section 6) before anything relies on it.

---

## 2. What the probe changed

From `docs/FLASH_PROBE.md`, five chips of three parts:

- **Case 2 works** (A-8): programming bytes already programmed clears
  exactly the 0 bits and nothing else, 2,048 programs a page.
- **A program's time is a fixed cost plus a cost a byte.** About 20 us
  plus 1.5 us a byte on the W25Q128JV, 54 us plus 0.6 us on the
  XT25F128F, 58 us plus 1.2 us on the BY25Q64ES. A small program is fast;
  many small programs pay the fixed cost many times.
- **Padding costs.** A page padded with FFh takes 94 us on the W25Q128JV
  and XT25F128F and 250 us on the BY25Q64ES, where the same 16 bytes sent
  alone take 45-78 us.
- **Erases take 15 ms (XT25F128F), 26 ms (BY25Q64ES), 47 ms (W25Q128JV)**
  a sector. They happen in erase steps, before flight, so they set how
  long making ready takes, not the loop.

---

## 3. Where v1 spends writes now

On littlefs's logging workload (`life_wlog_erases`, 10 minutes at 50 rows
a second) v1 fills 168 sectors to hold 161 sectors' worth of rows, about
4% more:

| Where | Cost |
|---|---|
| The sector header: stamp, dead mark, claim | 41 B a sector, 1.0% |
| The chunk header: length and CRC-32 | 6 B a chunk; a chunk is at most a page, so at least 2.3% |
| Page tails too short for a chunk | under 7 B a page |
| A reset (`alfs_open_append` starts a new sector, `alfs.c` line 781) | up to one sector per reset |
| A failed program | up to one sector per failure |

A catalog (CATALOG_EVALUATION D1 or D2) lowers none of these, and adds a
program per sector and an erase per file. Under "minimize writes" it
loses. The claim's sector number (D0+ there, change 2 here) gives most of
a catalog's reading speed with no added program.

---

## 4. The changes

### 4.1 Program only the bytes being written

alfs already hands the driver each chunk's exact bytes. The driver must
send exactly those. Pyro's flash path today programs whole 256-byte pages
(`src/littlefs_driver.c` line 122, `prog_size = FLASH_PAGE_SIZE`), as the
SDK's `flash_range_program` requires. An alfs driver for pyro issues Page
Program (02h) with only the chunk's bytes; `probe/probe.c`,
`flash_write`, is the pattern.

- Gain: 16 bytes stall the system 45-78 us sent alone, 94-250 us in a
  padded page (measured); most on the BY25Q64ES.
- Cost: none in alfs; a short driver in pyro.
- New requirement: IF-05, the driver programs exactly the bytes it is
  given (measured cost of padding in FLASH_PROBE).

### 4.2 Number each sector within its stream

The claim gains a 2-byte sector number: 0 in the stream's first sector,
one more in each next. `next_sector` (`alfs.c` line 535) stops at the
claim with the next number. Allocation moves forward around the region,
so that claim is usually a few sectors ahead: as many as other streams
claimed in between. Today the scan stops early only when no other stream
claimed a sector in between, and otherwise reads every header, about
2 million headers to read back a 1,000-sector stream on 2,048 sectors.

- Gain: reading a stream written alongside others reads a few headers
  per sector, not the region's. A gap in the numbers shows a lost sector,
  which the reader can count instead of passing over silently.
- Cost: 2 B a sector (0.05%), a few lines of code, format version 2.
- New requirement: SPD-10, reading on to a stream's next sector reads at
  most the headers of the sectors claimed between them.

### 4.3 Firm and resume

Today a reset costs up to a sector, because a power cut may have left
bits half programmed in the last sector, and v1 never programs a page a
power cut touched (SAFE-04). Two places matter:

- **The claim.** A torn program of the claim (with its sector's first
  chunk) can leave bits that read correctly now and wrongly later. The
  sector would then look dirty and be erased, with every chunk synced
  into it after the reset. Programming the claim again with the bytes it
  reads now completes those bits: a torn program only ever pulses the bits
  meant to be 0, and a program of the same bytes finishes them (A-9).
- **The chunks.** The torn program was either the next chunk in the page
  of the last good chunk, or the first chunk of the next page. Neither
  page gets new data. The stream continues at the start of the page after
  those two, or in a new sector if there is none.

So `alfs_open_append` stays free of flash operations (SPD-07), and the
first append after it programs the claim again (one program of 18 bytes,
about 47-80 us by the cost model of section 2), then carries on two pages
past the last good chunk.

- Gain: under 512 B per reset (the rest of the last good chunk's page and
  the page after it) instead of up to 4 KB. The claim can no longer decay into erasing synced data.
- Cost: one 18-byte program per reset; SAFE-04 and IF-02 amended to allow
  programming identical bytes over a claim; the simulator learns that
  programming 0 into a half-programmed bit settles it.
- Rests on A-9 (section 6).

### 4.4 Firm the dead mark

A torn delete can leave the dead mark half programmed. Today a mount that
reads it as deleted starts erasing the stream's sectors; a power cut, and
the next mount may read the mark as erased, and find the stream alive
with some of its sectors gone. SAFE-11 promises whole or gone.

Before it erases any sector of a deleted stream, the erase step programs
its dead mark to 00h again: one program per deleted stream, on the
ground.

- Gain: SAFE-11 holds through a power cut in the reclaim.
- Cost: one program per deleted stream, in an erase step.
- Rests on A-9.

---

## 5. Considered and not recommended

| Idea | Why not |
|---|---|
| A catalog, D1 or D2 | adds a program per sector and an erase per file; buys seeking, which no use case needs (UC-1 to UC-10 read logs whole) |
| A directory sector | erases live metadata; a single point of failure |
| A shorter chunk check (CRC-16) | saves 2 B a chunk (under 1%) and weakens the guard against false data |
| Case 2 for a length or commit field in place | lets synced bytes exist before their check does |
| Raising LIFE-06's 16 programs a page | pages fill before 16 programs unless syncs are under 10 bytes; and the W25Q16JV is untested |

**For the user to decide: a cap on a program's length.** Chunks of at most
64 bytes instead of a page would cut the longest stall from 0.2-0.46 ms
measured (2.4-3 ms datasheet maximum) to 0.09-0.15 ms measured (0.4-0.8 ms
worst by the XT25F128F's and BY25Q64ES's byte times; the Winbond sheets
give only the page maximum). It costs four times the programs and
raises the chunk header's share from 2.3% to 9.4%. Speed comes first,
but a 0.46 ms stall may already be well inside pyro's loop budget, so it
would be a configuration value, off by default.

---

## 6. A-9 and how to test it

**A-9:** a program of the intended bytes over a program that a power cut
interrupted leaves those bytes reading as intended, on every later read,
and changes no other byte.

All three parts abort an operation under way when given Enable Reset (66h)
then Reset (99h), and warn that "data corruption may happen" (W25Q128JV
p. 60, XT25F128F p. 41, BY25Q64ES p. 30). That makes torn programs on
demand, without cutting power. It is a stand-in: an aborted program is
not a supply collapsing, but it leaves cells in the same half-programmed
states.

- **P8, torn programs:** program known data, reset after a delay swept
  across the program's time, read the page 16 times to find bits that
  read both ways, program the intended data again, read 16 more times.
  Pass: every byte reads as intended on every read, and no other byte
  changed.
- **P9, torn erases:** erase, reset after a swept delay, read the sector
  16 times. Counts sectors that read blank once and not later: the hazard
  of A-4, which tells how far an erase step's single blank check can be
  trusted (SAFE-05).

Both run in the probe's test sectors, on every chip, as P1 to P7 did.

---

## 7. The order of work

1. **Requirements:** A-9 (assumed until P8 passes), IF-05, SPD-10, a
   LIFE requirement bounding a reset's waste, SAFE-04, SAFE-11 and IF-02
   amended. The measured figures are already in section 4.3.
2. **Tests:** P8 and P9 in the probe; in the simulator, reprograms as
   AND and half-programmed bits settled by programming 0; host tests for
   the sector number, firm-and-resume under every power-loss behaviour,
   and the dead mark under a power cut in the reclaim.
3. **Code:** format version 2. There is no v1 data to migrate: alfs is
   not yet in pyro.

**Decisions for the user:**

1. Adopt changes 1 to 4, and drop the catalog (D2), which
   CATALOG_EVALUATION recommended before "minimize writes" was the test.
2. A cap on a program's length: off, 64 bytes, or another value.
3. Run P8 and P9 on the bench boards. They abort writes only in the
   probe's test sectors.
