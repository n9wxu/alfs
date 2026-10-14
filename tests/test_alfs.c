/*
 * alfs's tests, one or more for each requirement of docs/REQUIREMENTS.md.
 * Each is named in the requirement's "Verified by".
 *
 * Every alfs call a test makes goes through CALL, ESTEP or FORMAT, which
 * hold it to SPD-01 and SPD-02 (one flash operation, no erase outside an
 * erase step) and record the longest stall (SPD-08). Every test ends with
 * done(), which fails it if the simulator saw a violation: a program
 * across a page, a prog unit programmed twice, or a program into a page a
 * power loss touched.
 */
#include "alfs.h"
#include "sim.h"
#include "tf.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SECTOR 4096u
#define PAGE 256u
#define LARGE (16u * SECTOR)
#define ROW 22u

/* ---- the rig ---------------------------------------------------------- */

typedef struct {
    sim_t sim;
    alfs_driver_t drv;
    alfs_config_t cfg;
    alfs_t fs;
    uint8_t pbuf[4][PAGE];
} rig_t;

static int call_bad;          /* calls that broke SPD-01 or SPD-02 */
static char call_why[96];
static uint64_t longest_stall; /* the longest call but an erase step, us */

static void rig_open(rig_t *r, uint32_t sectors, uint32_t unit, uint32_t large) {
    uint32_t start = large ? large : SECTOR; /* the region does not start at 0 */
    uint32_t chip = start + sectors * SECTOR + (large ? large : SECTOR);
    sim_init(&r->sim, chip, SECTOR, PAGE, unit, large, 0xFF);
    sim_fill_random(&r->sim, 0, chip); /* what a used chip holds */
    r->drv = sim_driver(&r->sim);
    memset(&r->cfg, 0, sizeof(r->cfg));
    r->cfg.region_start = start;
    r->cfg.region_size = sectors * SECTOR;
    r->cfg.chip_size = chip;
    r->cfg.sector_size = SECTOR;
    r->cfg.page_size = PAGE;
    r->cfg.prog_unit = unit;
    r->cfg.large_size = large;
    r->cfg.erase_value = 0xFF;
    memset(&r->fs, 0, sizeof(r->fs));
    call_bad = 0;
    call_why[0] = 0;
    longest_stall = 0;
}

static void rig_close(rig_t *r) {
    sim_free(&r->sim);
}

static int64_t chk(rig_t *r, int64_t rc, bool erase_step, const char *what) {
    sim_t *s = &r->sim;
    uint64_t erases = s->c_ops - s->c_programs;
    if (erase_step ? (s->c_programs > 1 || erases > 1) : (s->c_ops > 1 || erases > 0)) {
        call_bad++;
        snprintf(call_why, sizeof(call_why), "%s: %llu programs, %llu erases", what,
                 (unsigned long long)s->c_programs, (unsigned long long)erases);
    }
    if (!erase_step && s->c_time_us > longest_stall)
        longest_stall = s->c_time_us;
    return rc;
}

#define CALL(r, e) (sim_call_begin(&(r)->sim), chk((r), (int64_t)(e), false, #e))
#define ESTEP(r) ((int)(sim_call_begin(&(r)->sim), chk((r), alfs_erase_step(&(r)->fs), true, "erase step")))
#define FORMAT(r)                                                                                  \
    ((int)(sim_call_begin(&(r)->sim),                                                              \
           chk((r), alfs_format(&(r)->fs, &(r)->cfg, &(r)->drv), true, "format")))
#define MOUNT(r) ((int)CALL((r), alfs_mount(&(r)->fs, &(r)->cfg, &(r)->drv)))

/* Erase steps until nothing is dirty. The number of steps, or -1. */
static int clean(rig_t *r) {
    for (int i = 0; i < 2000000; i++) {
        int rc = ESTEP(r);
        if (rc == ALFS_CLEAN)
            return i;
        if (rc < 0 && rc != ALFS_EIO)
            return -1;
        if (r->sim.off)
            return -1;
    }
    return -1;
}

static int format_only(rig_t *r) {
    int rc = FORMAT(r);
    while (rc == ALFS_BUSY)
        rc = ESTEP(r);
    return rc < 0 ? rc : 0;
}

static int setup(rig_t *r) {
    int rc = format_only(r);
    if (rc)
        return rc;
    return clean(r) < 0 ? -1 : 0;
}

/* Ends a test: no violation of the flash's rules or of SPD-01/02. */
#define DONE(r)                                                                                    \
    do {                                                                                           \
        if ((r)->sim.violations)                                                                   \
            printf("    flash: %s\n", (r)->sim.why);                                               \
        if (call_bad)                                                                              \
            printf("    call: %s\n", call_why);                                                    \
        CHECK_EQ((r)->sim.violations, 0);                                                          \
        CHECK_EQ(call_bad, 0);                                                                     \
    } while (0)

/* ---- data ------------------------------------------------------------- */

