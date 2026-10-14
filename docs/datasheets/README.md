# Datasheets

The primary sources for every flash figure in alfs's documents. Cite the
file and PDF page, not memory. Copied from the pyro firmware's
`docs/datasheets/` on 2026-10-14, where their provenance is recorded.

| File | Part | Revision |
|---|---|---|
| `W25Q16JV_RevI_2024-12-24.pdf` | Winbond W25Q16JV 16 Mbit 3 V serial NOR flash (pyro MK1B) | Revision I, 24 December 2024 |
| `W25Q128JV_RevH_2021-03-10.pdf` | Winbond W25Q128JV 128 Mbit 3 V serial NOR flash (pyro MK1A) | Revision H, 10 March 2021 |
| `XT25F128F-W_Rev0.6_2022-02-26.pdf` | XTX XT25F128F-W 128 Mbit 3 V serial NOR flash (pyro MK1C); preliminary | Rev 0.6, 2022-02-26 |
| `BY25Q64ES_Rev2.9_2024-10-29.pdf` | BYTe BY25Q64ES 64 Mbit 3 V serial NOR flash (pyro MK1B-8MB) | Rev. 2.9, 2024-10-29 |
| `rp2040-datasheet_2025-02-20.pdf` | Raspberry Pi RP2040, the microcontroller of pyro's boards | build 3184e62, 2025-02-20 |

## Figures used

| Part | Page program typ / max | 4 KB erase typ / max | 64 KB erase typ / max | Endurance | Suspend |
|---|---|---|---|---|---|
| W25Q16JV | 0.4 / 3 ms (p. 64) | 45 / 400 ms (p. 64) | 150 / 2,000 ms (p. 64) | 100K cycles a sector, minimum (p. 5) | erase and program, 75h/7Ah; tSUS 20 us (pp. 40, 64) |
| W25Q128JV | 0.4 / 3 ms (p. 66) | 45 / 400 ms (p. 66) | 150 / 2,000 ms (p. 66) | 100K, minimum (p. 5) | as the W25Q16JV (pp. 43, 66) |
| XT25F128F | 0.4 / 2 ms (p. 55) | 40 / 800 ms (p. 56) | 250 / 1,600 ms (p. 56) | 100,000, minimum (p. 2) | erase and program; tSUS 20 us, resume to suspend 500 us at least (pp. 43, 55) |
| BY25Q64ES | 0.45 / 2.4 ms (p. 71) | 35 / 300 ms (p. 71) | 180 / 2,000 ms (p. 71) | 100k, typical (p. 2) | erase only; tESL 30 us (pp. 62, 71) |

Partial page programs: each part programs fewer than 256 bytes "without
having any effect on other bytes within the same page" (W25Q16JV p. 34,
W25Q128JV p. 37, XT25F128F p. 36, BY25Q64ES pp. 56-57). None states a limit
on how many times one page may be programmed between erases.

Power cut while an erase or program is suspended: "The data within the
page, sector or block that was being suspended may become corrupted"
(W25Q16JV p. 40).

RP2040, p. 136 (the bootrom's flash functions): between `_flash_exit_xip`
and the return to XIP, "the SSI is not in a state where it can handle XIP
accesses, so the code that calls the intervening functions must be located
in SRAM". A program or erase therefore stops all code running from that
flash for its whole duration. Page 135: `_flash_range_erase` takes an
optional block erase command (D8h, 64 KB) "for much higher erase speed".
Page 130: the bootrom jumps to code pre-loaded in SRAM after a watchdog
reset, so SRAM keeps its contents through one.
