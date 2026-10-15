/*
 * A simulated serial NOR flash for alfs's tests.
 *
 * It holds the chip in RAM and injects the faults of docs/REQUIREMENTS.md
 * section 4.4: the power-loss behaviours littlefs's emulated device names,
 * the bad-sector behaviours, wear-out, a read that fails once, and bits
 * that read randomly. It also enforces what the requirements forbid, and
 * counts each violation: a program across a page (SPD-03), a prog unit
 * programmed twice between erases (IF-02), and a program into a page an
 * interrupted operation touched (SAFE-04), byte by byte. With a prog unit of
 * 1, a byte that settles is allowed in both cases: what the programs before
 * it meant, or the erase value's complement (A-8, A-9).
 *
 * A program is an AND (for erase value 0xFF). A torn program's bit that
 * reads randomly is one its data clears, as the flash probe found; a later
 * program that clears it settles it.
 *
 * Its counters are per test and per call: sim_call_begin() starts a call.
 */
#ifndef SIM_H
#define SIM_H

#include "alfs.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PL_ATOMIC,     /* the interrupted operation is not applied             */
    PL_SOMEBITS,   /* prog: one bit of its first unit; erase: one bit flips */
    PL_MOSTBITS,   /* applied, then one bit flips                          */
    PL_METASTABLE, /* applied, then one bit reads randomly until erased or,
                    * for a program, programmed again: a bit its data clears,
                    * and none with a prog unit over 1, which corrects it */
    PL_TORNTAIL,   /* prog: first unit erased, a run after it programmed   */
    PL_FLICKER,    /* not applied, but one bit reads randomly: for a program,
                    * a bit its data clears (the probe's early abort, P8);
                    * none with a prog unit over 1 */
    PL_COUNT
} sim_pl_t;

typedef enum {
    BAD_NONE,
    BAD_PROGERROR,  /* programs fail and write nothing          */
    BAD_ERASEERROR, /* erases fail and erase nothing            */
    BAD_READERROR,  /* reads fail                               */
    BAD_PROGNOOP,   /* programs succeed and write nothing       */
    BAD_ERASENOOP,  /* erases succeed and do nothing; so do programs */
    BAD_PROGFLIP,   /* programs land with one bit wrong          */
    BAD_READFLIP,   /* reads flip one bit half the time          */
} sim_bad_t;

#define SIM_META_MAX 64

typedef struct sim {
    /* geometry */
    uint32_t chip_size, sector_size, page_size, prog_unit, large_size;
    uint8_t erase_value;

    /* state */
    uint8_t *mem;
    uint8_t *meant;      /* per byte: what every program since its erase meant */
    uint8_t *unit_done;  /* per prog unit: programmed since its erase   */
    uint8_t *page_progs; /* per page: programs since its erase          */
    uint8_t *torn;       /* per byte: an interrupted operation changed it */
    uint32_t *wear;      /* per sector: completed erases                */
    uint32_t *sector_progs;       /* per sector: programs since its erase  */
    uint64_t *sector_progs_total; /* per sector: programs ever             */
    uint8_t *sector_erased;       /* per sector: erased here at least once */
    uint64_t ready_erases; /* erases of a sector holding only its stamp (LIFE-01) */
    uint8_t *bad;        /* per sector: sim_bad_t                       */
    uint32_t wear_limit; /* a sector goes bad (ERASEERROR) past it; 0: never */
    struct {
        uint32_t addr;
        uint8_t mask;
    } meta[SIM_META_MAX];
    int nmeta;

    /* power loss: the operation numbered loss_at (counted from 1 after
     * sim_power_after) is interrupted; then the chip is off */
    int64_t loss_in;
    sim_pl_t pl;
    bool off;

    /* an erase that takes busy_calls calls: ALFS_BUSY until the last */
    int busy_calls;
    int busy_left;
    uint32_t busy_addr;

    int read_fail_once; /* the next n reads fail */

    /* the last program that completed, and the operation a power loss cut */
    uint32_t last_prog_addr, last_prog_len;
    uint32_t cut_addr;
    bool cut_prog;

    /* totals */
    uint64_t ops, programs, erases, large_erases;
    uint64_t bytes_read, bytes_programmed;
    uint64_t time_us;
    unsigned max_page_progs;

    /* this call */
    uint64_t c_ops, c_programs, c_erases, c_bytes_read, c_time_us;

    /* violations */
    int violations;
    char why[160];

    uint32_t rng;
} sim_t;

/* Typical times (docs/datasheets/README.md, W25Q128JV). */
#define SIM_T_PROG_US 400u
#define SIM_T_ERASE_US 45000u
#define SIM_T_LARGE_US 150000u

void sim_init(sim_t *s, uint32_t chip_size, uint32_t sector_size, uint32_t page_size,
              uint32_t prog_unit, uint32_t large_size, uint8_t erase_value);
void sim_free(sim_t *s);
alfs_driver_t sim_driver(sim_t *s);

void sim_call_begin(sim_t *s);

/* The (n+1)th operation from now is interrupted with behaviour pl. */
void sim_power_after(sim_t *s, int64_t n, sim_pl_t pl);
/* Power back on. Metastable bits stay until their sector is erased. */
void sim_reboot(sim_t *s);

void sim_mkbad(sim_t *s, uint32_t sector, sim_bad_t mode);
void sim_flip(sim_t *s, uint32_t addr, uint8_t mask);
void sim_fill_random(sim_t *s, uint32_t addr, uint32_t len);
uint32_t sim_rand(sim_t *s);

#endif