static uint32_t crc32_ref(const uint8_t *p, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* A row carries its sequence number and a check, like littlefs's W-LOG. */
static void make_row(uint32_t seq, uint8_t *row) {
    row[0] = (uint8_t)seq;
    row[1] = (uint8_t)(seq >> 8);
    row[2] = (uint8_t)(seq >> 16);
    row[3] = (uint8_t)(seq >> 24);
    for (uint32_t i = 4; i < ROW - 4; i++)
        row[i] = (uint8_t)(seq * 31u + i);
    uint32_t c = crc32_ref(row, ROW - 4);
    row[ROW - 4] = (uint8_t)c;
    row[ROW - 3] = (uint8_t)(c >> 8);
    row[ROW - 2] = (uint8_t)(c >> 16);
    row[ROW - 1] = (uint8_t)(c >> 24);
}

/* Appends d, one call at a time, adding what alfs took to *taken. 0, or
 * the first error. */
static int put_n(rig_t *r, alfs_stream_t *s, const uint8_t *d, uint32_t n, uint32_t *taken) {
    for (int guard = 0; n && guard < 100000; guard++) {
        int32_t k = (int32_t)CALL(r, alfs_append(&r->fs, s, d, n));
        if (k < 0)
            return (int)k;
        d += k;
        n -= (uint32_t)k;
        *taken += (uint32_t)k;
    }
    return n ? -100 : 0;
}

static int put(rig_t *r, alfs_stream_t *s, const uint8_t *d, uint32_t n) {
    uint32_t taken = 0;
    return put_n(r, s, d, n, &taken);
}

/* Reads a whole stream. Its length, or an error. */
static int32_t slurp(rig_t *r, uint32_t id, uint8_t *out, uint32_t cap) {
    alfs_stream_t s;
    int rc = (int)CALL(r, alfs_open_read(&r->fs, &s, id));
    if (rc)
        return rc;
    uint32_t n = 0;
    for (int guard = 0; guard < 1000000; guard++) {
        uint32_t want = cap - n < 300 ? cap - n : 300;
        int32_t k = (int32_t)CALL(r, alfs_read(&r->fs, &s, out + n, want));
        if (k < 0)
            return k;
        if (k == 0)
            return (int32_t)n;
        n += (uint32_t)k;
        if (n >= cap)
            return -200; /* more than was ever written */
    }
    return -201;
}

/*
 * A logging run: rows produced at `rate` a second, appended every 200 ms,
 * synced every second, `ms` long, with an erase step every 20 ms if asked.
 * Rows go to exp[]; *synced is the length a successful sync covered. Stops
 * early when the power is lost. 0, or the first error that is not a power
 * loss.
 */
typedef struct {
    uint8_t *exp;
    uint32_t len, cap;
    uint32_t synced;
    uint32_t seq;
} log_t;

static int log_run(rig_t *r, alfs_stream_t *s, log_t *lg, uint32_t rate, uint32_t ms, bool steps) {
    uint32_t due = 0;
    for (uint32_t t = 20; t <= ms; t += 20) {
        if (steps) {
            int rc = ESTEP(r);
            if (r->sim.off)
                return 0;
            if (rc < 0 && rc != ALFS_EIO)
                return rc;
        }
        if (t % 200)
            continue;
        due += rate;
        uint32_t n = 0;
        while (due >= 5 && lg->len + ROW <= lg->cap) {
            make_row(lg->seq++, lg->exp + lg->len + n * ROW);
            n++;
            due -= 5;
        }
        if (n) {
            uint32_t taken = 0;
            int rc = put_n(r, s, lg->exp + lg->len, n * ROW, &taken);
            lg->len += taken;
            if (r->sim.off)
                return 0;
            if (rc)
                return rc;
        }
        if (t % 1000 == 0) {
            int rc = (int)CALL(r, alfs_sync(&r->fs, s));
            if (r->sim.off)
                return 0;
            if (rc)
                return rc;
            lg->synced = lg->len;
        }
    }
    return 0;
}

static void log_init(log_t *lg, uint32_t cap) {
    memset(lg, 0, sizeof(*lg));
    lg->exp = malloc(cap);
    lg->cap = cap;
}

/* ---- IF ---------------------------------------------------------------- */

static void if_geometry(void) {
    static const uint32_t units[] = {1, 16, 256};
    for (unsigned u = 0; u < 3; u++) {
        rig_t r;
        rig_open(&r, 8, units[u], 0);
        CHECK_EQ(setup(&r), 0);
        CHECK_EQ(MOUNT(&r), 0);
        DONE(&r);
        rig_close(&r);
    }

    rig_t r;
    rig_open(&r, 8, 1, 0);
    alfs_config_t good = r.cfg;
    struct {
        const char *what;
        uint32_t *field;
        uint32_t value;
    } bad[] = {
        {"region not sector-aligned", &r.cfg.region_start, SECTOR + 1},
        {"region past the chip", &r.cfg.region_size, 64 * SECTOR},
        {"sector not a power of two", &r.cfg.sector_size, 3000},
        {"sector under four pages", &r.cfg.sector_size, 512},
        {"page not a power of two", &r.cfg.page_size, 200},
        {"prog unit over a page", &r.cfg.prog_unit, 512},
        {"prog unit not a power of two", &r.cfg.prog_unit, 3},
        {"empty region", &r.cfg.region_size, 0},
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        r.cfg = good;
        *bad[i].field = bad[i].value;
        int rc = FORMAT(&r);
        if (rc != ALFS_EGEOMETRY && rc != ALFS_EINVAL)
            printf("    accepted: %s (%d)\n", bad[i].what, rc);
        CHECK(rc == ALFS_EGEOMETRY || rc == ALFS_EINVAL);
        CHECK_EQ(r.sim.c_ops, 0);
    }
    r.cfg = good;
    r.cfg.erase_value = 0x55;
    CHECK(FORMAT(&r) < 0);
    CHECK_EQ(r.sim.c_ops, 0);

    /* A region formatted with one geometry does not mount with another. */
    r.cfg = good;
    CHECK_EQ(setup(&r), 0);
    r.cfg.prog_unit = 16;
    CHECK_EQ(MOUNT(&r), ALFS_EGEOMETRY);
    r.cfg = good;
    r.cfg.region_size = 6 * SECTOR;
    CHECK_EQ(MOUNT(&r), ALFS_EGEOMETRY);
    DONE(&r);
    rig_close(&r);
}

/* The stamp's fields, little-endian, where docs/SPEC.md puts them. */
static void if_endian(void) {
    rig_t r;
    rig_open(&r, 8, 1, 0);
    CHECK_EQ(setup(&r), 0);
    const uint8_t *st = r.sim.mem + r.cfg.region_start;
    CHECK(memcmp(st, "alfs", 4) == 0);
    CHECK_EQ(st[4], 1);  /* version            */
    CHECK_EQ(st[5], 12); /* log2(sector size)  */
    CHECK_EQ(st[6], 8);  /* log2(page size)    */
    CHECK_EQ(st[7], 0);  /* log2(prog unit)    */
    CHECK_EQ(st[8] | st[9] << 8, 8);
    CHECK_EQ(st[10] | st[11] << 8 | st[12] << 16 | (uint32_t)st[13] << 24, 1); /* generation */
    CHECK_EQ(st[14] | st[15] << 8 | st[16] << 16 | (uint32_t)st[17] << 24, 1); /* erase count */
    uint32_t c = crc32_ref(st, 18);
    CHECK_EQ(st[18] | st[19] << 8 | st[20] << 16 | (uint32_t)st[21] << 24, c);
    DONE(&r);
    rig_close(&r);
}

static void if_list_streams(void) {
    rig_t r;
    rig_open(&r, 16, 1, 0);
    CHECK_EQ(setup(&r), 0);
    uint32_t ids[5];
    uint8_t row[ROW];
    for (uint32_t i = 0; i < 5; i++) {
        alfs_stream_t s;
        CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 100 + i)), 0);
        make_row(i, row);
        CHECK_EQ(put(&r, &s, row, ROW), 0);
        CHECK_EQ(CALL(&r, alfs_close(&r.fs, &s)), 0);
        ids[i] = s.id;
        if (i)
            CHECK(ids[i] > ids[i - 1]);
    }
    CHECK_EQ(CALL(&r, alfs_delete(&r.fs, ids[2])), 0);
    for (int pass = 0; pass < 2; pass++) {
        alfs_info_t in;
        uint32_t after = 0, seen = 0;
        while (CALL(&r, alfs_next(&r.fs, after, &in)) == 0) {
            CHECK(in.id != ids[2]);
            CHECK(in.id > after);
            after = in.id;
            seen++;
        }
        CHECK_EQ(seen, 4);
        CHECK_EQ(MOUNT(&r), 0); /* the second pass after a remount */
    }
    DONE(&r);
    rig_close(&r);
}

/* ---- basics ---------------------------------------------------------- */

