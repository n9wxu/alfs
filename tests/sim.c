/*
 * The simulated NOR flash. See sim.h.
 */
#include "sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t sim_rand(sim_t *s) {
    /* xorshift32 */
    uint32_t x = s->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng = x;
    return x;
}

void sim_init(sim_t *s, uint32_t chip_size, uint32_t sector_size, uint32_t page_size, uint8_t erase_value,
              uint32_t seed) {
    memset(s, 0, sizeof(*s));
    s->chip_size = chip_size;
    s->sector_size = sector_size;
    s->page_size = page_size;
    s->erase_value = erase_value;
    s->mem = malloc(chip_size);
    s->meant = malloc(chip_size);
    s->done = calloc(chip_size, 1);
    s->page_progs = calloc(chip_size / page_size, 1);
    s->torn = calloc(chip_size, 1);
    s->wear = calloc(chip_size / sector_size, sizeof(uint32_t));
    s->sector_progs = calloc(chip_size / sector_size, sizeof(uint32_t));
    s->sector_progs_total = calloc(chip_size / sector_size, sizeof(uint64_t));
    s->sector_erased = calloc(chip_size / sector_size, 1);
    s->bad = calloc(chip_size / sector_size, 1);
    memset(s->mem, erase_value, chip_size);
    memset(s->meant, erase_value, chip_size);
    s->loss_in = -1;
    s->rng = seed ? seed : 0x2545F491u; /* xorshift32 never leaves 0 */
}

void sim_free(sim_t *s) {
    free(s->mem);
    free(s->meant);
    free(s->done);
    free(s->page_progs);
    free(s->torn);
    free(s->wear);
    free(s->sector_progs);
    free(s->sector_progs_total);
    free(s->sector_erased);
    free(s->bad);
}

void sim_call_begin(sim_t *s) {
    s->c_ops = s->c_programs = s->c_erases = s->c_bytes_read = s->c_time_us = s->c_max_op_us = 0;
}

void sim_power_after(sim_t *s, int64_t n, sim_pl_t pl) {
    s->loss_in = n;
    s->pl = pl;
}

void sim_reboot(sim_t *s) {
    s->off = false;
    s->loss_in = -1;
}

void sim_mkbad(sim_t *s, uint32_t sector, sim_bad_t mode) {
    s->bad[sector] = (uint8_t)mode;
}

void sim_flip(sim_t *s, uint32_t addr, uint8_t mask) {
    s->mem[addr] ^= mask;
}

void sim_fill_random(sim_t *s, uint32_t addr, uint32_t len) {
    for (uint32_t i = 0; i < len; i++)
        s->mem[addr + i] = (uint8_t)sim_rand(s);
}

static void violate(sim_t *s, const char *what, uint32_t addr) {
    s->violations++;
    snprintf(s->why, sizeof(s->why), "%s at 0x%lx", what, (unsigned long)addr);
}

static void add_meta(sim_t *s, uint32_t addr, uint8_t mask) {
    if (s->nmeta < SIM_META_MAX) {
        s->meta[s->nmeta].addr = addr;
        s->meta[s->nmeta].mask = mask;
        s->nmeta++;
    }
}

static void clear_meta(sim_t *s, uint32_t lo, uint32_t hi) {
    int j = 0;
    for (int i = 0; i < s->nmeta; i++)
        if (s->meta[i].addr < lo || s->meta[i].addr >= hi)
            s->meta[j++] = s->meta[i];
    s->nmeta = j;
}

/* Programs a byte: bits move only away from the erase value. */
static uint8_t apply(const sim_t *s, uint8_t old, uint8_t v) {
    return s->erase_value == 0xFF ? (uint8_t)(old & v) : (uint8_t)(old | v);
}

static void prog_byte(sim_t *s, uint32_t addr, uint8_t v) {
    s->mem[addr] = apply(s, s->mem[addr], v);
}

/* The bits of v that a program moves away from the erase value. */
static uint8_t moved(const sim_t *s, uint8_t v) {
    return s->erase_value == 0xFF ? (uint8_t)~v : v;
}

/* A byte that settles: what was meant, or the erase value's complement. */
static bool settles(const sim_t *s, uint32_t addr, uint8_t v) {
    return v == s->meant[addr] || v == (uint8_t)~s->erase_value;
}

/* Bits that read randomly settle when a program moves them. */
static void settle_meta(sim_t *s, uint32_t addr, const uint8_t *buf, uint32_t len) {
    int j = 0;
    for (int i = 0; i < s->nmeta; i++) {
        uint32_t a = s->meta[i].addr;
        if (a >= addr && a < addr + len && (moved(s, buf[a - addr]) & s->meta[i].mask))
            continue;
        s->meta[j++] = s->meta[i];
    }
    s->nmeta = j;
}

/*
 * A bit of buf[from, to) that the program moves and that was not moved in
 * before[]: the only kind a torn program can leave half done (A-9). Its
 * offset and mask, or a mask of 0 when the program moves none.
 */
