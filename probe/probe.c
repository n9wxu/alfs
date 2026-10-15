/*
 * The flash probe: docs/TEST_PLAN.md section 6, on the chip itself.
 *
 * Runs from RAM, since it erases and programs the flash the board boots
 * from, and drives the RP2040's SSI directly: hardware_flash's flash_do_cmd
 * does not exist in a no_flash build, and its XIP exit and entry around
 * every command would swamp the timings.
 *
 * Commands, one a line, on USB CDC:
 *   id                                  the chip's identity
 *   run <addr> <sectors> <cycles> <seed> [selftest]
 *   reboot                              into the board's own firmware
 * Answers are JSON, one object a line.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/ssi.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"

#define SECTOR 4096u
#define PAGE 256u
#define PROG_TIMEOUT_US 10000u
#define ERASE_TIMEOUT_US 2000000u
#define MISMATCH_REPORTS 16u

/* ---- the chip ----------------------------------------------------------- */

static void cs(bool high) {
    uint32_t v = high ? IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_VALUE_HIGH : IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_VALUE_LOW;
    hw_write_masked(&ioqspi_hw->io[1].ctrl, v << IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_BITS);
}

/* The SSI's FIFOs are 16 deep: keep fewer in flight, as an interrupt may
 * stall the reader. tx or rx may be NULL. */
static void xfer(const uint8_t *tx, uint8_t *rx, size_t n) {
    size_t tl = n, rl = n;
    while (tl || rl) {
        uint32_t sr = ssi_hw->sr;
        if ((sr & SSI_SR_TFNF_BITS) && tl && rl - tl < 14) {
            ssi_hw->dr0 = tx ? *tx++ : 0;
            tl--;
        }
        if ((sr & SSI_SR_RFNE_BITS) && rl) {
            uint8_t b = (uint8_t)ssi_hw->dr0;
            if (rx)
                *rx++ = b;
            rl--;
        }
    }
}

static void cmd(const uint8_t *hdr, size_t hn, const uint8_t *tx, uint8_t *rx, size_t n) {
    cs(false);
    xfer(hdr, NULL, hn);
    xfer(tx, rx, n);
    cs(true);
}

static void addr_cmd(uint8_t op, uint32_t a, const uint8_t *tx, uint8_t *rx, size_t n) {
    uint8_t h[4] = {op, (uint8_t)(a >> 16), (uint8_t)(a >> 8), (uint8_t)a};
    cmd(h, 4, tx, rx, n);
}

static uint8_t status(uint8_t op) {
    uint8_t s;
    cmd(&op, 1, NULL, &s, 1);
    return s;
}

static void flash_read(uint32_t a, uint8_t *buf, size_t n) {
    addr_cmd(0x03, a, NULL, buf, n);
}

/* Microseconds from the end of the command to the status register's BUSY
 * clearing, or -1 when the write enable did not latch, or -2 on timeout. */
static int32_t flash_write(uint8_t op, uint32_t a, const uint8_t *data, size_t n, uint32_t timeout) {
    uint8_t wren = 0x06;
    cmd(&wren, 1, NULL, NULL, 0);
    if (!(status(0x05) & 2))
        return -1;
    addr_cmd(op, a, data, NULL, n);
    uint32_t t0 = time_us_32();
    while (status(0x05) & 1)
        if (time_us_32() - t0 > timeout)
            return -2;
    return (int32_t)(time_us_32() - t0);
}

