/*
 * alfs's tests, one or more for each requirement of docs/REQUIREMENTS.md.
 * Each is named in the requirement's "Verified by".
 *
 * Every alfs call a test makes goes through CALL, ERASE or FORMAT, which
 * hold it to SPD-01 (no erase outside an erase) and record the longest
 * single operation (SPD-08). Every test ends with DONE, which fails it if
 * the simulator saw a violation: a program across a page, a byte
 * programmed twice, or a program into a byte a power loss touched (each
 * but a program that settles, docs/TEST_PLAN.md 2). The simulator's seed
 * comes from ALFS_SEED.
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
#define C1 27u /* the claim's offset in a sector, prog unit 1 (docs/SPEC.md 1) */
#define D1 47u /* the first chunk's */

/* ---- the rig ---------------------------------------------------------- */

typedef struct {
    sim_t sim;
    alfs_driver_t drv;
    alfs_config_t cfg;
    alfs_t fs;
    uint8_t pbuf[4][ALFS_BUF_SIZE(PAGE)];
} rig_t;

static int call_bad;          /* calls that broke SPD-01 */
static char call_why[96];
static uint64_t longest_stall; /* the longest operation outside an erase, us */
static uint32_t seed;          /* the simulator's, from ALFS_SEED */

static void rig_open(rig_t *r, uint32_t sectors, uint32_t large) {
    uint32_t start = large ? large : SECTOR; /* the region does not start at 0 */
    uint32_t chip = start + sectors * SECTOR + (large ? large : SECTOR);
    sim_init(&r->sim, chip, SECTOR, PAGE, large, 0xFF, seed);
    sim_fill_random(&r->sim, 0, chip); /* what a used chip holds */
    r->drv = sim_driver(&r->sim);
    memset(&r->cfg, 0, sizeof(r->cfg));
    r->cfg.region_start = start;
    r->cfg.region_size = sectors * SECTOR;
    r->cfg.chip_size = chip;
    r->cfg.sector_size = SECTOR;
    r->cfg.page_size = PAGE;
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

static int64_t chk(rig_t *r, int64_t rc, bool erases, const char *what) {
    sim_t *s = &r->sim;
    uint64_t n = s->c_ops - s->c_programs;
    if (!erases && n > 0) {
        call_bad++;
        snprintf(call_why, sizeof(call_why), "%s: %llu erases", what, (unsigned long long)n);
    }
    if (!erases && s->c_max_op_us > longest_stall)
        longest_stall = s->c_max_op_us;
    return rc;
}

#define CALL(r, e) (sim_call_begin(&(r)->sim), chk((r), (int64_t)(e), false, #e))
#define ERASE(r) ((int)(sim_call_begin(&(r)->sim), chk((r), alfs_erase(&(r)->fs, 1), true, "erase")))
#define FORMAT(r)                                                                                  \
    ((int)(sim_call_begin(&(r)->sim),                                                              \
           chk((r), alfs_format(&(r)->fs, &(r)->cfg, &(r)->drv), true, "format")))
#define MOUNT(r) ((int)CALL((r), alfs_mount(&(r)->fs, &(r)->cfg, &(r)->drv)))

/* Erase steps until nothing is dirty. The number of steps, or -1. */
static int clean(rig_t *r) {
    for (int i = 0; i < 2000000; i++) {
        int rc = ERASE(r);
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
    return rc < 0 ? rc : 0;
}

static int setup(rig_t *r) {
    int rc = format_only(r);
    if (rc)
        return rc;
    return clean(r) < 0 ? -1 : 0;
}

/* Ends a test: no violation of the flash's rules or of SPD-01. */
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

static uint32_t get32le(const uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
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

/* Appends d, adding what alfs took to *taken. 0, or the error. */
static int put_n(rig_t *r, alfs_stream_t *s, const uint8_t *d, uint32_t n, uint32_t *taken) {
    while (n) {
        int32_t k = (int32_t)CALL(r, alfs_append(&r->fs, s, d, n));
        if (k <= 0)
            return k ? (int)k : -100;
        d += k;
        n -= (uint32_t)k;
        *taken += (uint32_t)k;
    }
    return 0;
}

static int put(rig_t *r, alfs_stream_t *s, const uint8_t *d, uint32_t n) {
    uint32_t taken = 0;
    return put_n(r, s, d, n, &taken);
}

static int sync_done(rig_t *r, alfs_stream_t *s) {
    return (int)CALL(r, alfs_sync(&r->fs, s));
}

static int close_done(rig_t *r, alfs_stream_t *s) {
    return (int)CALL(r, alfs_close(&r->fs, s));
}

/* Reads a whole stream. Its length, or an error. */
static int32_t slurp(rig_t *r, uint32_t id, uint8_t *out, uint32_t cap) {
    alfs_stream_t s;
    int rc = (int)CALL(r, alfs_open_read(&r->fs, &s, r->pbuf[3], id));
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
            int rc = ERASE(r);
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
            int rc = sync_done(r, s);
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

/* ---- the simulator ------------------------------------------------------ */

/* 4.4: a torn program moves only bits its data moves (A-9), into erased
 * bytes and over programmed ones, under every behaviour. */
static void sim_torn_obeys_a9(void) {
    for (int pl = 0; pl < PL_COUNT; pl++)
        for (int t = 0; t < 500; t++) {
            sim_t s;
            sim_init(&s, 4 * SECTOR, SECTOR, PAGE, 0, 0xFF, seed + (uint32_t)t);
            alfs_driver_t d = sim_driver(&s);
            uint8_t first[PAGE], data[PAGE], got[PAGE];
            uint32_t len = 1 + sim_rand(&s) % PAGE, addr = SECTOR + (sim_rand(&s) % 16) * PAGE;
            for (uint32_t i = 0; i < len; i++) {
                first[i] = (uint8_t)sim_rand(&s);
                data[i] = (uint8_t)sim_rand(&s);
            }
            bool over = t % 2; /* over bytes already programmed */
            if (over)
                CHECK_EQ(d.prog(d.ctx, addr, first, len), 0);
            sim_power_after(&s, 0, (sim_pl_t)pl);
            d.prog(d.ctx, addr, data, len);
            CHECK(s.off);
            sim_reboot(&s);
            for (int k = 0; k < 16; k++) {
                CHECK_EQ(d.read(d.ctx, addr, got, len), 0);
                for (uint32_t i = 0; i < len; i++) {
                    uint8_t had = over ? first[i] : 0xFF;
                    CHECK_EQ((uint8_t)(~got[i] & had & data[i]), 0); /* 1 before and in the data: 1 */
                    CHECK_EQ((uint8_t)(got[i] & ~had), 0);           /* programmed before: 0 */
                }
            }
            sim_free(&s);
        }
}

/* ---- IF ---------------------------------------------------------------- */

static void if_geometry(void) {
    rig_t r;
    rig_open(&r, 8, 0);
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
    CHECK_EQ(MOUNT(&r), 0);
    r.cfg.region_size = 6 * SECTOR;
    CHECK_EQ(MOUNT(&r), ALFS_EGEOMETRY);
    DONE(&r);
    rig_close(&r);
}

/* The stamp's fields, little-endian, where docs/SPEC.md puts them. */
static void if_endian(void) {
    rig_t r;
    rig_open(&r, 8, 0);
    CHECK_EQ(setup(&r), 0);
    const uint8_t *st = r.sim.mem + r.cfg.region_start;
    CHECK(memcmp(st, "alfs", 4) == 0);
    CHECK_EQ(st[4], 2);  /* version            */
    CHECK_EQ(st[5], 12); /* log2(sector size)  */
    CHECK_EQ(st[6], 8);  /* log2(page size)    */
    CHECK_EQ(st[7], 0);  /* log2(prog unit)    */
    CHECK_EQ(st[8] | st[9] << 8, 8);
    CHECK_EQ(st[10] | st[11] << 8 | st[12] << 16 | (uint32_t)st[13] << 24, 1); /* generation */
    CHECK_EQ(st[14] | st[15] << 8 | st[16] << 16 | (uint32_t)st[17] << 24, 1); /* erase count */
    CHECK(get32le(st + 18) >= 1);                                               /* sequence floor */
    CHECK_EQ(get32le(st + 22), crc32_ref(st, 22));

    /* A claim and its first chunk. */
    alfs_stream_t s;
    CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 0x11223344u)), 0);
    const uint8_t data[3] = {7, 8, 9};
    CHECK_EQ(put(&r, &s, data, 3), 0);
    CHECK_EQ(sync_done(&r, &s), 0);
    const uint8_t *sec = r.sim.mem + r.cfg.region_start + s.sector * SECTOR;
    const uint8_t *cl = sec + C1;
    CHECK_EQ(get32le(cl), s.id);           /* identity                */
    CHECK_EQ(get32le(cl + 4), s.id);       /* sequence: a first claim */
    CHECK_EQ(get32le(cl + 8), 0x11223344u); /* tag                     */
    CHECK_EQ(cl[12] | cl[13] << 8, s.sector); /* first sector          */
    CHECK_EQ(cl[14] | cl[15] << 8, 0);     /* sector number           */
    CHECK_EQ(get32le(cl + 16), crc32_ref(cl, 16));
    CHECK_EQ(sec[C1 - 1], 0xFF);           /* the dead mark, erased   */
    const uint8_t *ch = sec + D1;
    CHECK_EQ(ch[0] | ch[1] << 8, 3);
    uint8_t body[5] = {ch[0], ch[1], 7, 8, 9};
    CHECK_EQ(get32le(ch + 2), crc32_ref(body, 5));
    CHECK(memcmp(ch + 6, data, 3) == 0);
    DONE(&r);
    rig_close(&r);
}

static void if_list_streams(void) {
    rig_t r;
    rig_open(&r, 16, 0);
    CHECK_EQ(setup(&r), 0);
    uint32_t ids[5];
    uint8_t row[ROW];
    for (uint32_t i = 0; i < 5; i++) {
        alfs_stream_t s;
        CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 100 + i)), 0);
        make_row(i, row);
        CHECK_EQ(put(&r, &s, row, ROW), 0);
        CHECK_EQ(close_done(&r, &s), 0);
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
    static const uint32_t sizes[] = {1, 7, 250, 251, 256, 4000, 20000};
    {
        for (unsigned z = 0; z < 7; z++) {
            rig_t r;
            rig_open(&r, 16, 0);
            CHECK_EQ(setup(&r), 0);
            uint8_t *d = malloc(sizes[z]), *o = malloc(sizes[z] + 1024);
            for (uint32_t i = 0; i < sizes[z]; i++)
                d[i] = (uint8_t)(i * 7 + z);
            alfs_stream_t s;
            CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
            CHECK_EQ(put(&r, &s, d, sizes[z]), 0);
            CHECK_EQ(close_done(&r, &s), 0);
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

/* W-LOG at both rates: every call held to SPD-01 by CALL. */
static void spd_no_erase_outside_erase(void) {
    static const uint32_t rates[] = {1, 50};
    for (unsigned k = 0; k < 2; k++) {
            rig_t r;
            rig_open(&r, 64, 0);
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

static void spd_enospc_without_erase(void) {
    rig_t r;
    rig_open(&r, 6, 0);
    CHECK_EQ(format_only(&r), 0); /* one sector ready, five dirty */
    CHECK_EQ(ERASE(&r), ALFS_OK); /* two ready */
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
    CHECK_EQ(sync_done(&r, &s), ALFS_ENOSPC);
    CHECK_EQ(r.sim.c_ops, 0);
    /* One erase step makes room; the bytes held are written. */
    CHECK_EQ(ERASE(&r), ALFS_OK);
    CHECK_EQ(sync_done(&r, &s), 0);
    CHECK_EQ(close_done(&r, &s), 0);
    CHECK_EQ(slurp(&r, s.id, o, 6 * SECTOR), (int32_t)n);
    CHECK(memcmp(o, d, n) == 0);
    free(d);
    free(o);
    DONE(&r);
    rig_close(&r);
}

static void spd_mount_reads_bounded(void) {
    rig_t r;
    rig_open(&r, 128, 0);
    CHECK_EQ(setup(&r), 0);
    log_t lg;
    log_init(&lg, 1 << 20);
    for (int k = 0; k < 4; k++) {
        alfs_stream_t s;
        CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], (uint32_t)k)), 0);
        lg.len = 0;
        CHECK_EQ(log_run(&r, &s, &lg, 50, 30000, false), 0);
        CHECK_EQ(close_done(&r, &s), 0);
    }
    CHECK_EQ(MOUNT(&r), 0);
    CHECK_EQ(r.sim.c_ops, 0);
    CHECK(r.sim.c_bytes_read <= 68u * 128u);
    free(lg.exp);
    DONE(&r);
    rig_close(&r);
}