static uint8_t torn_bit(sim_t *s, const uint8_t *buf, const uint8_t *before, uint32_t from, uint32_t to,
                        uint32_t *at) {
    for (int tries = 0; tries < 64 && to > from; tries++) {
        uint32_t i = from + sim_rand(s) % (to - from);
        uint8_t mask = (uint8_t)(1u << (sim_rand(s) % 8));
        if (moved(s, buf[i]) & ~moved(s, before[i]) & mask) {
            *at = i;
            return mask;
        }
    }
    return 0;
}

/* Puts a bit a program moved back to the erase value. */
static void unmove(sim_t *s, uint32_t addr, uint8_t mask) {
    s->mem[addr] = s->erase_value == 0xFF ? (uint8_t)(s->mem[addr] | mask) : (uint8_t)(s->mem[addr] & ~mask);
}

/* True when this operation is the one a power loss interrupts. */
static bool losing(sim_t *s) {
    if (s->loss_in < 0)
        return false;
    if (s->loss_in == 0) {
        s->loss_in = -1;
        return true;
    }
    s->loss_in--;
    return false;
}

static void count(sim_t *s, uint64_t t_us) {
    s->ops++;
    s->c_ops++;
    s->time_us += t_us;
    s->c_time_us += t_us;
    if (t_us > s->c_max_op_us)
        s->c_max_op_us = t_us;
}

static int sim_read(void *ctx, uint32_t addr, void *buf, uint32_t len) {
    sim_t *s = ctx;
    if (s->off)
        return ALFS_EIO;
    if (addr + len > s->chip_size || addr + len < addr) {
        violate(s, "read past the chip", addr);
        return ALFS_EIO;
    }
    if (s->read_fail_once > 0) {
        s->read_fail_once--;
        return ALFS_EIO;
    }
    uint32_t sec = addr / s->sector_size;
    uint32_t last = (addr + len - 1) / s->sector_size;
    for (uint32_t k = sec; len && k <= last; k++)
        if (s->bad[k] == BAD_READERROR)
            return ALFS_EIO;
    memcpy(buf, s->mem + addr, len);
    for (int i = 0; i < s->nmeta; i++) {
        uint32_t a = s->meta[i].addr;
        if (a >= addr && a < addr + len && (sim_rand(s) & 1))
            ((uint8_t *)buf)[a - addr] ^= s->meta[i].mask;
    }
    for (uint32_t k = sec; len && k <= last; k++)
        if (s->bad[k] == BAD_READFLIP && (sim_rand(s) & 1))
            ((uint8_t *)buf)[sim_rand(s) % len] ^= (uint8_t)(1u << (sim_rand(s) % 8));
    s->bytes_read += len;
    s->c_bytes_read += len;
    return 0;
}

static int sim_prog(void *ctx, uint32_t addr, const void *data, uint32_t len) {
    sim_t *s = ctx;
    const uint8_t *buf = data;
    if (s->off)
        return ALFS_EIO;
    if (len == 0 || len > s->page_size || addr + len > s->chip_size) {
        violate(s, "program outside the chip", addr);
        return ALFS_EIO;
    }
    if (addr / s->page_size != (addr + len - 1) / s->page_size)
        violate(s, "program across a page", addr);
    uint32_t page = addr / s->page_size;
    bool into_torn = false;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t a = addr + i;
        if (settles(s, a, buf[i]))
            continue;
        if (s->torn[a] && s->torn_ok) {
            into_torn = true;
            continue;
        }
        if (s->torn[a])
            violate(s, "program into bytes a power loss touched", a);
        if (s->done[a])
            violate(s, "byte programmed twice", a);
    }
    if (into_torn)
        s->torn_programs++;
    count(s, SIM_T_PROG_US);
    s->programs++;
    s->c_programs++;
    s->bytes_programmed += len;
    uint8_t before[256]; /* what the chip held: a page at most */
    memcpy(before, s->mem + addr, len);
    for (uint32_t i = 0; i < len; i++)
        s->meant[addr + i] = apply(s, s->meant[addr + i], buf[i]);

    if (losing(s)) {
        s->off = true;
        s->cut_addr = addr;
        s->cut_prog = true;
        if (s->pl != PL_ATOMIC)
            memset(s->torn + addr, 1, len);
        uint32_t at = 0;
        uint8_t mask;
        switch (s->pl) {
        case PL_ATOMIC:
            break;
        case PL_SOMEBITS:
            mask = torn_bit(s, buf, before, 0, 1, &at);
            prog_byte(s, addr, s->erase_value == 0xFF ? (uint8_t)~mask : mask);
            break;
        case PL_MOSTBITS:
            for (uint32_t i = 0; i < len; i++)
                prog_byte(s, addr + i, buf[i]);
            settle_meta(s, addr, buf, len);
            mask = torn_bit(s, buf, before, 0, len, &at);
            unmove(s, addr + at, mask);
            break;
        case PL_METASTABLE:
            for (uint32_t i = 0; i < len; i++)
                prog_byte(s, addr + i, buf[i]);
            settle_meta(s, addr, buf, len);
            if ((mask = torn_bit(s, buf, before, 0, len, &at)) != 0)
                add_meta(s, addr + at, mask);
            break;
        case PL_FLICKER:
            if ((mask = torn_bit(s, buf, before, 0, len, &at)) != 0)
                add_meta(s, addr + at, mask);
            break;
        case PL_TORNTAIL:
            if (len > 1) {
                uint32_t run = 1 + sim_rand(s) % (len - 1);
                for (uint32_t i = 1; i < 1 + run; i++)
                    prog_byte(s, addr + i, buf[i]);
                mask = torn_bit(s, buf, before, 1, 1 + run, &at);
                unmove(s, addr + at, mask);
            }
            break;
        default:
            break;
        }
        return ALFS_EIO;
    }

    uint32_t sec = addr / s->sector_size;
    if (s->bad[sec] == BAD_PROGERROR)
        return ALFS_EIO;
    s->sector_progs[sec]++;
    s->sector_progs_total[sec]++;
    s->last_prog_addr = addr;
    s->last_prog_len = len;
    memset(s->done + addr, 1, len);
    if (++s->page_progs[page] > s->max_page_progs)
        s->max_page_progs = s->page_progs[page];
    if (s->bad[sec] == BAD_PROGNOOP || s->bad[sec] == BAD_ERASENOOP)
        return 0;
    for (uint32_t i = 0; i < len; i++)
        prog_byte(s, addr + i, buf[i]);
    settle_meta(s, addr, buf, len);
    if (s->bad[sec] == BAD_PROGFLIP)
        s->mem[addr + sim_rand(s) % len] ^= (uint8_t)(1u << (sim_rand(s) % 8));
    return 0;
}