static void chip_init(void) {
    rom_connect_internal_flash_fn connect = (rom_connect_internal_flash_fn)rom_func_lookup(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    rom_flash_exit_xip_fn exit_xip = (rom_flash_exit_xip_fn)rom_func_lookup(ROM_FUNC_FLASH_EXIT_XIP);
    connect();
    exit_xip();
    ssi_hw->ssienr = 0;
    ssi_hw->baudr = 6; /* clk_sys / 6: within every part's limit for 03h */
    ssi_hw->ssienr = 1;
    cs(true);
}

static uint8_t jedec[3];

static uint32_t chip_size(void) {
    return jedec[2] >= 0x10 && jedec[2] <= 0x19 ? 1u << jedec[2] : 0;
}

static void identify(void) {
    uint8_t op = 0x9F, uid[16], h[5] = {0x4B, 0, 0, 0, 0};
    cmd(&op, 1, NULL, jedec, 3);
    cmd(h, 5, NULL, uid, sizeof uid);
    printf("{\"jedec\":\"%02X%02X%02X\",\"bytes\":%lu,\"uid\":\"", jedec[0], jedec[1], jedec[2],
           (unsigned long)chip_size());
    for (unsigned i = 0; i < sizeof uid; i++)
        printf("%02X", uid[i]);
    printf("\",\"sr\":[%u,%u,%u],\"clk_sys\":%lu,\"ssi_div\":%lu}\n", status(0x05), status(0x35), status(0x15),
           (unsigned long)clock_get_hz(clk_sys), (unsigned long)ssi_hw->baudr);
}

/* ---- the record --------------------------------------------------------- */

typedef struct {
    uint16_t len;
    bool reprogram, padded;
    uint32_t count, min, max;
    uint64_t sum;
} timing_t;

static timing_t timings[24];
static unsigned ntimings;
static timing_t erases;

static struct {
    uint32_t programs, erases, mismatches, blank_failures, timeouts, wel_failures;
} tally;

static void see(timing_t *t, uint32_t us) {
    if (!t->count || us < t->min)
        t->min = us;
    if (us > t->max)
        t->max = us;
    t->count++;
    t->sum += us;
}

static timing_t *timing_for(uint16_t len, bool reprogram, bool padded) {
    for (unsigned i = 0; i < ntimings; i++)
        if (timings[i].len == len && timings[i].reprogram == reprogram && timings[i].padded == padded)
            return &timings[i];
    if (ntimings == sizeof timings / sizeof timings[0])
        return NULL;
    timing_t *t = &timings[ntimings++];
    memset(t, 0, sizeof *t);
    t->len = len;
    t->reprogram = reprogram;
    t->padded = padded;
    return t;
}

/* ---- the model ---------------------------------------------------------- */

static uint8_t model[SECTOR];
static uint8_t got[SECTOR];
static uint32_t base;       /* the sector under test */
static unsigned cycle;
static const char *test;
static bool failed_hard;    /* a timeout or a write enable that did not latch */

static uint32_t rng;

static uint32_t rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static uint8_t rnd_programmed(void) { /* a byte with at least one 0 bit */
    uint8_t b = (uint8_t)rnd();
    return b == 0xFF ? 0xFE : b;
}

/* Compares [off, off + n) of the sector with the model; a byte that differs
 * is reported, then adopted, so each fault counts once. */
static void verify(uint32_t off, uint32_t n) {
    flash_read(base + off, got + off, n);
    for (uint32_t i = off; i < off + n; i++) {
        if (got[i] == model[i])
            continue;
        if (tally.mismatches < MISMATCH_REPORTS)
            printf("{\"mismatch\":{\"addr\":%lu,\"expected\":%u,\"got\":%u,\"test\":\"%s\",\"cycle\":%u}}\n",
                   (unsigned long)(base + i), model[i], got[i], test, cycle);
        tally.mismatches++;
        model[i] = got[i];
    }
}

/* Programs n bytes at off, within one page, then checks the page. */
static void prog(uint32_t off, const uint8_t *data, uint32_t n, bool padded) {
    if (failed_hard)
        return;
    bool reprogram = false;
    for (uint32_t i = 0; i < n; i++) {
        reprogram |= model[off + i] != 0xFF && data[i] != 0xFF;
        model[off + i] &= data[i];
    }
    int32_t us = flash_write(0x02, base + off, data, n, PROG_TIMEOUT_US);
    tally.programs++;
    if (us == -1) {
        tally.wel_failures++;
        failed_hard = true;
        printf("{\"error\":\"write enable did not latch\",\"addr\":%lu}\n", (unsigned long)(base + off));
        return;
    }
    if (us == -2) {
        tally.timeouts++;
        failed_hard = true;
        printf("{\"error\":\"program timeout\",\"addr\":%lu}\n", (unsigned long)(base + off));
        return;
    }
    timing_t *t = timing_for((uint16_t)n, reprogram, padded);
    if (t)
        see(t, (uint32_t)us);
    verify(off - off % PAGE, PAGE);
}

static void prog1(uint32_t off, uint8_t v) {
    prog(off, &v, 1, false);
}

static bool erase_sector(void) {
    int32_t us = flash_write(0x20, base, NULL, 0, ERASE_TIMEOUT_US);
    tally.erases++;
    if (us < 0) {
        us == -1 ? tally.wel_failures++ : tally.timeouts++;
        failed_hard = true;
        printf("{\"error\":\"erase %s\",\"addr\":%lu}\n", us == -1 ? "write enable" : "timeout", (unsigned long)base);
        return false;
    }
    see(&erases, (uint32_t)us);
    memset(model, 0xFF, sizeof model);
    flash_read(base, got, SECTOR);
    for (uint32_t i = 0; i < SECTOR; i++)
        if (got[i] != 0xFF) {
            tally.blank_failures++;
            printf("{\"blank_failure\":{\"addr\":%lu,\"got\":%u,\"cycle\":%u}}\n", (unsigned long)(base + i), got[i],
                   cycle);
            memcpy(model, got, sizeof model);
            break;
        }
    return true;
}

/* ---- the tests (TEST_PLAN section 6) ------------------------------------ */

#define PG(n) ((n) * PAGE)

static void p5_many_programs(void) {
    test = "P5";
    for (uint32_t i = 0; i < PAGE; i++)
        prog1(PG(0) + i, rnd_programmed());
}

static void p1_bit_stepping(void) {
    test = "P1";
    for (uint32_t i = PG(1); i < PG(3); i++)
        for (unsigned k = 1; k <= 8; k++)
            prog1(i, (uint8_t)(0xFFu << k));
    for (uint32_t i = PG(3); i < PG(5); i++) {
        uint8_t order[8] = {0, 1, 2, 3, 4, 5, 6, 7};
        for (unsigned k = 7; k > 0; k--) {
            unsigned j = rnd() % (k + 1);
            uint8_t x = order[k];
            order[k] = order[j];
            order[j] = x;
        }
        for (unsigned k = 0; k < 8; k++)
            prog1(i, (uint8_t)~(1u << order[k]));
    }
}

static void p2_and(bool selftest) {
    test = "P2";
    uint8_t d[PAGE];
    for (uint32_t p = 5; p <= 8; p++)
        for (unsigned r = 0; r < 3; r++) {
            for (uint32_t i = 0; i < PAGE; i++)
                d[i] = (uint8_t)rnd();
            prog(PG(p), d, PAGE, false);
            if (selftest && p == 5 && r == 0) {
                model[PG(5)] ^= 1;
                verify(PG(5), PAGE);
            }
        }
}

static void p6_padded(void) {
    test = "P6";
    uint8_t d[PAGE];
    for (uint32_t p = 9; p <= 10; p++)
        for (unsigned u = 0; u < 64; u++) {
            memset(d, 0xFF, sizeof d);
            for (unsigned k = 0; k < 16; k++)
                d[rnd() % PAGE] = rnd_programmed();
            prog(PG(p), d, PAGE, true);
        }
}

static void p4_over_programming(void) {
    test = "P4";
    for (unsigned k = 1; k <= 1000; k++) {
        prog1(PG(11), 0x00);
        if (k % 100 == 0)
            verify(PG(10), PG(3));
    }
}

static void p7_timing(void) {
    test = "P7";
    static const uint16_t lens[] = {1, 2, 4, 8, 16, 64, 128};
    uint8_t d[PAGE];
    uint32_t off = PG(12);
    for (unsigned k = 0; k < sizeof lens / sizeof lens[0]; k++) {
        for (unsigned i = 0; i < lens[k]; i++)
            d[i] = rnd_programmed();
        prog(off, d, lens[k], false);
        off += lens[k];
    }
    for (unsigned i = 0; i < PAGE; i++)
        d[i] = rnd_programmed();
    prog(PG(13), d, PAGE, false);
    static const uint8_t f0[4] = {0xF0, 0xF0, 0xF0, 0xF0}, z[4] = {0, 0, 0, 0};
    for (uint32_t g = PG(14); g < PG(15); g += 4) {
        prog(g, f0, 4, false);
        prog1(g, 0x00);
        prog(g, z, 4, false);
    }
}

static bool blank_at_start(uint32_t a) {
    flash_read(a, got, SECTOR);
    for (uint32_t i = 0; i < SECTOR; i++)
        if (got[i] != 0xFF) {
            printf("{\"refused\":\"not blank\",\"addr\":%lu,\"got\":%u}\n", (unsigned long)(a + i), got[i]);
            return false;
        }
    return true;
}

static void print_timing(const timing_t *t) {
    printf("{\"len\":%u,\"reprogram\":%s,\"padded\":%s,\"count\":%lu,\"min_us\":%lu,\"mean_us\":%lu,\"max_us\":%lu}",
           t->len, t->reprogram ? "true" : "false", t->padded ? "true" : "false", (unsigned long)t->count,
           (unsigned long)t->min, (unsigned long)(t->count ? t->sum / t->count : 0), (unsigned long)t->max);
}

static void run(uint32_t addr, uint32_t sectors, uint32_t cycles, uint32_t seed, bool selftest) {
    uint32_t size = chip_size();
    if (!size || addr % SECTOR || !sectors || addr + sectors * SECTOR > size || !cycles) {
        printf("{\"refused\":\"bad arguments\",\"chip_bytes\":%lu}\n", (unsigned long)size);
        return;
    }
    for (uint32_t s = 0; s < sectors; s++)
        if (!blank_at_start(addr + s * SECTOR))
            return;
    memset(&tally, 0, sizeof tally);
    memset(&erases, 0, sizeof erases);
    ntimings = 0;
    failed_hard = false;
    rng = seed ? seed : 1;
    printf("{\"run\":{\"addr\":%lu,\"sectors\":%lu,\"cycles\":%lu,\"seed\":%lu,\"selftest\":%s}}\n",
           (unsigned long)addr, (unsigned long)sectors, (unsigned long)cycles, (unsigned long)seed,
           selftest ? "true" : "false");
    uint32_t t0 = time_us_32();
    for (cycle = 0; cycle < cycles && !failed_hard; cycle++) {
        for (uint32_t s = 0; s < sectors && !failed_hard; s++) {
            base = addr + s * SECTOR;
            if (!erase_sector())
                break;
            p5_many_programs();
            p1_bit_stepping();
            p2_and(selftest && cycle == 0 && s == 0);
            p6_padded();
            p4_over_programming();
            p7_timing();
            test = "sector";
            verify(0, SECTOR);
        }
        printf("{\"cycle\":%u,\"programs\":%lu,\"mismatches\":%lu,\"seconds\":%lu}\n", cycle,
               (unsigned long)tally.programs, (unsigned long)tally.mismatches,
               (unsigned long)((time_us_32() - t0) / 1000000u));
    }
    bool left_blank = !failed_hard;
    for (uint32_t s = 0; s < sectors && !failed_hard; s++) {
        base = addr + s * SECTOR;
        uint32_t before = tally.blank_failures;
        left_blank &= erase_sector() && tally.blank_failures == before;
    }
    printf("{\"timing\":[");
    for (unsigned i = 0; i < ntimings; i++) {
        if (i)
            printf(",");
        print_timing(&timings[i]);
    }
    printf("],\"erase\":{\"count\":%lu,\"min_us\":%lu,\"mean_us\":%lu,\"max_us\":%lu}}\n", (unsigned long)erases.count,
           (unsigned long)erases.min, (unsigned long)(erases.count ? erases.sum / erases.count : 0),
           (unsigned long)erases.max);
    bool pass = !failed_hard && !tally.blank_failures && left_blank &&
                (selftest ? tally.mismatches == 1 : tally.mismatches == 0);
    printf("{\"done\":true,\"pass\":%s,\"mismatches\":%lu,\"blank_failures\":%lu,\"timeouts\":%lu,"
           "\"wel_failures\":%lu,\"programs\":%lu,\"erases\":%lu,\"left_blank\":%s,\"seconds\":%lu}\n",
           pass ? "true" : "false", (unsigned long)tally.mismatches, (unsigned long)tally.blank_failures,
           (unsigned long)tally.timeouts, (unsigned long)tally.wel_failures, (unsigned long)tally.programs,
           (unsigned long)tally.erases, left_blank ? "true" : "false",
           (unsigned long)((time_us_32() - t0) / 1000000u));
}

/* ---- the command line ----------------------------------------------------- */

static bool read_line(char *buf, size_t n) {
    size_t len = 0;
    for (;;) {
        int c = getchar_timeout_us(100000);
        if (c == PICO_ERROR_TIMEOUT)
            continue;
        if (c == '\r' || c == '\n') {
            if (!len)
                continue;
            buf[len] = 0;
            return true;
        }
        if (len + 1 < n)
            buf[len++] = (char)c;
    }
}

int main(void) {
    stdio_init_all();
    chip_init();
    char line[96];
    for (;;) {
        read_line(line, sizeof line);
        char *w = strtok(line, " ");
        if (!w)
            continue;
        if (!strcmp(w, "id")) {
            identify();
        } else if (!strcmp(w, "run")) {
            uint32_t v[4] = {0};
            bool ok = true;
            for (unsigned i = 0; i < 4 && ok; i++) {
                char *a = strtok(NULL, " ");
                ok = a != NULL;
                if (ok)
                    v[i] = strtoul(a, NULL, 0);
            }
            char *flag = strtok(NULL, " ");
            if (!ok)
                printf("{\"refused\":\"usage: run <addr> <sectors> <cycles> <seed> [selftest]\"}\n");
            else
                run(v[0], v[1], v[2], v[3], flag && !strcmp(flag, "selftest"));
        } else if (!strcmp(w, "reboot")) {
            printf("{\"rebooting\":true}\n");
            stdio_flush();
            watchdog_reboot(0, 0, 50);
            for (;;)
                tight_loop_contents();
        } else {
            printf("{\"refused\":\"unknown command\"}\n");
        }
    }
}
