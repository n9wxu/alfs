# What the flash probe found

The probe (`probe/`, `docs/TEST_PLAN.md` section 6) tests assumption A-8
of `docs/REQUIREMENTS.md` on the chips themselves: a program into bytes
already programmed clears exactly the bits that are 0 in its data and
changes nothing else. No datasheet promises it (W25Q16JV p. 34 and
W25Q128JV p. 37 say Page Program writes "previously erased (FFh) memory
locations"; the XT25F128F and BY25Q64ES sheets say nothing). It also
measures how long programs and erases take.

Every line each run printed is in `docs/probe_results/`, one file a run,
named by JEDEC ID, board kind, the last six digits of the chip's unique
ID, and the date.

---

## 1. Verdict

**A-8 holds on every chip tested.** Five chips of three parts took
783,040 programs each (674,231 of them into bytes already programmed),
with 84 erases each, and not one byte read back other than the model
said. Every page of the bit-stepping test (P1) took 2,048 programs between
erases, and the over-programmed byte (P4) 1,000.

| Part | Board kind | Chip (last 6 of unique ID) | Result |
|---|---|---|---|
| W25Q128JV (EF4018) | MK1A | 2D472F | pass |
| XT25F128F (0B4018) | MK1C | 3D0025 | pass |
| XT25F128F (0B4018) | MK1C_SD | 640027 | pass |
| BY25Q64ES (684017) | MK1B_8MB | 851D20 | pass |
| BY25Q64ES (684017) | MK1B_8MB | 361D20 | pass |
| W25Q16JV | MK1B | | not tested: no MK1B was attached |

**The probe can fail.** The self-test run on the MK1A (`..._selftest.jsonl`)
planted one wrong bit in the model and reported exactly that one
mismatch, at 0x7FC500.

---

## 2. The runs

Each run used four sectors at the end of the board's firmware-update
download slot (0x7FC000 on MK1A, MK1C and MK1C_SD; 0x3FC000 on MK1B_8MB),
which were blank before and are blank after. 20 cycles, seed 20261014.
Each board rebooted into its own firmware afterwards and answered its
status page.

| Part | Programs | Of them reprograms | Erases | Mismatches | Erases not blank | Seconds |
|---|---|---|---|---|---|---|
| each of the five | 783,040 | 674,231 | 84 | 0 | 0 | 132-158 |

The status registers read as each part's defaults; no block was write
protected. The probe reads with 03h at clk_sys / 6 (20.8 MHz).

---

## 3. Program and erase times

Microseconds, from the end of the command to the status register showing
the chip done, polled at about 2 us. The mean, then the range, over 80
samples a size a chip, or more where the tests made more.

| Program | W25Q128JV | XT25F128F (two chips) | BY25Q64ES (two chips) |
|---|---|---|---|
| 1 byte into erased | 21 (21-41) | 55-56 (54-73) | 59-60 (58-104) |
| 2 bytes | 24 | 56 | 60 |
| 4 bytes | 26 | 56 | 61 |
| 16 bytes | 45 | 64-66 | 77-78 |
| 64 bytes | 118 | 92 | 138-149 |
| 128 bytes | 215 | 128-129 | 217-245 |
| 256 bytes, a whole page | 402 (385-435) | 197-200 (195-222) | 353-397 (328-458) |
| a page padded with FFh, ~16 bytes programmed | 94 | 94-95 | 248-252 |
| 1 byte reprogrammed | 21 | 52 | 55 |
| 4 bytes reprogrammed | 25 | 56 | 60-61 |
| a byte reprogrammed with nothing left to clear (P4) | 16 | 22 | 17 |
| **4 KB sector erase, ms** | 46.8 (44.1-50.8) | 15.0-15.5 (6.8-18.0) | 25.9-26.2 (0.56-32.2) |

What the times say:

- **A short program is fast.** 1 to 4 bytes take 21-26 us on the
  W25Q128JV and 55-61 us on the others, against 200-400 us for a page.
  A small record costs a seventh to a twentieth of the 0.4 ms the
  requirements budget for a program (section 4.3).
- **The XT25F128F and BY25Q64ES are slower than their datasheets allow
  for one byte:** 50 us is the first-byte maximum on both (XT25F128F p. 55,
  BY25Q64ES p. 71), and they take 55-61 us, which the probe's few
  microseconds of polling do not explain.
- **The XT25F128F is faster than its datasheet elsewhere:** a page in
  0.2 ms against 0.4 typical (p. 55), and a sector erase in 15 ms against
  40 typical (p. 56).
- **Reprogramming costs no more than programming.** A reprogram takes no
  longer than a program of the same length into erased bytes, and one that
  clears fewer bits takes less: a whole page, 326 against 402 us on the
  W25Q128JV.
- **Padding costs, most on the BY25Q64ES.** A page padded with FFh, about
  16 of its bytes really programmed, takes 94 us on the W25Q128JV and
  XT25F128F, a quarter and a half of a full page; on the BY25Q64ES 250 us,
  two thirds of a full page. A driver should send only the bytes it
  programs.
- **Some erases end early.** The quickest BY25Q64ES erases took 0.56 ms,
  and XT25F128F 6.8 ms. The probe does not record which erases those were.
  The only sectors erased while already blank are the four at the start of
  each run, so these parts appear to skip most of the work on a blank
  sector.

---

## 4. Limits

- **One chip of each part**, or two where two boards carry it. A-8 says
  nothing about other lots, and nothing about a part not listed here.
- **Read back at once.** Whether bits cleared by a reprogram keep as well
  as bits programmed once (retention) is not tested.
- **No power cut during a reprogram.** What a byte holds after one is not
  known; a format that relies on case 2 must treat such a byte as it
  treats any torn program.
- **The probe reads with 03h at clk_sys / 6** (20.8 MHz), which is not how
  pyro reads in flight (execute-in-place).