static void erase_range(sim_t *s, uint32_t addr, uint32_t len) {
    memset(s->mem + addr, s->erase_value, len);
    memset(s->meant + addr, s->erase_value, len);
    memset(s->done + addr, 0, len);
    memset(s->page_progs + addr / s->page_size, 0, len / s->page_size);
    memset(s->torn + addr, 0, len);
    clear_meta(s, addr, addr + len);
}

static int erase_any(sim_t *s, uint32_t addr, uint32_t len, uint64_t t_us) {
    if (s->off)
        return ALFS_EIO;
    if (addr % len || addr + len > s->chip_size) {
        violate(s, "misaligned erase", addr);
        return ALFS_EIO;
    }
    count(s, t_us);

    if (losing(s)) {
        s->off = true;
        s->cut_addr = addr;
        s->cut_prog = false;
        bool none = s->pl == PL_ATOMIC || s->pl == PL_TORNTAIL;
        memset(s->torn + addr, !none, len); /* those change nothing */
        switch (s->pl) {
        case PL_ATOMIC:
        case PL_TORNTAIL:
            break;
        case PL_SOMEBITS:
            s->mem[addr + sim_rand(s) % len] ^= (uint8_t)(1u << (sim_rand(s) % 8));
            break;
        case PL_MOSTBITS:
            erase_range(s, addr, len);
            memset(s->torn + addr, 1, len);
            s->mem[addr + sim_rand(s) % len] ^= (uint8_t)(1u << (sim_rand(s) % 8));
            break;
        case PL_METASTABLE:
            erase_range(s, addr, len);
            memset(s->torn + addr, 1, len);
            add_meta(s, addr + sim_rand(s) % len, (uint8_t)(1u << (sim_rand(s) % 8)));
            break;
        case PL_FLICKER:
            add_meta(s, addr + sim_rand(s) % len, (uint8_t)(1u << (sim_rand(s) % 8)));
            break;
        default:
            break;
        }
        return ALFS_EIO;
    }

    s->erases++;
    s->c_erases++;
    for (uint32_t k = addr / s->sector_size; k < (addr + len) / s->sector_size; k++) {
        if (s->wear_limit && s->wear[k] >= s->wear_limit)
            s->bad[k] = BAD_ERASEERROR;
        if (s->bad[k] == BAD_ERASEERROR)
            return ALFS_EIO;
    }
    for (uint32_t k = addr / s->sector_size; k < (addr + len) / s->sector_size; k++) {
        if (s->sector_erased[k] && s->sector_progs[k] == 1)
            s->ready_erases++;
        s->sector_progs[k] = 0;
        s->sector_erased[k] = 1;
        s->wear[k]++;
        if (s->bad[k] != BAD_ERASENOOP)
            erase_range(s, k * s->sector_size, s->sector_size);
    }
    return 0;
}

static int sim_erase(void *ctx, uint32_t addr) {
    sim_t *s = ctx;
    return erase_any(s, addr, s->sector_size, SIM_T_ERASE_US);
}

alfs_driver_t sim_driver(sim_t *s) {
    alfs_driver_t d = {s, sim_read, sim_prog, sim_erase};
    return d;
}