static void basic_round_trip(void) {
    static const uint32_t units[] = {1, 16, 256};
    static const uint32_t sizes[] = {1, 7, 250, 251, 256, 4000, 20000};
    for (unsigned u = 0; u < 3; u++) {
        for (unsigned z = 0; z < 7; z++) {
            rig_t r;
            rig_open(&r, 16, units[u], 0);
            CHECK_EQ(setup(&r), 0);
            uint8_t *d = malloc(sizes[z]), *o = malloc(sizes[z] + 1024);
            for (uint32_t i = 0; i < sizes[z]; i++)
                d[i] = (uint8_t)(i * 7 + z);
            alfs_stream_t s;
            CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
            CHECK_EQ(put(&r, &s, d, sizes[z]), 0);
            CHECK_EQ(CALL(&r, alfs_close(&r.fs, &s)), 0);
            CHECK_EQ(slurp(&r, s.id, o, sizes[z] + 1024), (int32_t)sizes[z]);
            CHECK(memcmp(o, d, sizes[z]) == 0);
            CHECK_EQ(MOUNT(&r), 0);
            CHECK_EQ(slurp(&r, s.id, o, sizes[z] + 1024), (int32_t)sizes[z]);
            CHECK(memcmp(o, d, sizes[z]) == 0);
            free(d);
            free(o);
            DONE(&r);
            rig_close(&r);
        }
    }
}

/* ---- SPD -------------------------------------------------------------- */

/* W-LOG at both rates and every prog unit, and W-HR: every call held to
 * SPD-01/02 by CALL, every erase step by ESTEP. */
static void spd_no_erase_outside_erase_step(void) {
    static const uint32_t units[] = {1, 16, 256};
    static const uint32_t rates[] = {1, 50};
    for (unsigned u = 0; u < 3; u++)
        for (unsigned k = 0; k < 2; k++) {
            rig_t r;
            rig_open(&r, 64, units[u], 0);
            CHECK_EQ(setup(&r), 0);
            alfs_stream_t s;
            CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
            log_t lg;
            log_init(&lg, 1 << 20);
            CHECK_EQ(log_run(&r, &s, &lg, rates[k], 120000, true), 0);
            free(lg.exp);
            DONE(&r);
            rig_close(&r);
        }
}

static void spd_one_operation_per_call(void) {
    /* W-HR: 480 bytes every 20 ms, synced every second. */
    rig_t r;
    rig_open(&r, 256, 1, 0);
    CHECK_EQ(setup(&r), 0);
    alfs_stream_t s;
    CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 2)), 0);
    uint8_t blk[480];
    for (uint32_t t = 1; t <= 30 * 50; t++) {
        for (uint32_t i = 0; i < sizeof(blk); i++)
            blk[i] = (uint8_t)(t + i);
        CHECK_EQ(put(&r, &s, blk, sizeof(blk)), 0);
        if (t % 50 == 0)
            CHECK_EQ(CALL(&r, alfs_sync(&r.fs, &s)), 0);
    }
    DONE(&r);
    rig_close(&r);
}

static void spd_enospc_without_erase(void) {
    rig_t r;
    rig_open(&r, 6, 1, 0);
    CHECK_EQ(format_only(&r), 0); /* one sector ready, five dirty */
    CHECK_EQ(ESTEP(&r), ALFS_OK); /* two ready */
    alfs_stream_t s;
    CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 3)), 0);
    uint8_t *d = malloc(6 * SECTOR), *o = malloc(6 * SECTOR);
    for (uint32_t i = 0; i < 6 * SECTOR; i++)
        d[i] = (uint8_t)(i ^ (i >> 8));
    uint32_t n = 0;
    int rc = 0;
    while (n < 6 * SECTOR) {
        int32_t k = (int32_t)CALL(&r, alfs_append(&r.fs, &s, d + n, 6 * SECTOR - n));
        if (k < 0) {
            rc = (int)k;
            break;
        }
        n += (uint32_t)k;
    }
    CHECK_EQ(rc, ALFS_ENOSPC);
    CHECK_EQ(r.sim.c_ops, 0);
    CHECK_EQ(CALL(&r, alfs_sync(&r.fs, &s)), ALFS_ENOSPC);
    CHECK_EQ(r.sim.c_ops, 0);
    /* One erase step makes room; the bytes held are written. */
    CHECK_EQ(ESTEP(&r), ALFS_OK);
    CHECK_EQ(CALL(&r, alfs_sync(&r.fs, &s)), 0);
    CHECK_EQ(CALL(&r, alfs_close(&r.fs, &s)), 0);
    CHECK_EQ(slurp(&r, s.id, o, 6 * SECTOR), (int32_t)n);
    CHECK(memcmp(o, d, n) == 0);
    free(d);
    free(o);
    DONE(&r);
    rig_close(&r);
}

static void spd_busy_erase_resumes(void) {
    rig_t r;
    rig_open(&r, 8, 1, 0);
    r.sim.busy_calls = 5;
    CHECK_EQ(FORMAT(&r), ALFS_BUSY);
    int busy = 0, ok = 0, rc;
    while ((rc = ESTEP(&r)) != ALFS_CLEAN) {
        CHECK(rc == ALFS_BUSY || rc == ALFS_OK);
        busy += rc == ALFS_BUSY;
        ok += rc == ALFS_OK;
    }
    /* 8 erases of 5 calls each: format's first call, then 4 + 7 * 5 - 7. */
    CHECK_EQ(ok, 8);
    CHECK_EQ(busy, 3 + 7 * 4);
    alfs_status_t st;
    alfs_status(&r.fs, &st);
    CHECK_EQ(st.ready, 8);
    DONE(&r);
    rig_close(&r);
}

static void spd_mount_reads_bounded(void) {
    rig_t r;
    rig_open(&r, 128, 1, 0);
    CHECK_EQ(setup(&r), 0);
    log_t lg;
    log_init(&lg, 1 << 20);
    for (int k = 0; k < 4; k++) {
        alfs_stream_t s;
        CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], (uint32_t)k)), 0);
        lg.len = 0;
        CHECK_EQ(log_run(&r, &s, &lg, 50, 30000, false), 0);
        CHECK_EQ(CALL(&r, alfs_close(&r.fs, &s)), 0);
    }
    CHECK_EQ(MOUNT(&r), 0);
    CHECK_EQ(r.sim.c_ops, 0);
    CHECK(r.sim.c_bytes_read <= 64u * 128u);
    free(lg.exp);
    DONE(&r);
    rig_close(&r);
}

static void spd_reopen_reads_bounded(void) {
    rig_t r;
    rig_open(&r, 64, 1, 0);
    CHECK_EQ(setup(&r), 0);
    alfs_stream_t s;
    CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 9)), 0);
    uint8_t b = 0x5A;
    for (int i = 0; i < 3000; i++) { /* the last sector full of tiny chunks */
        CHECK_EQ(put(&r, &s, &b, 1), 0);
        CHECK_EQ(CALL(&r, alfs_sync(&r.fs, &s)), 0);
    }
    uint32_t id = s.id;
    CHECK_EQ(MOUNT(&r), 0);
    CHECK_EQ(CALL(&r, alfs_open_append(&r.fs, &s, r.pbuf[0], id)), 0);
    CHECK_EQ(r.sim.c_ops, 0);
    CHECK(r.sim.c_bytes_read <= SECTOR + 64u * 64u);
    DONE(&r);
    rig_close(&r);
}

