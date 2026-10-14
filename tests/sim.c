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

void sim_init(sim_t *s, uint32_t chip_size, uint32_t sector_size, uint32_t page_size,
              uint32_t prog_unit, uint32_t large_size, uint8_t erase_value) {
    memset(s, 0, sizeof(*s));
    s->chip_size = chip_size;
    s->sector_size = sector_size;
    s->page_size = page_size;
    s->prog_unit = prog_unit;
    s->large_size = large_size;
    s->erase_value = erase_value;
    s->mem = malloc(chip_size);
    s->unit_done = calloc(chip_size / prog_unit, 1);
    s->page_progs = calloc(chip_size / page_size, 1);
    s->torn = calloc(chip_size / page_size, 1);
    s->wear = calloc(chip_size / sector_size, sizeof(uint32_t));
    s->sector_progs = calloc(chip_size / sector_size, sizeof(uint32_t));
    s->sector_progs_total = calloc(chip_size / sector_size, sizeof(uint64_t));
    s->sector_erased = calloc(chip_size / sector_size, 1);
    s->bad = calloc(chip_size / sector_size, 1);
    memset(s->mem, erase_value, chip_size);
    s->loss_in = -1;
    s->rng = 0x2545F491u;
}

void sim_free(sim_t *s) {
    free(s->mem);
    free(s->unit_done);
    free(s->page_progs);
    free(s->torn);
    free(s->wear);
    free(s->sector_progs);
    free(s->sector_progs_total);
    free(s->sector_erased);
    free(s->bad);
}

void sim_call_begin(sim_t *s) {
    s->c_ops = s->c_programs = s->c_erases = s->c_bytes_read = s->c_time_us = 0;
}

void sim_power_after(sim_t *s, int64_t n, sim_pl_t pl) {
    s->loss_in = n;
    s->pl = pl;
}

void sim_reboot(sim_t *s) {
    s->off = false;
    s->loss_in = -1;
    s->busy_left = 0;
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
static void prog_byte(sim_t *s, uint32_t addr, uint8_t v) {
    if (s->erase_value == 0xFF)
        s->mem[addr] &= v;
    else
        s->mem[addr] |= v;
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
    if (len == 0 || addr + len > s->chip_size) {
        violate(s, "program outside the chip", addr);
        return ALFS_EIO;
    }
    if (addr / s->page_size != (addr + len - 1) / s->page_size)
        violate(s, "program across a page", addr);
    uint32_t page = addr / s->page_size;
    if (s->torn[page])
        violate(s, "program into a page a power loss touched", addr);
    for (uint32_t u = addr / s->prog_unit; u <= (addr + len - 1) / s->prog_unit; u++)
        if (s->unit_done[u])
            violate(s, "prog unit programmed twice", u * s->prog_unit);
    count(s, SIM_T_PROG_US);
    s->programs++;
    s->c_programs++;
    s->bytes_programmed += len;

    if (losing(s)) {
        s->off = true;
        s->torn[page] = s->pl != PL_ATOMIC; /* ATOMIC changed nothing */
        uint32_t first = s->prog_unit < len ? s->prog_unit : len;
        switch (s->pl) {
        case PL_ATOMIC:
            break;
        case PL_SOMEBITS: {
            uint32_t a = addr + sim_rand(s) % first;
            prog_byte(s, a, (uint8_t)~(1u << (sim_rand(s) % 8)) ^ (uint8_t)(s->erase_value ? 0 : 0xFF));
            break;
        }
        case PL_MOSTBITS:
            for (uint32_t i = 0; i < len; i++)
                prog_byte(s, addr + i, buf[i]);
            s->mem[addr + sim_rand(s) % first] ^= (uint8_t)(1u << (sim_rand(s) % 8));
            break;
        case PL_METASTABLE:
            for (uint32_t i = 0; i < len; i++)
                prog_byte(s, addr + i, buf[i]);
            add_meta(s, addr + sim_rand(s) % len, (uint8_t)(1u << (sim_rand(s) % 8)));
            break;
        case PL_TORNTAIL:
            if (len > first) {
                uint32_t run = 1 + sim_rand(s) % (len - first);
                for (uint32_t i = first; i < first + run; i++)
                    prog_byte(s, addr + i, buf[i]);
                s->mem[addr + first + sim_rand(s) % run] ^= (uint8_t)(1u << (sim_rand(s) % 8));
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
    for (uint32_t u = addr / s->prog_unit; u <= (addr + len - 1) / s->prog_unit; u++)
        s->unit_done[u] = 1;
    if (++s->page_progs[page] > s->max_page_progs)
        s->max_page_progs = s->page_progs[page];
    if (s->bad[sec] == BAD_PROGNOOP || s->bad[sec] == BAD_ERASENOOP)
        return 0;
    for (uint32_t i = 0; i < len; i++)
        prog_byte(s, addr + i, buf[i]);
    if (s->bad[sec] == BAD_PROGFLIP)
        s->mem[addr + sim_rand(s) % len] ^= (uint8_t)(1u << (sim_rand(s) % 8));
    return 0;
}

static void erase_range(sim_t *s, uint32_t addr, uint32_t len) {
    memset(s->mem + addr, s->erase_value, len);
    memset(s->unit_done + addr / s->prog_unit, 0, len / s->prog_unit);
    memset(s->page_progs + addr / s->page_size, 0, len / s->page_size);
    memset(s->torn + addr / s->page_size, 0, len / s->page_size);
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

    if (s->busy_calls > 0) {
        if (s->busy_left == 0) {
            s->busy_left = s->busy_calls;
            s->busy_addr = addr;
        } else if (s->busy_addr != addr) {
            violate(s, "a second erase while one is suspended", addr);
        }
    }

    if (losing(s)) {
        s->off = true;
        s->busy_left = 0;
        for (uint32_t p = addr / s->page_size; p < (addr + len) / s->page_size; p++)
            s->torn[p] = s->pl != PL_ATOMIC && s->pl != PL_TORNTAIL; /* those two change nothing */
        switch (s->pl) {
        case PL_ATOMIC:
        case PL_TORNTAIL:
            break;
        case PL_SOMEBITS:
            s->mem[addr + sim_rand(s) % len] ^= (uint8_t)(1u << (sim_rand(s) % 8));
            break;
        case PL_MOSTBITS:
            erase_range(s, addr, len);
            for (uint32_t p = addr / s->page_size; p < (addr + len) / s->page_size; p++)
                s->torn[p] = 1;
            s->mem[addr + sim_rand(s) % len] ^= (uint8_t)(1u << (sim_rand(s) % 8));
            break;
        case PL_METASTABLE:
            erase_range(s, addr, len);
            for (uint32_t p = addr / s->page_size; p < (addr + len) / s->page_size; p++)
                s->torn[p] = 1;
            add_meta(s, addr + sim_rand(s) % len, (uint8_t)(1u << (sim_rand(s) % 8)));
            break;
        default:
            break;
        }
        return ALFS_EIO;
    }

    if (s->busy_calls > 0 && --s->busy_left > 0)
        return ALFS_BUSY;

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

static int sim_erase_large(void *ctx, uint32_t addr) {
    sim_t *s = ctx;
    int rc = erase_any(s, addr, s->large_size, SIM_T_LARGE_US);
    if (rc == 0)
        s->large_erases++;
    return rc;
}

alfs_driver_t sim_driver(sim_t *s) {
    alfs_driver_t d = {s, sim_read, sim_prog, sim_erase, s->large_size ? sim_erase_large : NULL};
    return d;
}