static void spd_wlog_longest_stall(void) {
    {
        rig_t r;
        rig_open(&r, 256, 0);
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

/* Bytes read reading a stream back whole. */
static void read_cost(rig_t *r, uint32_t id, uint32_t want, uint64_t *cost) {
    uint8_t *got = malloc(want + 1024);
    uint64_t b0 = r->sim.bytes_read;
    int32_t n = slurp(r, id, got, want + 1024);
    *cost = r->sim.bytes_read - b0;
    free(got);
    CHECK_EQ(n, (int32_t)want);
}

/* SPD-10: reading on to the next sector reads the headers between, not the
 * region's. Two streams written together cost one header a sector more to
 * read than one written alone. */
static void spd_read_next_bounded(void) {
    enum { ROUNDS = 200, CHUNK = 3900 };
    uint8_t *d = malloc(CHUNK);
    for (uint32_t i = 0; i < CHUNK; i++)
        d[i] = (uint8_t)(i * 7u + 1u);
    rig_t r;
    rig_open(&r, 512, 0);
    CHECK_EQ(setup(&r), 0);
    alfs_stream_t a, b;
    CHECK_EQ(CALL(&r, alfs_create(&r.fs, &a, r.pbuf[0], 1)), 0);
    CHECK_EQ(CALL(&r, alfs_create(&r.fs, &b, r.pbuf[1], 2)), 0);
    for (int i = 0; i < ROUNDS; i++) {
        CHECK_EQ(put(&r, &a, d, CHUNK), 0);
        CHECK_EQ(sync_done(&r, &a), 0);
        CHECK_EQ(put(&r, &b, d, CHUNK), 0);
        CHECK_EQ(sync_done(&r, &b), 0);
    }
    uint64_t pair = 0, alone = 0;
    read_cost(&r, a.id, ROUNDS * CHUNK, &pair);
    DONE(&r);
    rig_close(&r);

    rig_t q;
    rig_open(&q, 512, 0);
    CHECK_EQ(setup(&q), 0);
    alfs_stream_t c;
    CHECK_EQ(CALL(&q, alfs_create(&q.fs, &c, q.pbuf[0], 3)), 0);
    for (int i = 0; i < ROUNDS; i++) {
        CHECK_EQ(put(&q, &c, d, CHUNK), 0);
        CHECK_EQ(sync_done(&q, &c), 0);
    }
    read_cost(&q, c.id, ROUNDS * CHUNK, &alone);
    printf("    reading %u sectors: %llu bytes beside another stream, %llu alone\n", ROUNDS,
           (unsigned long long)pair, (unsigned long long)alone);
    CHECK(pair <= alone + 2u * D1 * (ROUNDS + 10u));
    free(d);
    DONE(&q);
    rig_close(&q);
}

/* Reads per append and sync do not grow with the region. */
static void spd_cpu_linear(void) {
    uint64_t reads[2];
    double secs[2];
    static const uint32_t sizes[] = {32, 2048};
    for (int k = 0; k < 2; k++) {
        rig_t r;
        rig_open(&r, sizes[k], 0);
        CHECK_EQ(setup(&r), 0);
        alfs_stream_t s;
        CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
        uint8_t blk[64];
        memset(blk, 0xA5, sizeof(blk));
        uint64_t before = r.sim.bytes_read;
        clock_t t0 = clock();
        for (int i = 0; i < 1000; i++) { /* 64 kB: 16 sectors */
            CHECK_EQ(put(&r, &s, blk, sizeof(blk)), 0);
            CHECK_EQ(sync_done(&r, &s), 0);
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
        int rc = ERASE(r);
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
        if (tick % 50 == 0 && sync_done(r, &s) < 0)
            return -5;
        if (rows >= every) {
            if (close_done(r, &s))
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
    rig_open(&r, 32, 0);
    CHECK_EQ(setup(&r), 0);
    CHECK_EQ(ring(&r, 3, 800, 5000000), 0);
    CHECK_EQ(r.sim.ready_erases, 0);
    DONE(&r);
    rig_close(&r);
}

static void life_write_amplification(void) {
    static const uint32_t rates[] = {1, 50};
    for (unsigned k = 0; k < 2; k++) {
            rig_t r;
            rig_open(&r, 256, 0);
            CHECK_EQ(setup(&r), 0);
            uint64_t p0 = r.sim.programs, b0 = r.sim.bytes_programmed;
            alfs_stream_t s;
            CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
            log_t lg;
            log_init(&lg, 1 << 20);
            CHECK_EQ(log_run(&r, &s, &lg, rates[k], 600000, false), 0);
            CHECK_EQ(close_done(&r, &s), 0);
            uint64_t progs = r.sim.programs - p0, bytes = r.sim.bytes_programmed - b0;
            alfs_status_t st;
            alfs_status(&r.fs, &st);
            uint64_t used = 256 - st.ready;
            uint64_t bound = lg.len + progs * 13 + used * 64;
            if (bytes > bound)
                printf("    rate %u: %llu programmed, bound %llu\n", rates[k], (unsigned long long)bytes,
                       (unsigned long long)bound);
            CHECK(bytes <= bound);
            free(lg.exp);
            DONE(&r);
            rig_close(&r);
        }
}

static void life_wlog_erases(void) {
    rig_t r;
    rig_open(&r, 256, 0);
    CHECK_EQ(setup(&r), 0);
    uint64_t b0 = r.sim.bytes_programmed;
    alfs_stream_t s;
    CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
    log_t lg;
    log_init(&lg, 1 << 20);
    CHECK_EQ(log_run(&r, &s, &lg, 50, 600000, false), 0);
    CHECK_EQ(close_done(&r, &s), 0);
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
    rig_open(&r, 32, 0);
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
    rig_open(&r, 32, 0);
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
        rig_open(&r, 16, 0);
        r.cfg.max_page_programs = caps[c];
        CHECK_EQ(setup(&r), 0);
        alfs_stream_t s;
        CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
        uint8_t row[ROW];
        for (uint32_t i = 0; i < 1000; i++) {
            make_row(i, row);
            CHECK_EQ(put(&r, &s, row, 3), 0); /* tiny syncs: many per page */
            CHECK_EQ(sync_done(&r, &s), 0);
        }
        CHECK(r.sim.max_page_progs <= (caps[c] ? caps[c] : 16u));
        DONE(&r);
        rig_close(&r);
    }
}

static void life_large_erase(void) {
    rig_t r;
    rig_open(&r, 64, LARGE); /* four aligned blocks */
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
 * What a file may read after a power loss (SAFE-01): a prefix of what was
 * appended, at least what was synced. Bytes never synced may be lost, or
 * come back at a later mount in their place.
 */
typedef struct {
    const uint8_t *exp;
    uint32_t synced, len;
} expect_t;

static bool reads_as_expected(const uint8_t *got, int32_t n, const expect_t *e) {
    if (n < 0 && e->synced == 0)
        return true; /* nothing synced: the file may be gone */
    return n >= (int32_t)e->synced && n <= (int32_t)e->len && !memcmp(got, e->exp, (size_t)n);
}

/* Remounts, erases until clean, and reads both files: twice. */
static void reread(rig_t *r, uint32_t old_id, const expect_t *old, uint32_t new_id, const expect_t *now,
                   uint8_t *got, const char *what) {
    for (int round = 0; round < 2; round++) {
        memset(&r->fs, 0, sizeof(r->fs));
        CHECK_EQ(MOUNT(r), 0);
        CHECK(clean(r) >= 0);
        for (int f = 0; f < 2; f++) {
            const expect_t *e = f ? now : old;
            int32_t n = slurp(r, f ? new_id : old_id, got, 64 * 1024);
            bool ok = reads_as_expected(got, n, e);
            if (!ok)
                printf("    %s, round %d, %s file: read %d of %u (synced %u)\n", what, round,
                       f ? "new" : "old", n, e->len, e->synced);
            CHECK(ok);
        }
    }
}

/*
 * Power lost at operation k of a logging run that also erases, with each
 * behaviour; then a remount and a read, a new file logged (some cases lose
 * power a second time there), and remounts with erases and reads of both.
 * Sets *past when k is past the run's last operation.
 */
static void powerloss_case(sim_pl_t pl, int64_t k, bool twice, int *past) {
    rig_t r;
    rig_open(&r, 10, 0);
    *past = 1;
    CHECK_EQ(format_only(&r), 0);
    for (int i = 0; i < 3; i++)
        CHECK(ERASE(&r) >= 0);
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
    uint32_t old_id = s.id;
    expect_t old = {malloc(lg.len + 1), lg.synced, lg.len};
    memcpy((uint8_t *)old.exp, lg.exp, lg.len);
    sim_reboot(&r.sim);
    memset(&r.fs, 0, sizeof(r.fs));
    CHECK_EQ(MOUNT(&r), 0);

    uint8_t *got = malloc(64 * 1024);
    char what[64];
    snprintf(what, sizeof(what), "pl %d k %lld", pl, (long long)k);
    int32_t n = slurp(&r, old_id, got, 64 * 1024);
    if (!reads_as_expected(got, n, &old))
        printf("    %s: read %d, synced %u, appended %u\n", what, n, old.synced, old.len);
    CHECK(reads_as_expected(got, n, &old));

    /* A new file after the restart. */
    CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 78)), 0);
    lg.len = lg.synced = 0;
    if (twice)
        sim_power_after(&r.sim, (int64_t)(sim_rand(&r.sim) % 40), pl);
    rc = log_run(&r, &s, &lg, 50, 6000, true);
    if (!r.sim.off) {
        CHECK_EQ(rc, 0);
        CHECK_EQ(close_done(&r, &s), 0);
        lg.synced = lg.len;
    } else {
        sim_reboot(&r.sim);
    }
    expect_t now = {lg.exp, lg.synced, lg.len};
    reread(&r, old_id, &old, s.id, &now, got, what);
    free(got);
    free((uint8_t *)old.exp);
    free(lg.exp);
    DONE(&r);
    rig_close(&r);
}

static void powerloss_all(bool twice) {
    for (int pl = 0; pl < PL_COUNT; pl++)
        for (int64_t k = 0;; k++) {
            int past = 0;
            powerloss_case((sim_pl_t)pl, k, twice && k % 3 == 0, &past);
            if (past && !tf_failed)
                break;
            if (tf_failed) {
                printf("    behaviour %d, power lost at operation %lld\n", pl, (long long)k);
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
        rig_open(&r, 16, 0);
        CHECK_EQ(setup(&r), 0);
        alfs_stream_t s;
        CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
        log_t lg;
        log_init(&lg, 1 << 16);
        CHECK_EQ(log_run(&r, &s, &lg, 50, 20000, false), 0);
        CHECK_EQ(close_done(&r, &s), 0);
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

static void safe_write_only_verified_ready(void) {
    /* An ERASENOOP sector is found out on the ground and never written. */
    rig_t r;
    rig_open(&r, 8, 0);
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
        rig_open(&r, 16, 0);
        CHECK_EQ(setup(&r), 0);
        alfs_stream_t s;
        CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
        log_t lg;
        log_init(&lg, 1 << 16);
        CHECK_EQ(log_run(&r, &s, &lg, 50, 3000, false), 0);
        /* The sector being written goes bad. */
        sim_mkbad(&r.sim, r.cfg.region_start / SECTOR + s.sector, modes[m]);
        for (int t = 0; t < 40; t++) {
            uint8_t row[ROW];
            make_row(lg.seq, row);
            uint32_t off = 0;
            int32_t k = (int32_t)CALL(&r, alfs_append(&r.fs, &s, row + off, ROW - off));
            CHECK_EQ(k, (int32_t)ROW); /* a failed program goes to a fresh sector in the call */
            memcpy(lg.exp + lg.len, row, ROW);
            lg.len += ROW;
            lg.seq++;
            CHECK_EQ(sync_done(&r, &s), 0);
        }
        CHECK_EQ(close_done(&r, &s), 0);
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
    rig_open(&r, 8, 0);
    CHECK_EQ(setup(&r), 0);
    for (uint32_t k = 1; k <= 8; k++)
        sim_mkbad(&r.sim, k, BAD_PROGERROR);
    alfs_stream_t s;
    CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
    uint8_t row[ROW] = {0};
    CHECK_EQ(put(&r, &s, row, ROW), 0);
    CHECK_EQ(sync_done(&r, &s), ALFS_ELOST);
    alfs_status_t st;
    alfs_status(&r.fs, &st);
    CHECK_EQ(st.n.bytes_lost, ROW);
    CHECK_EQ(st.n.prog_failures, 3);
    DONE(&r);
    rig_close(&r);
}

static void safe_bad_sector(void) {
    static const sim_bad_t modes[] = {BAD_ERASEERROR, BAD_ERASENOOP};
    for (unsigned m = 0; m < 2; m++) {
        rig_t r;
        rig_open(&r, 8, 0);
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
    rig_open(&r, 32, 0);
    CHECK_EQ(setup(&r), 0);
    r.sim.wear_limit = 6;
    int rc = ring(&r, 1000, 400, 400000);
    CHECK(rc == -3 || rc == -10 || rc == 0); /* worn out: no room, time up, or all bad */
    alfs_status_t st;
    alfs_status(&r.fs, &st);
    CHECK(st.bad > 0);
    DONE(&r);
    rig_close(&r);
}

static void safe_read_retry(void) {
    rig_t r;
    rig_open(&r, 16, 0);
    CHECK_EQ(setup(&r), 0);
    alfs_stream_t s;
    CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
    log_t lg;
    log_init(&lg, 1 << 16);
    CHECK_EQ(log_run(&r, &s, &lg, 50, 10000, false), 0);
    CHECK_EQ(close_done(&r, &s), 0);
    uint32_t id = s.id;

    r.sim.read_fail_once = 1;
    CHECK_EQ(MOUNT(&r), ALFS_EIO);
    CHECK_EQ(MOUNT(&r), 0);
    r.sim.read_fail_once = 1;
    CHECK_EQ(CALL(&r, alfs_open_read(&r.fs, &s, r.pbuf[1], id)), ALFS_EIO);
    CHECK_EQ(CALL(&r, alfs_open_read(&r.fs, &s, r.pbuf[1], id)), 0);
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
    rig_open(&r, 16, 0);
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
    rig_open(r, 16, 0);
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
                sync_done(r, &s);
        }
        close_done(r, &s);
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
        memset(r.sim.done, 0, r.sim.chip_size); /* bytes changed under it */
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
        rig_open(&r, 16, 0);
        CHECK_EQ(setup(&r), 0);
        alfs_stream_t s;
        CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
        log_t lg;
        log_init(&lg, 1 << 16);
        CHECK_EQ(log_run(&r, &s, &lg, 50, 10000, false), 0);
        CHECK_EQ(close_done(&r, &s), 0);
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
    rig_open(&r, 16, 0);
    CHECK_EQ(setup(&r), 0);
    alfs_stream_t s;
    CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
    log_t lg;
    log_init(&lg, 1 << 16);
    CHECK_EQ(log_run(&r, &s, &lg, 50, 10000, false), 0);
    CHECK_EQ(close_done(&r, &s), 0);
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
    CHECK_EQ(CALL(&r, alfs_open_read(&r.fs, &t, r.pbuf[1], s.id)), ALFS_ENOENT);
    free(lg.exp);
    DONE(&r);
    rig_close(&r);
}

/* The first claim of stream s's sector numbered n, or NULL. */
static uint8_t *claim_of(rig_t *r, uint32_t id, uint32_t n) {
    for (uint32_t k = 0; k < r->cfg.region_size / SECTOR; k++) {
        uint8_t *cl = r->sim.mem + r->cfg.region_start + k * SECTOR + C1;
        if (get32le(cl) == id && (uint32_t)(cl[14] | cl[15] << 8) == n && get32le(cl + 16) == crc32_ref(cl, 16))
            return cl;
    }
    return NULL;
}

/* SAFE-11: a delete a power loss left half programmed, read as deleted,
 * then power lost at each operation of its reclaim: whole or gone. */
static void safe_delete_reclaim(void) {
    for (int64_t k = 0;; k++) {
        rig_t r;
        rig_open(&r, 16, 0);
        CHECK_EQ(setup(&r), 0);
        alfs_stream_t s;
        CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
        log_t lg;
        log_init(&lg, 1 << 16);
        CHECK_EQ(log_run(&r, &s, &lg, 50, 10000, false), 0);
        CHECK_EQ(close_done(&r, &s), 0);
        sim_power_after(&r.sim, 0, PL_FLICKER);
        CALL(&r, alfs_delete(&r.fs, s.id));
        CHECK(r.sim.off);
        sim_reboot(&r.sim);
        alfs_info_t in;
        bool deleted = false;
        for (int t = 0; t < 64 && !deleted; t++) {
            memset(&r.fs, 0, sizeof(r.fs));
            CHECK_EQ(MOUNT(&r), 0);
            deleted = CALL(&r, alfs_next(&r.fs, 0, &in)) == ALFS_ENOENT;
        }
        CHECK(deleted);
        sim_power_after(&r.sim, k, PL_ATOMIC);
        clean(&r);
        bool past = !r.sim.off;
        sim_reboot(&r.sim);
        uint8_t *got = malloc(1 << 16);
        for (int t = 0; t < 8; t++) {
            memset(&r.fs, 0, sizeof(r.fs));
            CHECK_EQ(MOUNT(&r), 0);
            int32_t n = slurp(&r, s.id, got, 1 << 16);
            bool ok = n == ALFS_ENOENT || (n == (int32_t)lg.len && memcmp(got, lg.exp, lg.len) == 0);
            if (!ok)
                printf("    power lost at reclaim operation %lld: read %d of %u\n", (long long)k, n, lg.len);
            CHECK(ok);
        }
        free(got);
        free(lg.exp);
        DONE(&r);
        rig_close(&r);
        if (past || tf_failed)
            break;
    }
}

/* SAFE-12: a sector lost from the middle of a stream is counted, and the
 * rest reads in order. */
static void safe_missing_sector(void) {
    rig_t r;
    rig_open(&r, 16, 0);
    CHECK_EQ(setup(&r), 0);
    alfs_stream_t s;
    CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
    uint32_t total = 5u * 3900u;
    uint8_t *exp = malloc(total);
    for (uint32_t i = 0; i < total; i++)
        exp[i] = (uint8_t)(i * 5u + 3u);
    for (uint32_t n = 0; n < total; n += 3900) {
        CHECK_EQ(put(&r, &s, exp + n, 3900), 0);
        CHECK_EQ(sync_done(&r, &s), 0);
    }
    CHECK_EQ(close_done(&r, &s), 0);
    uint8_t *cl = claim_of(&r, s.id, 2);
    CHECK(cl != NULL);
    if (cl)
        sim_flip(&r.sim, (uint32_t)(cl + 16 - r.sim.mem), 0x01);
    alfs_status_t st;
    alfs_status(&r.fs, &st);
    uint32_t before = st.n.check_failures;
    uint8_t *got = malloc(total + 1024);
    int32_t n = slurp(&r, s.id, got, total + 1024);
    CHECK(n > 0 && n < (int32_t)total);
    uint32_t a = 0;
    while (a < (uint32_t)n && got[a] == exp[a])
        a++;
    CHECK(n > 0 && memcmp(got + a, exp + total - ((uint32_t)n - a), (uint32_t)n - a) == 0);
    alfs_status(&r.fs, &st);
    CHECK_EQ(st.n.check_failures - before, 1);
    free(got);
    free(exp);
    DONE(&r);
    rig_close(&r);
}

/* IF-04: a first claim torn by a power loss never shares its identity with
 * a stream made after it. */
static void if_identity_never_reused(void) {
    static const sim_pl_t pls[] = {PL_FLICKER, PL_METASTABLE, PL_SOMEBITS};
    for (unsigned b = 0; b < 3; b++) {
        rig_t r;
        rig_open(&r, 16, 0);
        CHECK_EQ(setup(&r), 0);
        alfs_stream_t a;
        CHECK_EQ(CALL(&r, alfs_create(&r.fs, &a, r.pbuf[0], 1)), 0);
        uint8_t row[ROW];
        make_row(1, row);
        CHECK_EQ(put(&r, &a, row, ROW), 0);
        sim_power_after(&r.sim, 0, pls[b]);
        CALL(&r, alfs_sync(&r.fs, &a)); /* its first claim, torn */
        CHECK(r.sim.off);
        sim_reboot(&r.sim);
        memset(&r.fs, 0, sizeof(r.fs));
        CHECK_EQ(MOUNT(&r), 0);
        alfs_stream_t c;
        CHECK_EQ(CALL(&r, alfs_create(&r.fs, &c, r.pbuf[1], 2)), 0);
        uint8_t rows[3 * ROW];
        for (int i = 0; i < 3; i++)
            make_row(10u + (uint32_t)i, rows + i * ROW);
        CHECK_EQ(put(&r, &c, rows, sizeof(rows)), 0);
        CHECK_EQ(close_done(&r, &c), 0);
        CHECK(c.id != a.id);
        for (int t = 0; t < 8; t++) {
            memset(&r.fs, 0, sizeof(r.fs));
            CHECK_EQ(MOUNT(&r), 0);
            uint32_t seen[8], nseen = 0, after = 0;
            alfs_info_t in;
            while (nseen < 8 && CALL(&r, alfs_next(&r.fs, after, &in)) == 0) {
                for (uint32_t i = 0; i < nseen; i++)
                    CHECK(seen[i] != in.id);
                seen[nseen++] = in.id;
                after = in.id;
            }
            uint8_t got[1024];
            CHECK_EQ(slurp(&r, c.id, got, sizeof(got)), (int32_t)sizeof(rows));
            CHECK(memcmp(got, rows, sizeof(rows)) == 0);
        }
        DONE(&r);
        rig_close(&r);
    }
}

/* ---- OBS -------------------------------------------------------------- */

static void obs_status(void) {
    rig_t r;
    rig_open(&r, 32, 0);
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
    CHECK_EQ(close_done(&r, &s), 0);
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
    rig_open(&r, 8, 0);
    alfs_status_t st;
    CHECK_EQ(FORMAT(&r), 0);
    alfs_status(&r.fs, &st);
    CHECK(!st.clean);
    CHECK(clean(&r) >= 0);
    alfs_status(&r.fs, &st);
    CHECK(st.clean);
    alfs_stream_t s;
    CHECK_EQ(CALL(&r, alfs_create(&r.fs, &s, r.pbuf[0], 1)), 0);
    uint8_t row[ROW] = {1};
    CHECK_EQ(put(&r, &s, row, ROW), 0);
    CHECK_EQ(close_done(&r, &s), 0);
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
    CHECK(sizeof(alfs_t) <= 96 + 2 * wide);
    CHECK(sizeof(alfs_stream_t) <= 32 + 2 * wide); /* a wider pointer, and its alignment */
}

int main(int argc, char **argv) {
    static const tf_test_t tests[] = {
        {"sim_torn_obeys_a9", sim_torn_obeys_a9},
        {"if_geometry", if_geometry},
        {"if_endian", if_endian},
        {"if_list_streams", if_list_streams},
        {"if_identity_never_reused", if_identity_never_reused},
        {"basic_round_trip", basic_round_trip},
        {"spd_no_erase_outside_erase", spd_no_erase_outside_erase},
        {"spd_enospc_without_erase", spd_enospc_without_erase},
        {"spd_mount_reads_bounded", spd_mount_reads_bounded},
        {"spd_wlog_longest_stall", spd_wlog_longest_stall},
        {"spd_cpu_linear", spd_cpu_linear},
        {"spd_read_next_bounded", spd_read_next_bounded},
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
        {"safe_write_only_verified_ready", safe_write_only_verified_ready},
        {"safe_prog_failure", safe_prog_failure},
        {"safe_bad_sector", safe_bad_sector},
        {"safe_read_retry", safe_read_retry},
        {"safe_foreign_region", safe_foreign_region},
        {"safe_fuzz_mount_read", safe_fuzz_mount_read},
        {"safe_delete_atomic", safe_delete_atomic},
        {"safe_delete_reclaim", safe_delete_reclaim},
        {"safe_missing_sector", safe_missing_sector},
        {"obs_status", obs_status},
        {"obs_clean", obs_clean},
        {"simp_ram", simp_ram},
    };
    const char *env = getenv("ALFS_SEED");
    seed = env ? (uint32_t)strtoul(env, NULL, 0) : 0;
    int failures = tf_run(tests, (int)(sizeof(tests) / sizeof(tests[0])), argc > 1 ? argv[1] : NULL);
    if (failures)
        printf("seed %u\n", seed);
    return failures ? 1 : 0;
}