static void spd_wlog_longest_stall(void) {
    static const uint32_t units[] = {1, 16, 256};
    for (unsigned u = 0; u < 3; u++) {
        rig_t r;
        rig_open(&r, 256, units[u], 0);
        CHECK_EQ(setup(&r), 0);
        alfs_stream_t s;
        CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
        log_t lg;
        log_init(&lg, 1 << 20);
        CHECK_EQ(log_run(&r, &s, &lg, 50, 600000, false), 0);
        CHECK(longest_stall <= SIM_T_PROG_US);
        free(lg.exp);
        DONE(&r);
        rig_close(&r);
    }
}

/* Reads per append and sync do not grow with the region. */
static void spd_cpu_linear(void) {
    uint64_t reads[2];
    double secs[2];
    static const uint32_t sizes[] = {32, 2048};
    for (int k = 0; k < 2; k++) {
        rig_t r;
        rig_open(&r, sizes[k], 1, 0);
        CHECK_EQ(setup(&r), 0);
        alfs_stream_t s;
        CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
        uint8_t blk[64];
        memset(blk, 0xA5, sizeof(blk));
        uint64_t before = r.sim.bytes_read;
        clock_t t0 = clock();
        for (int i = 0; i < 1000; i++) { /* 64 kB: 16 sectors */
            CHECK_EQ(put(&r, &s, blk, sizeof(blk)), 0);
            CHECK_EQ(CALL(&r, alfs_sync(&r.fs, &s)), 0);
        }
        secs[k] = (double)(clock() - t0) / CLOCKS_PER_SEC;
        reads[k] = r.sim.bytes_read - before;
        DONE(&r);
        rig_close(&r);
    }
    CHECK(reads[1] <= reads[0] + 64u * 64u);
    CHECK(secs[1] <= 2.0 * secs[0] + 0.05);
}

/* ---- LIFE ------------------------------------------------------------- */

/*
 * W-RING on a small region: W-LOG at 50 rows a second, a new stream every
 * `every` rows, the oldest deleted while fewer than two streams' worth of
 * sectors are ready, an erase step every 20 ms. Runs until every sector has
 * been erased `cycles` times.
 */
static int ring(rig_t *r, uint32_t cycles, uint32_t every, uint32_t max_ticks) {
    uint32_t ids[64], nids = 0, rows = 0, seq = 0;
    alfs_stream_t s;
    uint8_t row[ROW];
    if (CALL(r, alfs_create(&r->fs, &s, r->pbuf[0], 0)))
        return -1;
    ids[nids++] = s.id;
    uint32_t sectors = r->cfg.region_size / SECTOR;
    uint32_t need = (every * ROW) / (SECTOR - 64) + 2;
    for (uint32_t tick = 1; tick <= max_ticks; tick++) {
        int rc = ESTEP(r);
        if (rc < 0 && rc != ALFS_EIO)
            return -2;
        if (tick % 10 == 0) { /* 200 ms: 10 rows */
            for (int i = 0; i < 10; i++) {
                make_row(seq++, row);
                rc = put(r, &s, row, ROW);
                if (rc == ALFS_ENOSPC)
                    return -3;
                if (rc && rc != ALFS_EPROG && rc != ALFS_ELOST)
                    return -4;
                rows++;
            }
        }
        if (tick % 50 == 0 && CALL(r, alfs_sync(&r->fs, &s)) < 0)
            return -5;
        if (rows >= every) {
            if (CALL(r, alfs_close(&r->fs, &s)))
                return -6;
            if (CALL(r, alfs_create(&r->fs, &s, r->pbuf[0], tick)))
                return -7;
            if (nids == 64)
                return -8;
            ids[nids++] = s.id;
            rows = 0;
        }
        alfs_status_t st;
        alfs_status(&r->fs, &st);
        while (st.ready < 2 * need && nids > 1) {
            if (CALL(r, alfs_delete(&r->fs, ids[0])))
                return -9;
            memmove(ids, ids + 1, --nids * sizeof(ids[0]));
            alfs_status(&r->fs, &st);
        }
        uint32_t least = UINT32_MAX;
        for (uint32_t k = 1; k <= sectors; k++)
            if (!r->sim.bad[k] && r->sim.wear[k] < least)
                least = r->sim.wear[k];
        if (least >= cycles)
            return 0;
    }
    return -10;
}

static void life_erase_only_dirty(void) {
    rig_t r;
    rig_open(&r, 32, 1, 0);
    CHECK_EQ(setup(&r), 0);
    CHECK_EQ(ring(&r, 3, 800, 5000000), 0);
    CHECK_EQ(r.sim.ready_erases, 0);
    DONE(&r);
    rig_close(&r);
}

static void life_write_amplification(void) {
    static const uint32_t units[] = {1, 16, 256};
    static const uint32_t rates[] = {1, 50};
    for (unsigned u = 0; u < 3; u++)
        for (unsigned k = 0; k < 2; k++) {
            rig_t r;
            rig_open(&r, 256, units[u], 0);
            CHECK_EQ(setup(&r), 0);
            uint64_t p0 = r.sim.programs, b0 = r.sim.bytes_programmed;
            alfs_stream_t s;
            CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
            log_t lg;
            log_init(&lg, 1 << 20);
            CHECK_EQ(log_run(&r, &s, &lg, rates[k], 600000, false), 0);
            CHECK_EQ(CALL(&r, alfs_close(&r.fs, &s)), 0);
            uint64_t progs = r.sim.programs - p0, bytes = r.sim.bytes_programmed - b0;
            alfs_status_t st;
            alfs_status(&r.fs, &st);
            uint64_t used = 256 - st.ready;
            uint64_t per_sector = units[u] * 3 > 64 ? units[u] * 3 : 64;
            uint64_t bound = lg.len + progs * (12 + units[u]) + used * per_sector;
            if (bytes > bound)
                printf("    unit %u rate %u: %llu programmed, bound %llu\n", units[u], rates[k],
                       (unsigned long long)bytes, (unsigned long long)bound);
            CHECK(bytes <= bound);
            free(lg.exp);
            DONE(&r);
            rig_close(&r);
        }
}

static void life_wlog_erases(void) {
    rig_t r;
    rig_open(&r, 256, 1, 0);
    CHECK_EQ(setup(&r), 0);
    uint64_t b0 = r.sim.bytes_programmed;
    alfs_stream_t s;
    CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
    log_t lg;
    log_init(&lg, 1 << 20);
    CHECK_EQ(log_run(&r, &s, &lg, 50, 600000, false), 0);
    CHECK_EQ(CALL(&r, alfs_close(&r.fs, &s)), 0);
    uint64_t filled = (r.sim.bytes_programmed - b0 + SECTOR - 1) / SECTOR;
    uint64_t e0 = r.sim.erases;
    CHECK_EQ(CALL(&r, alfs_delete(&r.fs, s.id)), 0);
    CHECK(clean(&r) >= 0);
    uint64_t erases = r.sim.erases - e0;
    printf("    W-LOG 50 rows/s, 10 min: %llu erases (littlefs v2.11.3: 807)\n",
           (unsigned long long)erases);
    CHECK(erases <= filled + 1);
    free(lg.exp);
    DONE(&r);
    rig_close(&r);
}

static void life_wear_level(void) {
    rig_t r;
    rig_open(&r, 32, 1, 0);
    CHECK_EQ(setup(&r), 0);
    CHECK_EQ(ring(&r, 10, 800, 20000000), 0);
    uint32_t lo = UINT32_MAX, hi = 0;
    for (uint32_t k = 1; k <= 32; k++) {
        if (r.sim.wear[k] < lo)
            lo = r.sim.wear[k];
        if (r.sim.wear[k] > hi)
            hi = r.sim.wear[k];
    }
    printf("    wear %u to %u\n", lo, hi);
    CHECK(hi - lo <= 2);
    DONE(&r);
    rig_close(&r);
}

static void life_erase_count_persists(void) {
    rig_t r;
    rig_open(&r, 32, 1, 0);
    CHECK_EQ(setup(&r), 0);
    CHECK_EQ(ring(&r, 4, 800, 20000000), 0);
    CHECK_EQ(MOUNT(&r), 0);
    alfs_status_t st;
    alfs_status(&r.fs, &st);
    uint32_t lo = UINT32_MAX, hi = 0;
    for (uint32_t k = 1; k <= 32; k++) {
        if (r.sim.wear[k] < lo)
            lo = r.sim.wear[k];
        if (r.sim.wear[k] > hi)
            hi = r.sim.wear[k];
    }
    /* Sectors in use keep the count of their last erase. */
    CHECK_EQ(st.min_wear, lo);
    CHECK_EQ(st.max_wear, hi);
    DONE(&r);
    rig_close(&r);
}

static void life_partial_program_cap(void) {
    static const uint8_t caps[] = {0, 4};
    for (int c = 0; c < 2; c++) {
        rig_t r;
        rig_open(&r, 16, 1, 0);
        r.cfg.max_page_programs = caps[c];
        CHECK_EQ(setup(&r), 0);
        alfs_stream_t s;
        CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
        uint8_t row[ROW];
        for (uint32_t i = 0; i < 1000; i++) {
            make_row(i, row);
            CHECK_EQ(put(&r, &s, row, 3), 0); /* tiny syncs: many per page */
            CHECK_EQ(CALL(&r, alfs_sync(&r.fs, &s)), 0);
        }
        CHECK(r.sim.max_page_progs <= (caps[c] ? caps[c] : 16u));
        DONE(&r);
        rig_close(&r);
    }
}

static void life_large_erase(void) {
    rig_t r;
    rig_open(&r, 64, 1, LARGE); /* four aligned blocks */
    CHECK_EQ(setup(&r), 0);
    /* The first block holds the formatted sector, so it goes sector by
     * sector; the other three each go in one large erase. */
    CHECK_EQ(r.sim.large_erases, 3);
    CHECK_EQ(r.sim.erases - r.sim.large_erases, 16);
    alfs_status_t st;
    alfs_status(&r.fs, &st);
    CHECK_EQ(st.ready, 64);
    DONE(&r);
    rig_close(&r);
}

/* ---- SAFE ------------------------------------------------------------- */

/*
 * Power lost at operation k of a logging run that also erases, with each
 * behaviour; then a remount, a read, a resume and a read again. Some cases
 * lose power a second time during the resume. Returns 1 when k is past the
 * run's last operation.
 */
static void powerloss_case(sim_pl_t pl, uint32_t unit, int64_t k, bool twice, int *past) {
    rig_t r;
    rig_open(&r, 10, unit, 0);
    *past = 1;
    CHECK_EQ(format_only(&r), 0);
    for (int i = 0; i < 3; i++)
        CHECK(ESTEP(&r) >= 0);
    *past = 0;
    log_t lg;
    log_init(&lg, 64 * 1024);
    alfs_stream_t s;
    CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 77)), 0);
    sim_power_after(&r.sim, k, pl);
    int rc = log_run(&r, &s, &lg, 50, 20000, true);
    if (!r.sim.off) {
        free(lg.exp);
        rig_close(&r);
        *past = 1;
        CHECK_EQ(rc, 0);
        return;
    }
    uint32_t synced = lg.synced, appended = lg.len;
    sim_reboot(&r.sim);
    memset(&r.fs, 0, sizeof(r.fs));
    CHECK_EQ(MOUNT(&r), 0);

    uint8_t *got = malloc(64 * 1024);
    alfs_info_t in;
    uint32_t have = 0;
    if (CALL(&r, alfs_next(&r.fs, 0, &in)) == 0) {
        int32_t n = slurp(&r, in.id, got, 64 * 1024);
        if (n < (int32_t)synced || n > (int32_t)appended || memcmp(got, lg.exp, (size_t)n))
            printf("    pl %d unit %u k %lld: read %d, synced %u, appended %u\n", pl, unit,
                   (long long)k, n, synced, appended);
        CHECK(n >= (int32_t)synced);
        CHECK(n <= (int32_t)appended);
        CHECK(memcmp(got, lg.exp, (size_t)n) == 0);
        have = (uint32_t)n;
        CHECK_EQ(CALL(&r, alfs_open_append(&r.fs, &s, r.pbuf[0], in.id)), 0);
    } else {
        CHECK_EQ(synced, 0);
        CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 77)), 0);
    }

    /* Resume: the survivors, then new rows. */
    lg.len = have;
    lg.synced = have;
    if (twice)
        sim_power_after(&r.sim, (int64_t)(sim_rand(&r.sim) % 40), pl);
    rc = log_run(&r, &s, &lg, 50, 6000, true);
    if (!r.sim.off) {
        CHECK_EQ(rc, 0);
        CHECK_EQ(CALL(&r, alfs_close(&r.fs, &s)), 0);
        lg.synced = lg.len;
    } else {
        sim_reboot(&r.sim);
        memset(&r.fs, 0, sizeof(r.fs));
        CHECK_EQ(MOUNT(&r), 0);
    }
    int32_t n = slurp(&r, s.id, got, 64 * 1024);
    if (n < 0 && lg.synced == 0)
        n = 0; /* never synced after a create: no stream */
    CHECK(n >= (int32_t)lg.synced);
    CHECK(n <= (int32_t)lg.len);
    CHECK(memcmp(got, lg.exp, (size_t)n) == 0);
    free(got);
    free(lg.exp);
    DONE(&r);
    rig_close(&r);
}

static void powerloss_all(bool twice) {
    static const uint32_t units[] = {1, 16};
    for (int pl = 0; pl < PL_COUNT; pl++)
        for (unsigned u = 0; u < 2; u++)
            for (int64_t k = 0;; k++) {
                int past = 0;
                powerloss_case((sim_pl_t)pl, units[u], k, twice && k % 3 == 0, &past);
                if (past && !tf_failed)
                    break;
                if (tf_failed) {
                    printf("    behaviour %d, unit %u, power lost at operation %lld\n", pl,
                           units[u], (long long)k);
                    return;
                }
            }
}

static void safe_powerloss_durable(void) {
    powerloss_all(false);
}

static void safe_powerloss_bound(void) {
    powerloss_all(true);
}

/* SAFE-04 is the simulator's check, made in every power-loss case; this
 * names it for the requirement. */
static void safe_no_program_after_torn(void) {
    powerloss_all(false);
}

static void safe_no_false_data(void) {
    for (int trial = 0; trial < 200; trial++) {
        rig_t r;
        rig_open(&r, 16, 1, 0);
        CHECK_EQ(setup(&r), 0);
        alfs_stream_t s;
        CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
        log_t lg;
        log_init(&lg, 1 << 16);
        CHECK_EQ(log_run(&r, &s, &lg, 50, 20000, false), 0);
        CHECK_EQ(CALL(&r, alfs_close(&r.fs, &s)), 0);
        /* Flip one bit in a chunk: past the sector's headers. */
        uint32_t sector = r.fs.cfg->region_start + (uint32_t)(trial % 2) * SECTOR;
        uint32_t at = sector + 64 + sim_rand(&r.sim) % (SECTOR - 64);
        sim_flip(&r.sim, at, (uint8_t)(1u << (trial % 8)));
        uint8_t *got = malloc(1 << 16);
        int32_t n = slurp(&r, s.id, got, 1 << 16);
        CHECK(n >= 0);
        /* What was read is the stream with at most one run cut out: the
         * rest of the damaged page. No damaged byte comes back. */
        uint32_t a = 0, len = lg.len, got_n = (uint32_t)n;
        while (a < got_n && got[a] == lg.exp[a])
            a++;
        CHECK(got_n <= len);
        CHECK(memcmp(got + a, lg.exp + len - (got_n - a), got_n - a) == 0);
        alfs_status_t st;
        alfs_status(&r.fs, &st);
        CHECK(st.n.check_failures <= 1);
        CHECK_EQ(got_n < len, st.n.check_failures == 1);
        free(got);
        free(lg.exp);
        DONE(&r);
        rig_close(&r);
    }
}

static void safe_append_only_verified_ready(void) {
    /* An ERASENOOP sector is found out on the ground and never written. */
    rig_t r;
    rig_open(&r, 8, 1, 0);
    CHECK_EQ(format_only(&r), 0);
    sim_mkbad(&r.sim, 1 + 3, BAD_ERASENOOP); /* region sector 3 */
    CHECK(clean(&r) >= 0);
    uint64_t before = r.sim.sector_progs_total[4];
    alfs_status_t st;
    alfs_status(&r.fs, &st);
    CHECK_EQ(st.bad, 1);
    alfs_stream_t s;
    CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
    log_t lg;
    log_init(&lg, 1 << 16);
    int rc = log_run(&r, &s, &lg, 50, 40000, false);
    CHECK(rc == 0 || rc == ALFS_ENOSPC);
    CHECK_EQ(r.sim.sector_progs_total[4], before);
    free(lg.exp);
    DONE(&r);
    rig_close(&r);
    /* Power lost in erase steps is covered by every power-loss case. */
}

static void safe_prog_failure(void) {
    static const sim_bad_t modes[] = {BAD_PROGERROR, BAD_PROGNOOP, BAD_PROGFLIP};
    for (unsigned m = 0; m < 3; m++) {
        rig_t r;
        rig_open(&r, 16, 1, 0);
        CHECK_EQ(setup(&r), 0);
        alfs_stream_t s;
        CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
        log_t lg;
        log_init(&lg, 1 << 16);
        CHECK_EQ(log_run(&r, &s, &lg, 50, 3000, false), 0);
        /* The sector being written goes bad. */
        sim_mkbad(&r.sim, r.cfg.region_start / SECTOR + s.sector, modes[m]);
        int failures = 0;
        for (int t = 0; t < 40; t++) {
            uint8_t row[ROW];
            make_row(lg.seq, row);
            uint32_t off = 0;
            for (int guard = 0; off < ROW && guard < 100; guard++) {
                int32_t k = (int32_t)CALL(&r, alfs_append(&r.fs, &s, row + off, ROW - off));
                if (k == ALFS_EPROG) {
                    failures++; /* nothing taken; the held bytes go to a fresh sector */
                    continue;
                }
                CHECK(k >= 0);
                off += (uint32_t)k;
            }
            CHECK_EQ(off, ROW);
            memcpy(lg.exp + lg.len, row, ROW);
            lg.len += ROW;
            lg.seq++;
            int rc = (int)CALL(&r, alfs_sync(&r.fs, &s));
            while (rc == ALFS_EPROG) {
                failures++;
                rc = (int)CALL(&r, alfs_sync(&r.fs, &s));
            }
            CHECK_EQ(rc, 0);
        }
        CHECK(failures >= 1);
        CHECK_EQ(CALL(&r, alfs_close(&r.fs, &s)), 0);
        uint8_t *got = malloc(1 << 16);
        int32_t n = slurp(&r, s.id, got, 1 << 16);
        CHECK_EQ(n, (int32_t)lg.len);
        CHECK(memcmp(got, lg.exp, lg.len) == 0);
        alfs_status_t st;
        alfs_status(&r.fs, &st);
        CHECK(st.n.prog_failures >= 1);
        CHECK_EQ(st.n.bytes_lost, 0);
        free(got);
        free(lg.exp);
        DONE(&r);
        rig_close(&r);
    }

    /* With nothing that programs, the bytes are dropped and counted. */
    rig_t r;
    rig_open(&r, 8, 1, 0);
    CHECK_EQ(setup(&r), 0);
    for (uint32_t k = 1; k <= 8; k++)
        sim_mkbad(&r.sim, k, BAD_PROGERROR);
    alfs_stream_t s;
    CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
    uint8_t row[ROW] = {0};
    CHECK_EQ(put(&r, &s, row, ROW), 0);
    int rc, tries = 0;
    while ((rc = (int)CALL(&r, alfs_sync(&r.fs, &s))) == ALFS_EPROG)
        tries++;
    CHECK_EQ(rc, ALFS_ELOST);
    CHECK_EQ(tries, 2);
    alfs_status_t st;
    alfs_status(&r.fs, &st);
    CHECK_EQ(st.n.bytes_lost, ROW);
    DONE(&r);
    rig_close(&r);
}

static void safe_bad_sector(void) {
    static const sim_bad_t modes[] = {BAD_ERASEERROR, BAD_ERASENOOP};
    for (unsigned m = 0; m < 2; m++) {
        rig_t r;
        rig_open(&r, 8, 1, 0);
        CHECK_EQ(format_only(&r), 0);
        sim_mkbad(&r.sim, 1 + 5, modes[m]);
        CHECK(clean(&r) >= 0);
        alfs_status_t st;
        alfs_status(&r.fs, &st);
        CHECK_EQ(st.bad, 1);
        CHECK_EQ(st.ready, 7);
        CHECK_EQ(st.n.erase_failures, 1);
        /* After a remount it is dirty again; its next erase step finds it
         * bad again, and nothing is written to it. */
        CHECK_EQ(MOUNT(&r), 0);
        CHECK(clean(&r) >= 0);
        alfs_status(&r.fs, &st);
        CHECK_EQ(st.bad, 1);
        CHECK_EQ(r.sim.sector_progs_total[6], 0);
        DONE(&r);
        rig_close(&r);
    }

    /* Wear-out under W-RING: capacity shrinks, logging goes on. */
    rig_t r;
    rig_open(&r, 32, 1, 0);
    CHECK_EQ(setup(&r), 0);
    r.sim.wear_limit = 6;
    int rc = ring(&r, 1000, 400, 400000);
    CHECK(rc == -3 || rc == -10); /* until the flash wears out, or time is up */
    alfs_status_t st;
    alfs_status(&r.fs, &st);
    CHECK(st.bad > 0);
    DONE(&r);
    rig_close(&r);
}

static void safe_read_retry(void) {
    rig_t r;
    rig_open(&r, 16, 1, 0);
    CHECK_EQ(setup(&r), 0);
    alfs_stream_t s;
    CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
    log_t lg;
    log_init(&lg, 1 << 16);
    CHECK_EQ(log_run(&r, &s, &lg, 50, 10000, false), 0);
    CHECK_EQ(CALL(&r, alfs_close(&r.fs, &s)), 0);
    uint32_t id = s.id;

    r.sim.read_fail_once = 1;
    CHECK_EQ(MOUNT(&r), ALFS_EIO);
    CHECK_EQ(MOUNT(&r), 0);
    r.sim.read_fail_once = 1;
    CHECK_EQ(CALL(&r, alfs_open_read(&r.fs, &s, id)), ALFS_EIO);
    CHECK_EQ(CALL(&r, alfs_open_read(&r.fs, &s, id)), 0);
    uint8_t *got = malloc(1 << 16);
    uint32_t n = 0;
    for (int i = 0; i < 100000; i++) {
        if (i % 3 == 1)
            r.sim.read_fail_once = 1;
        int32_t k = (int32_t)CALL(&r, alfs_read(&r.fs, &s, got + n, 100));
        if (k == ALFS_EIO)
            continue;
        CHECK(k >= 0);
        if (k == 0)
            break;
        n += (uint32_t)k;
    }
    CHECK_EQ(n, lg.len);
    CHECK(memcmp(got, lg.exp, n) == 0);
    free(got);
    free(lg.exp);
    DONE(&r);
    rig_close(&r);
}

static void safe_foreign_region(void) {
    rig_t r;
    rig_open(&r, 16, 1, 0);
    CHECK_EQ(MOUNT(&r), ALFS_ENOFS); /* random content */
    CHECK_EQ(r.sim.c_ops, 0);
    memset(r.sim.mem + r.cfg.region_start, 0xFF, r.cfg.region_size);
    CHECK_EQ(MOUNT(&r), ALFS_ENOFS); /* blank */
    CHECK_EQ(r.sim.c_ops, 0);
    CHECK_EQ(setup(&r), 0);
    alfs_config_t good = r.cfg;
    r.cfg.sector_size = 2 * SECTOR; /* formatted with 4 kB sectors */
    r.cfg.region_size = 8 * 2 * SECTOR;
    CHECK_EQ(MOUNT(&r), ALFS_EGEOMETRY);
    r.cfg = good;
    r.cfg.chip_size = r.cfg.region_start + r.cfg.region_size - SECTOR; /* the DD-098 case */
    CHECK_EQ(MOUNT(&r), ALFS_EGEOMETRY);
    CHECK_EQ(FORMAT(&r), ALFS_EGEOMETRY);
    CHECK_EQ(r.sim.c_ops, 0);
    DONE(&r);
    rig_close(&r);
}

static void build_image(rig_t *r) {
    rig_open(r, 16, 1, 0);
    if (setup(r)) {
        tf_failed = 1;
        return;
    }
    uint8_t row[ROW];
    uint32_t del = 0;
    for (uint32_t k = 0; k < 3; k++) {
        alfs_stream_t s;
        if (CALL(r, alfs_create(&r->fs, &s, r->pbuf[0], k)))
            tf_failed = 1;
        for (uint32_t i = 0; i < 300 * (k + 1); i++) {
            make_row(i, row);
            put(r, &s, row, ROW);
            if (i % 20 == 0)
                CALL(r, alfs_sync(&r->fs, &s));
        }
        CALL(r, alfs_close(&r->fs, &s));
        if (k == 1)
            del = s.id;
    }
    CALL(r, alfs_delete(&r->fs, del));
}

static void safe_fuzz_mount_read(void) {
    for (int iter = 0; iter < 400; iter++) {
        rig_t r;
        build_image(&r);
        r.sim.rng = 0x9E3779B9u * (uint32_t)(iter + 1);
        uint8_t *reg = r.sim.mem + r.cfg.region_start;
        switch (iter % 4) {
        case 0: /* bit flips anywhere */
            for (int i = 0; i < 1 + iter % 8; i++)
                reg[sim_rand(&r.sim) % r.cfg.region_size] ^= (uint8_t)(1u << (sim_rand(&r.sim) % 8));
            break;
        case 1: /* a sector's headers made random */
            sim_fill_random(&r.sim, r.cfg.region_start + (sim_rand(&r.sim) % 16) * SECTOR, 64);
            break;
        case 2: /* one sector's headers copied over another's */
            memcpy(reg + (sim_rand(&r.sim) % 16) * SECTOR, reg + (sim_rand(&r.sim) % 16) * SECTOR, 64);
            break;
        default: /* random bytes, keeping the first sector's stamp */
            sim_fill_random(&r.sim, r.cfg.region_start + 32, r.cfg.region_size - 32);
            break;
        }
        memset(r.sim.unit_done, 0, r.sim.chip_size / r.sim.prog_unit); /* bytes changed under it */
        if (MOUNT(&r) == 0) {
            alfs_info_t in;
            uint32_t after = 0;
            uint8_t *got = malloc(1 << 20);
            for (int guard = 0; guard < 64 && CALL(&r, alfs_next(&r.fs, after, &in)) == 0; guard++) {
                CHECK(in.id > after);
                after = in.id;
                slurp(&r, in.id, got, 1 << 20);
            }
            free(got);
        }
        rig_close(&r);
    }
}

static void safe_delete_atomic(void) {
    for (int pl = 0; pl < PL_COUNT; pl++) {
        rig_t r;
        rig_open(&r, 16, 1, 0);
        CHECK_EQ(setup(&r), 0);
        alfs_stream_t s;
        CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
        log_t lg;
        log_init(&lg, 1 << 16);
        CHECK_EQ(log_run(&r, &s, &lg, 50, 10000, false), 0);
        CHECK_EQ(CALL(&r, alfs_close(&r.fs, &s)), 0);
        sim_power_after(&r.sim, 0, (sim_pl_t)pl);
        CALL(&r, alfs_delete(&r.fs, s.id));
        CHECK(r.sim.off);
        sim_reboot(&r.sim);
        memset(&r.fs, 0, sizeof(r.fs));
        CHECK_EQ(MOUNT(&r), 0);
        uint8_t *got = malloc(1 << 16);
        int32_t n = slurp(&r, s.id, got, 1 << 16);
        CHECK(n == ALFS_ENOENT || (n == (int32_t)lg.len && memcmp(got, lg.exp, lg.len) == 0));
        free(got);
        free(lg.exp);
        DONE(&r);
        rig_close(&r);
    }
    /* A delete that returned is durable, and its sectors become dirty. */
    rig_t r;
    rig_open(&r, 16, 1, 0);
    CHECK_EQ(setup(&r), 0);
    alfs_stream_t s;
    CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
    log_t lg;
    log_init(&lg, 1 << 16);
    CHECK_EQ(log_run(&r, &s, &lg, 50, 10000, false), 0);
    CHECK_EQ(CALL(&r, alfs_close(&r.fs, &s)), 0);
    alfs_status_t st;
    alfs_status(&r.fs, &st);
    uint32_t used = 16u - st.ready;
    CHECK_EQ(CALL(&r, alfs_delete(&r.fs, s.id)), 0);
    alfs_status(&r.fs, &st);
    CHECK_EQ(st.dirty, used);
    CHECK_EQ(MOUNT(&r), 0);
    alfs_status(&r.fs, &st);
    CHECK_EQ(st.dirty, used);
    alfs_stream_t t;
    CHECK_EQ(CALL(&r, alfs_open_read(&r.fs, &t, s.id)), ALFS_ENOENT);
    free(lg.exp);
    DONE(&r);
    rig_close(&r);
}

/* ---- OBS -------------------------------------------------------------- */

static void obs_status(void) {
    rig_t r;
    rig_open(&r, 32, 1, 0);
    CHECK_EQ(setup(&r), 0);
    alfs_status_t st;
    alfs_status(&r.fs, &st);
    CHECK_EQ(st.sectors, 32);
    CHECK_EQ(st.ready, 32);
    CHECK_EQ(st.dirty, 0);
    CHECK_EQ(st.bad, 0);
    CHECK(st.appendable >= 32u * (SECTOR - 64u));
    CHECK_EQ(st.min_wear, 1);
    CHECK_EQ(st.max_wear, 1);
    alfs_stream_t s;
    CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
    log_t lg;
    log_init(&lg, 1 << 18);
    CHECK_EQ(log_run(&r, &s, &lg, 50, 60000, false), 0);
    CHECK_EQ(CALL(&r, alfs_close(&r.fs, &s)), 0);
    alfs_status(&r.fs, &st);
    CHECK_EQ(st.n.programs, r.sim.programs);
    CHECK_EQ(st.n.erases, r.sim.erases);
    CHECK(st.ready < 32);
    CHECK_EQ(st.dirty, 0);
    uint16_t ready = st.ready;
    CHECK_EQ(MOUNT(&r), 0);
    alfs_status(&r.fs, &st);
    CHECK_EQ(st.ready, ready);
    free(lg.exp);
    DONE(&r);
    rig_close(&r);
}

static void obs_clean(void) {
    rig_t r;
    rig_open(&r, 8, 1, 0);
    r.sim.busy_calls = 3;
    alfs_status_t st;
    CHECK_EQ(FORMAT(&r), ALFS_BUSY);
    alfs_status(&r.fs, &st);
    CHECK(!st.clean);
    while (ESTEP(&r) != ALFS_CLEAN) {
        alfs_status(&r.fs, &st);
        CHECK(!st.clean);
    }
    alfs_status(&r.fs, &st);
    CHECK(st.clean);
    alfs_stream_t s;
    CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
    uint8_t row[ROW] = {1};
    CHECK_EQ(put(&r, &s, row, ROW), 0);
    CHECK_EQ(CALL(&r, alfs_close(&r.fs, &s)), 0);
    CHECK_EQ(CALL(&r, alfs_delete(&r.fs, s.id)), 0);
    alfs_status(&r.fs, &st);
    CHECK(!st.clean);
    CHECK(clean(&r) >= 0);
    alfs_status(&r.fs, &st);
    CHECK(st.clean);
    DONE(&r);
    rig_close(&r);
}

/* ---- SIMP ------------------------------------------------------------- */

static void simp_ram(void) {
    /* The budgets are for a 32-bit target; a host's pointers are wider. */
    size_t wide = sizeof(void *) - 4;
    CHECK(sizeof(alfs_t) <= 64 + 2 * wide);
    CHECK(sizeof(alfs_stream_t) <= 32 + wide);
}

int main(int argc, char **argv) {
    static const tf_test_t tests[] = {
        {"if_geometry", if_geometry},
        {"if_endian", if_endian},
        {"if_list_streams", if_list_streams},
        {"basic_round_trip", basic_round_trip},
        {"spd_no_erase_outside_erase_step", spd_no_erase_outside_erase_step},
        {"spd_one_operation_per_call", spd_one_operation_per_call},
        {"spd_enospc_without_erase", spd_enospc_without_erase},
        {"spd_busy_erase_resumes", spd_busy_erase_resumes},
        {"spd_mount_reads_bounded", spd_mount_reads_bounded},
        {"spd_reopen_reads_bounded", spd_reopen_reads_bounded},
        {"spd_wlog_longest_stall", spd_wlog_longest_stall},
        {"spd_cpu_linear", spd_cpu_linear},
        {"life_erase_only_dirty", life_erase_only_dirty},
        {"life_write_amplification", life_write_amplification},
        {"life_wlog_erases", life_wlog_erases},
        {"life_wear_level", life_wear_level},
        {"life_erase_count_persists", life_erase_count_persists},
        {"life_partial_program_cap", life_partial_program_cap},
        {"life_large_erase", life_large_erase},
        {"safe_powerloss_durable", safe_powerloss_durable},
        {"safe_powerloss_bound", safe_powerloss_bound},
        {"safe_no_program_after_torn", safe_no_program_after_torn},
        {"safe_no_false_data", safe_no_false_data},
        {"safe_append_only_verified_ready", safe_append_only_verified_ready},
        {"safe_prog_failure", safe_prog_failure},
        {"safe_bad_sector", safe_bad_sector},
        {"safe_read_retry", safe_read_retry},
        {"safe_foreign_region", safe_foreign_region},
        {"safe_fuzz_mount_read", safe_fuzz_mount_read},
        {"safe_delete_atomic", safe_delete_atomic},
        {"obs_status", obs_status},
        {"obs_clean", obs_clean},
        {"simp_ram", simp_ram},
    };
    return tf_run(tests, (int)(sizeof(tests) / sizeof(tests[0])), argc > 1 ? argv[1] : NULL) ? 1 : 0;
}
