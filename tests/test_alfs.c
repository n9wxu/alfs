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
#define BLOCK_MAX 4096u
#define C1 27u     /* the claim's offset in a sector (docs/SPEC.md 1) */
#define D1 43u     /* the first chunk's, in a sector not a file's first */
#define NAME1 43u  /* a first sector's name */
#define CLOSE1 79u /* its close record */

/* ---- the rig ---------------------------------------------------------- */

typedef struct {
    sim_t sim;
    alfs_driver_t drv;
    alfs_config_t cfg;
    alfs_t fs;
    uint32_t bsize; /* the blocks', a page unless a test says */
    uint8_t scratch[PAGE];
    uint8_t blk[4][BLOCK_MAX];
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
    r->bsize = PAGE;
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
           chk((r), alfs_format(&(r)->fs, &(r)->cfg, &(r)->drv, (r)->scratch), true, "format")))
#define MOUNT(r) ((int)CALL((r), alfs_mount(&(r)->fs, &(r)->cfg, &(r)->drv, (r)->scratch)))

/* A reboot: every handle gone, the region mounted again. */
static int remount(rig_t *r) {
    memset(&r->fs, 0, sizeof(r->fs));
    return MOUNT(r);
}

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

static int create(rig_t *r, alfs_file_t *f, const char *name, int b) {
    return (int)CALL(r, alfs_create(&r->fs, f, name, r->blk[b], r->bsize));
}

/* Writes d, adding what alfs took to *taken. 0, or the error. */
static int put_n(rig_t *r, alfs_file_t *f, const uint8_t *d, uint32_t n, uint32_t *taken) {
    while (n) {
        int32_t k = (int32_t)CALL(r, alfs_write(&r->fs, f, d, n));
        if (k <= 0)
            return k ? (int)k : -100;
        d += k;
        n -= (uint32_t)k;
        *taken += (uint32_t)k;
    }
    return 0;
}

static int put(rig_t *r, alfs_file_t *f, const uint8_t *d, uint32_t n) {
    uint32_t taken = 0;
    return put_n(r, f, d, n, &taken);
}

static int sync_done(rig_t *r, alfs_file_t *f) {
    return (int)CALL(r, alfs_sync(&r->fs, f));
}

static int close_done(rig_t *r, alfs_file_t *f) {
    return (int)CALL(r, alfs_close(&r->fs, f));
}

/* Reads a whole file, by name and version (0: the newest closed). Its
 * length, or an error. */
static int32_t slurp(rig_t *r, const char *name, uint32_t version, uint8_t *out, uint32_t cap) {
    alfs_file_t f;
    int rc = (int)CALL(r, alfs_open_read(&r->fs, &f, name, version, r->blk[3], r->bsize));
    if (rc)
        return rc;
    uint32_t n = 0;
    int32_t result = -201;
    for (int guard = 0; guard < 1000000; guard++) {
        uint32_t want = cap - n < 300 ? cap - n : 300;
        int32_t k = (int32_t)CALL(r, alfs_read(&r->fs, &f, out + n, want));
        if (k <= 0) {
            result = k < 0 ? k : (int32_t)n;
            break;
        }
        n += (uint32_t)k;
        if (n >= cap) {
            result = -200; /* more than was ever written */
            break;
        }
    }
    CALL(r, alfs_close(&r->fs, &f));
    return result;
}

/* The listing entry for version v, or ALFS_ENOENT. */
static int info_of(rig_t *r, uint32_t v, alfs_info_t *in) {
    int rc = (int)CALL(r, alfs_list(&r->fs, v - 1, in));
    return rc ? rc : in->version == v ? ALFS_OK : ALFS_ENOENT;
}

/*
 * A logging run: rows produced at `rate` a second, written every 200 ms,
 * synced every second, `ms` long, with an erase step every 20 ms if asked.
 * Rows go to exp[]; synced is the length a successful sync covered. Stops
 * early when the power is lost. 0, or the first error that is not a power
 * loss.
 */
typedef struct {
    uint8_t *exp;
    uint32_t len, cap;
    uint32_t synced;
    uint32_t seq;
} log_t;

static int log_run(rig_t *r, alfs_file_t *f, log_t *lg, uint32_t rate, uint32_t ms, bool steps) {
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
            int rc = put_n(r, f, lg->exp + lg->len, n * ROW, &taken);
            lg->len += taken;
            if (r->sim.off)
                return 0;
            if (rc)
                return rc;
        }
        if (t % 1000 == 0) {
            int rc = sync_done(r, f);
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
        {"page under 128 bytes", &r.cfg.page_size, 64},
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

/* The stamp's, a first sector's and a chunk's bytes, little-endian, where
 * docs/SPEC.md puts them. */
static void if_endian(void) {
    rig_t r;
    rig_open(&r, 8, 0);
    CHECK_EQ(setup(&r), 0);
    const uint8_t *st = r.sim.mem + r.cfg.region_start;
    CHECK(memcmp(st, "alfs", 4) == 0);
    CHECK_EQ(st[4], 3);  /* version            */
    CHECK_EQ(st[5], 12); /* log2(sector size)  */
    CHECK_EQ(st[6], 8);  /* log2(page size)    */
    CHECK_EQ(st[7], 0);
    CHECK_EQ(st[8] | st[9] << 8, 8);
    CHECK_EQ(get32le(st + 10), 1); /* generation */
    CHECK_EQ(get32le(st + 14), 1); /* erase count */
    CHECK(get32le(st + 18) >= 1);  /* sequence floor */
    CHECK_EQ(get32le(st + 22), crc32_ref(st, 22));

    alfs_file_t f;
    CHECK_EQ(create(&r, &f, "ab", 0), 0);
    const uint8_t data[3] = {7, 8, 9};
    CHECK_EQ(put(&r, &f, data, 3), 0);
    CHECK_EQ(close_done(&r, &f), 0);
    const uint8_t *sec = r.sim.mem + r.cfg.region_start + f.first * SECTOR;
    const uint8_t *cl = sec + C1;
    CHECK_EQ(get32le(cl), f.version);     /* identity                */
    CHECK_EQ(get32le(cl + 4), f.version); /* sequence: a first claim */
    CHECK_EQ(cl[8] | cl[9] << 8, f.first); /* first sector           */
    CHECK_EQ(cl[10] | cl[11] << 8, 0);    /* sector number           */
    CHECK_EQ(get32le(cl + 12), crc32_ref(cl, 12));
    CHECK_EQ(sec[C1 - 1], 0xFF); /* the dead mark, erased */
    const uint8_t *nm = sec + NAME1;
    CHECK(memcmp(nm, "ab", 3) == 0);
    for (int i = 3; i < 32; i++)
        CHECK_EQ(nm[i], 0);
    uint8_t chk[36];
    memcpy(chk, cl, 4);
    memcpy(chk + 4, nm, 32);
    CHECK_EQ(get32le(nm + 32), crc32_ref(chk, 36)); /* the name check */
    const uint8_t *cr = sec + CLOSE1;
    CHECK_EQ(get32le(cr), 3); /* the size */
    memcpy(chk, cl, 4);
    memcpy(chk + 4, cr, 4);
    CHECK_EQ(get32le(cr + 4), crc32_ref(chk, 8));
    const uint8_t *ch = sec + PAGE; /* a first sector's chunks start at its second page */
    CHECK_EQ(ch[0] | ch[1] << 8, 3);
    uint8_t body[5] = {ch[0], ch[1], 7, 8, 9};
    CHECK_EQ(get32le(ch + 2), crc32_ref(body, 5));
    CHECK(memcmp(ch + 6, data, 3) == 0);
    DONE(&r);
    rig_close(&r);
}

static void if_names(void) {
    static const char *const good[] = {"a", "0123456789012345678901234567890", "logs/flight.bin"};
    rig_t r;
    rig_open(&r, 16, 0);
    CHECK_EQ(setup(&r), 0);
    uint32_t ver[3];
    for (int i = 0; i < 3; i++) {
        alfs_file_t f;
        CHECK_EQ(create(&r, &f, good[i], 0), 0);
        uint8_t b = (uint8_t)i;
        CHECK_EQ(put(&r, &f, &b, 1), 0);
        CHECK_EQ(close_done(&r, &f), 0);
        ver[i] = f.version;
    }
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < 3; i++) {
            uint8_t got[8];
            CHECK_EQ(slurp(&r, good[i], 0, got, sizeof(got)), 1);
            CHECK_EQ(got[0], i);
            alfs_info_t in;
            CHECK_EQ(info_of(&r, ver[i], &in), 0);
            CHECK(strcmp(in.name, good[i]) == 0);
        }
        CHECK_EQ(remount(&r), 0);
    }
    static const char *const bad[] = {"", "01234567890123456789012345678901"};
    for (int i = 0; i < 2; i++) {
        alfs_file_t f;
        CHECK_EQ(create(&r, &f, bad[i], 0), ALFS_EINVAL);
        CHECK_EQ(r.sim.c_ops, 0);
    }
    alfs_file_t f;
    CHECK_EQ(create(&r, &f, NULL, 0), ALFS_EINVAL);
    uint8_t got[8];
    CHECK_EQ(slurp(&r, "logs", 0, got, sizeof(got)), ALFS_ENOENT); /* a prefix is not the name */
    CHECK_EQ(slurp(&r, "logs/flight.bin2", 0, got, sizeof(got)), ALFS_ENOENT);
    CHECK_EQ(slurp(&r, "a", ver[2], got, sizeof(got)), ALFS_ENOENT); /* that version is another's */
    DONE(&r);
    rig_close(&r);
}

static void if_list_files(void) {
    rig_t r;
    rig_open(&r, 16, 0);
    CHECK_EQ(setup(&r), 0);
    static const char *const names[] = {"a", "b", "a"};
    uint32_t ver[4];
    uint8_t row[ROW];
    for (int i = 0; i < 3; i++) {
        alfs_file_t f;
        CHECK_EQ(create(&r, &f, names[i], 0), 0);
        make_row((uint32_t)i, row);
        CHECK_EQ(put(&r, &f, row, ROW), 0);
        CHECK_EQ(close_done(&r, &f), 0);
        ver[i] = f.version;
        if (i)
            CHECK(ver[i] > ver[i - 1]);
    }
    alfs_file_t c; /* written and synced, never closed */
    CHECK_EQ(create(&r, &c, "c", 1), 0);
    CHECK_EQ(put(&r, &c, row, 5), 0);
    CHECK_EQ(sync_done(&r, &c), 0);
    ver[3] = c.version;
    CHECK_EQ(CALL(&r, alfs_delete(&r.fs, "b", ver[1])), 0);
    for (int pass = 0; pass < 2; pass++) {
        static const int want[] = {0, 2, 3};
        alfs_info_t in;
        uint32_t after = 0, seen = 0;
        while (CALL(&r, alfs_list(&r.fs, after, &in)) == 0) {
            CHECK(seen < 3);
            int w = want[seen];
            CHECK_EQ(in.version, ver[w]);
            CHECK(strcmp(in.name, w == 3 ? "c" : "a") == 0);
            CHECK_EQ(in.closed, w != 3);
            CHECK_EQ(in.size, w == 3 ? ALFS_SIZE_UNKNOWN : ROW);
            after = in.version;
            seen++;
        }
        CHECK_EQ(seen, 3);
        CHECK_EQ(remount(&r), 0); /* the second pass after a reboot */
    }
    DONE(&r);
    rig_close(&r);
}

static void if_open_version0(void) {
    rig_t r;
    rig_open(&r, 16, 0);
    CHECK_EQ(setup(&r), 0);
    uint32_t ver[3];
    alfs_file_t f[3];
    for (int i = 0; i < 3; i++) {
        uint8_t b[2] = {(uint8_t)i, 0x5A};
        CHECK_EQ(create(&r, &f[i], "cfg", i), 0);
        CHECK_EQ(put(&r, &f[i], b, 2), 0);
        CHECK_EQ(i < 2 ? close_done(&r, &f[i]) : sync_done(&r, &f[i]), 0);
        ver[i] = f[i].version;
    }
    uint8_t got[8];
    CHECK_EQ(slurp(&r, "cfg", 0, got, sizeof(got)), 2);
    CHECK_EQ(got[0], 1); /* the newest closed */
    CHECK_EQ(slurp(&r, "cfg", ver[2], got, sizeof(got)), 2);
    CHECK_EQ(got[0], 2); /* the unclosed one, by its version */
    CHECK_EQ(slurp(&r, NULL, ver[0], got, sizeof(got)), 2);
    CHECK_EQ(got[0], 0);
    CHECK_EQ(close_done(&r, &f[2]), 0);
    CHECK_EQ(slurp(&r, "cfg", 0, got, sizeof(got)), 2);
    CHECK_EQ(got[0], 2);
    DONE(&r);
    rig_close(&r);
}

static void if_closed_read_only(void) {
    rig_t r;
    rig_open(&r, 16, 0);
    CHECK_EQ(setup(&r), 0);
    alfs_file_t f, g;
    uint8_t b[4] = {1, 2, 3, 4};
    CHECK_EQ(create(&r, &f, "x", 0), 0);
    CHECK_EQ(put(&r, &f, b, 4), 0);
    CHECK_EQ(close_done(&r, &f), 0);
    CHECK_EQ(CALL(&r, alfs_write(&r.fs, &f, b, 4)), ALFS_EINVAL);
    CHECK_EQ(CALL(&r, alfs_sync(&r.fs, &f)), ALFS_EINVAL);
    CHECK_EQ(CALL(&r, alfs_open_read(&r.fs, &g, "x", 0, r.blk[1], r.bsize)), 0);
    CHECK_EQ(CALL(&r, alfs_write(&r.fs, &g, b, 4)), ALFS_EINVAL);
    CHECK_EQ(CALL(&r, alfs_sync(&r.fs, &g)), ALFS_EINVAL);
    CHECK_EQ(close_done(&r, &g), 0);
    uint8_t got[8];
    CHECK_EQ(slurp(&r, "x", 0, got, sizeof(got)), 4);
    DONE(&r);
    rig_close(&r);
}

static void if_delete_open_refused(void) {
    rig_t r;
    rig_open(&r, 16, 0);
    CHECK_EQ(setup(&r), 0);
    alfs_file_t a, rd, b;
    uint8_t x[4] = {9, 9, 9, 9};
    CHECK_EQ(create(&r, &a, "a", 0), 0);
    CHECK_EQ(put(&r, &a, x, 4), 0);
    CHECK_EQ(close_done(&r, &a), 0);
    CHECK_EQ(CALL(&r, alfs_open_read(&r.fs, &rd, "a", 0, r.blk[1], r.bsize)), 0);
    CHECK_EQ(CALL(&r, alfs_delete(&r.fs, "a", a.version)), ALFS_EOPEN);
    CHECK_EQ(create(&r, &b, "b", 2), 0);
    CHECK_EQ(CALL(&r, alfs_delete(&r.fs, "b", b.version)), ALFS_EOPEN);
    CHECK_EQ(MOUNT(&r), ALFS_EOPEN);
    CHECK_EQ(FORMAT(&r), ALFS_EOPEN);
    CHECK_EQ(r.sim.c_ops, 0);
    CHECK_EQ(close_done(&r, &rd), 0);
    CHECK_EQ(close_done(&r, &b), 0);
    CHECK_EQ(CALL(&r, alfs_delete(&r.fs, "a", a.version)), 0);
    CHECK_EQ(CALL(&r, alfs_delete(&r.fs, "b", b.version)), 0);
    CHECK_EQ(MOUNT(&r), 0);
    DONE(&r);
    rig_close(&r);
}

static void if_empty_file(void) {
    rig_t r;
    rig_open(&r, 16, 0);
    CHECK_EQ(setup(&r), 0);
    alfs_file_t f;
    CHECK_EQ(create(&r, &f, "empty", 0), 0);
    CHECK_EQ(close_done(&r, &f), 0);
    for (int pass = 0; pass < 2; pass++) {
        alfs_info_t in;
        CHECK_EQ(info_of(&r, f.version, &in), 0);
        CHECK(strcmp(in.name, "empty") == 0);
        CHECK(in.closed);
        CHECK_EQ(in.size, 0);
        uint8_t got[8];
        CHECK_EQ(slurp(&r, "empty", 0, got, sizeof(got)), 0);
        CHECK_EQ(remount(&r), 0);
    }
    DONE(&r);
    rig_close(&r);
}

/* IF-07: versions grow and are never used again, after a delete or a torn
 * create. */
static void if_version_never_reused(void) {
    rig_t r;
    rig_open(&r, 16, 0);
    CHECK_EQ(setup(&r), 0);
    alfs_file_t f;
    uint32_t v[4];
    static const char *const names[] = {"a", "b", "a"};
    for (int i = 0; i < 3; i++) {
        CHECK_EQ(create(&r, &f, names[i], 0), 0);
        CHECK_EQ(close_done(&r, &f), 0);
        v[i] = f.version;
        if (i)
            CHECK(v[i] > v[i - 1]);
    }
    CHECK_EQ(CALL(&r, alfs_delete(&r.fs, "a", v[2])), 0);
    CHECK_EQ(remount(&r), 0);
    CHECK_EQ(create(&r, &f, "a", 0), 0);
    CHECK_EQ(close_done(&r, &f), 0);
    v[3] = f.version;
    CHECK(v[3] > v[2]);
    DONE(&r);
    rig_close(&r);

    static const sim_pl_t pls[] = {PL_FLICKER, PL_METASTABLE, PL_SOMEBITS};
    for (unsigned b = 0; b < 3; b++) {
        rig_open(&r, 16, 0);
        CHECK_EQ(setup(&r), 0);
        sim_power_after(&r.sim, 0, pls[b]);
        create(&r, &f, "torn", 0); /* its program, torn */
        CHECK(r.sim.off);
        sim_reboot(&r.sim);
        CHECK_EQ(remount(&r), 0);
        alfs_file_t c;
        CHECK_EQ(create(&r, &c, "z", 1), 0);
        uint8_t rows[3 * ROW];
        for (int i = 0; i < 3; i++)
            make_row(10u + (uint32_t)i, rows + i * ROW);
        CHECK_EQ(put(&r, &c, rows, sizeof(rows)), 0);
        CHECK_EQ(close_done(&r, &c), 0);
        CHECK(c.version != f.version);
        for (int t = 0; t < 8; t++) {
            CHECK_EQ(remount(&r), 0);
            uint32_t seen[8], nseen = 0, after = 0;
            alfs_info_t in;
            while (nseen < 8 && CALL(&r, alfs_list(&r.fs, after, &in)) == 0) {
                for (uint32_t i = 0; i < nseen; i++)
                    CHECK(seen[i] != in.version);
                seen[nseen++] = in.version;
                after = in.version;
            }
            uint8_t got[1024];
            CHECK_EQ(slurp(&r, "z", 0, got, sizeof(got)), (int32_t)sizeof(rows));
            CHECK(memcmp(got, rows, sizeof(rows)) == 0);
        }
        DONE(&r);
        rig_close(&r);
    }
}

/* ---- basics ---------------------------------------------------------- */

static void basic_round_trip(void) {
    static const uint32_t sizes[] = {1, 7, 250, 251, 256, 4000, 20000};
    static const uint32_t blocks[] = {PAGE, BLOCK_MAX};
    for (unsigned b = 0; b < 2; b++)
        for (unsigned z = 0; z < 7; z++) {
            rig_t r;
            rig_open(&r, 16, 0);
            r.bsize = blocks[b];
            CHECK_EQ(setup(&r), 0);
            uint8_t *d = malloc(sizes[z]), *o = malloc(sizes[z] + 1024);
            for (uint32_t i = 0; i < sizes[z]; i++)
                d[i] = (uint8_t)(i * 7 + z);
            alfs_file_t f;
            CHECK_EQ(create(&r, &f, "round", 0), 0);
            CHECK_EQ(put(&r, &f, d, sizes[z]), 0);
            CHECK_EQ(close_done(&r, &f), 0);
            CHECK_EQ(slurp(&r, "round", 0, o, sizes[z] + 1024), (int32_t)sizes[z]);
            CHECK(memcmp(o, d, sizes[z]) == 0);
            CHECK_EQ(remount(&r), 0);
            CHECK_EQ(slurp(&r, "round", 0, o, sizes[z] + 1024), (int32_t)sizes[z]);
            CHECK(memcmp(o, d, sizes[z]) == 0);
            free(d);
            free(o);
            DONE(&r);
            rig_close(&r);
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
        alfs_file_t f;
        CHECK_EQ(create(&r, &f, "log", 0), 0);
        log_t lg;
        log_init(&lg, 1 << 20);
        CHECK_EQ(log_run(&r, &f, &lg, rates[k], 120000, true), 0);
        free(lg.exp);
        DONE(&r);
        rig_close(&r);
    }
}

/* SPD-02: the programs a call made, for the X bytes of payload it
 * programmed: a chunk per page's room, one more for a part page at each
 * end, one for each sector entered, one for the fresh sector. */
static bool within_burst(rig_t *r, uint32_t x) {
    uint64_t bound = (x + PAGE - 7u) / (PAGE - 6u) + 2u + (x / (SECTOR - PAGE) + 1u) + 1u;
    if (r->sim.c_programs > bound)
        printf("    %llu programs for %u bytes, bound %llu\n", (unsigned long long)r->sim.c_programs, x,
               (unsigned long long)bound);
    return r->sim.c_programs <= bound;
}

static void spd_write_burst_bounded(void) {
    static const uint32_t blocks[] = {PAGE, BLOCK_MAX};
    static const uint32_t pieces[] = {480, ROW};
    for (unsigned b = 0; b < 2; b++)
        for (unsigned w = 0; w < 2; w++) {
            rig_t r;
            rig_open(&r, 256, 0);
            r.bsize = blocks[b];
            CHECK_EQ(setup(&r), 0);
            alfs_file_t f;
            CHECK_EQ(create(&r, &f, "hr", 0), 0);
            uint8_t d[480];
            for (uint32_t t = 1; t <= 30 * 50; t++) { /* 30 s at 20 ms */
                for (uint32_t i = 0; i < sizeof(d); i++)
                    d[i] = (uint8_t)(t + i);
                uint32_t u0 = f.used;
                int32_t k = (int32_t)CALL(&r, alfs_write(&r.fs, &f, d, pieces[w]));
                CHECK_EQ(k, (int32_t)pieces[w]);
                CHECK(within_burst(&r, u0 + (uint32_t)k - f.used));
                if (t % 50 == 0) {
                    u0 = f.used;
                    CHECK_EQ(sync_done(&r, &f), 0);
                    CHECK(within_burst(&r, u0));
                }
            }
            uint32_t u0 = f.used;
            CHECK_EQ(close_done(&r, &f), 0);
            CHECK(within_burst(&r, u0));
            DONE(&r);
            rig_close(&r);
        }
}

static void spd_enospc_without_erase(void) {
    rig_t r;
    rig_open(&r, 6, 0);
    CHECK_EQ(format_only(&r), 0); /* one sector ready, five dirty */
    CHECK_EQ(ERASE(&r), ALFS_OK); /* two ready */
    alfs_file_t f;
    CHECK_EQ(create(&r, &f, "fill", 0), 0);
    uint8_t *d = malloc(6 * SECTOR), *o = malloc(6 * SECTOR);
    for (uint32_t i = 0; i < 6 * SECTOR; i++)
        d[i] = (uint8_t)(i ^ (i >> 8));
    uint32_t n = 0;
    int rc = 0;
    while (n < 6 * SECTOR) {
        int32_t k = (int32_t)CALL(&r, alfs_write(&r.fs, &f, d + n, 6 * SECTOR - n));
        if (k < 0) {
            rc = (int)k;
            break;
        }
        n += (uint32_t)k;
    }
    CHECK_EQ(rc, ALFS_ENOSPC);
    CHECK_EQ(r.sim.c_ops, 0);
    CHECK_EQ(sync_done(&r, &f), ALFS_ENOSPC);
    CHECK_EQ(r.sim.c_ops, 0);
    /* One erase step makes room; the bytes held are written. */
    CHECK_EQ(ERASE(&r), ALFS_OK);
    CHECK_EQ(sync_done(&r, &f), 0);
    CHECK_EQ(close_done(&r, &f), 0);
    CHECK_EQ(slurp(&r, "fill", 0, o, 6 * SECTOR), (int32_t)n);
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
        alfs_file_t f;
        CHECK_EQ(create(&r, &f, "log", 0), 0);
        lg.len = 0;
        CHECK_EQ(log_run(&r, &f, &lg, 50, 30000, false), 0);
        CHECK_EQ(close_done(&r, &f), 0);
    }
    CHECK_EQ(MOUNT(&r), 0);
    CHECK_EQ(r.sim.c_ops, 0);
    CHECK(r.sim.c_bytes_read <= 60u * 128u);
    free(lg.exp);
    DONE(&r);
    rig_close(&r);
}

static void spd_wlog_longest_stall(void) {
    rig_t r;
    rig_open(&r, 256, 0);
    CHECK_EQ(setup(&r), 0);
    alfs_file_t f;
    CHECK_EQ(create(&r, &f, "log", 0), 0);
    log_t lg;
    log_init(&lg, 1 << 20);
    CHECK_EQ(log_run(&r, &f, &lg, 50, 600000, false), 0);
    CHECK(longest_stall <= SIM_T_PROG_US);
    free(lg.exp);
    DONE(&r);
    rig_close(&r);
}

/* Bytes read reading a file back whole. */
static void read_cost(rig_t *r, uint32_t version, uint32_t want, uint64_t *cost) {
    uint8_t *got = malloc(want + 1024);
    uint64_t b0 = r->sim.bytes_read;
    int32_t n = slurp(r, NULL, version, got, want + 1024);
    *cost = r->sim.bytes_read - b0;
    free(got);
    CHECK_EQ(n, (int32_t)want);
}

/* SPD-10: reading on to the next sector reads the headers between, not the
 * region's. Two files written together cost one header a sector more to
 * read than one written alone. */
static void spd_read_next_bounded(void) {
    enum { ROUNDS = 200, CHUNK = 3900 };
    uint8_t *d = malloc(CHUNK);
    for (uint32_t i = 0; i < CHUNK; i++)
        d[i] = (uint8_t)(i * 7u + 1u);
    rig_t r;
    rig_open(&r, 512, 0);
    CHECK_EQ(setup(&r), 0);
    alfs_file_t a, b;
    CHECK_EQ(create(&r, &a, "a", 0), 0);
    CHECK_EQ(create(&r, &b, "b", 1), 0);
    for (int i = 0; i < ROUNDS; i++) {
        CHECK_EQ(put(&r, &a, d, CHUNK), 0);
        CHECK_EQ(sync_done(&r, &a), 0);
        CHECK_EQ(put(&r, &b, d, CHUNK), 0);
        CHECK_EQ(sync_done(&r, &b), 0);
    }
    uint64_t pair = 0, alone = 0;
    read_cost(&r, a.version, ROUNDS * CHUNK, &pair);
    DONE(&r);
    rig_close(&r);

    rig_t q;
    rig_open(&q, 512, 0);
    CHECK_EQ(setup(&q), 0);
    alfs_file_t c;
    CHECK_EQ(create(&q, &c, "c", 0), 0);
    for (int i = 0; i < ROUNDS; i++) {
        CHECK_EQ(put(&q, &c, d, CHUNK), 0);
        CHECK_EQ(sync_done(&q, &c), 0);
    }
    read_cost(&q, c.version, ROUNDS * CHUNK, &alone);
    printf("    reading %u sectors: %llu bytes beside another file, %llu alone\n", ROUNDS,
           (unsigned long long)pair, (unsigned long long)alone);
    CHECK(pair <= alone + 2u * D1 * (ROUNDS + 10u));
    free(d);
    DONE(&q);
    rig_close(&q);
}

/* Reads per write and sync do not grow with the region. */
static void spd_cpu_linear(void) {
    uint64_t reads[2];
    double secs[2];
    static const uint32_t sizes[] = {32, 2048};
    for (int k = 0; k < 2; k++) {
        rig_t r;
        rig_open(&r, sizes[k], 0);
        CHECK_EQ(setup(&r), 0);
        alfs_file_t f;
        CHECK_EQ(create(&r, &f, "log", 0), 0);
        uint8_t blk[64];
        memset(blk, 0xA5, sizeof(blk));
        uint64_t before = r.sim.bytes_read;
        clock_t t0 = clock();
        for (int i = 0; i < 1000; i++) { /* 64 kB: 16 sectors */
            CHECK_EQ(put(&r, &f, blk, sizeof(blk)), 0);
            CHECK_EQ(sync_done(&r, &f), 0);
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
 * W-RING on a small region: W-LOG at 50 rows a second, a new version of
 * "log" every `every` rows, the oldest deleted while fewer than two logs'
 * worth of sectors are ready, an erase step every 20 ms. Runs until every
 * sector has been erased `cycles` times.
 */
static int ring(rig_t *r, uint32_t cycles, uint32_t every, uint32_t max_ticks) {
    uint32_t vers[64], nv = 0, rows = 0, seq = 0;
    alfs_file_t f;
    uint8_t row[ROW];
    if (create(r, &f, "log", 0))
        return -1;
    vers[nv++] = f.version;
    uint32_t sectors = r->cfg.region_size / SECTOR;
    uint32_t need = (every * ROW) / (SECTOR - 64) + 2;
    for (uint32_t tick = 1; tick <= max_ticks; tick++) {
        int rc = ERASE(r);
        if (rc < 0 && rc != ALFS_EIO)
            return -2;
        if (tick % 10 == 0) { /* 200 ms: 10 rows */
            for (int i = 0; i < 10; i++) {
                make_row(seq++, row);
                rc = put(r, &f, row, ROW);
                if (rc == ALFS_ENOSPC)
                    return -3;
                if (rc && rc != ALFS_EPROG && rc != ALFS_ELOST)
                    return -4;
                rows++;
            }
        }
        if (tick % 50 == 0 && sync_done(r, &f) < 0)
            return -5;
        if (rows >= every) {
            if (close_done(r, &f))
                return -6;
            if (create(r, &f, "log", 0))
                return -7;
            if (nv == 64)
                return -8;
            vers[nv++] = f.version;
            rows = 0;
        }
        alfs_status_t st;
        alfs_status(&r->fs, &st);
        while (st.ready < 2 * need && nv > 1) {
            if (CALL(r, alfs_delete(&r->fs, "log", vers[0])))
                return -9;
            memmove(vers, vers + 1, --nv * sizeof(vers[0]));
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
    static const uint32_t rates[] = {1, 50, 0}; /* 0: W-HR */
    for (unsigned k = 0; k < 3; k++) {
        rig_t r;
        rig_open(&r, 256, 0);
        CHECK_EQ(setup(&r), 0);
        uint64_t p0 = r.sim.programs, b0 = r.sim.bytes_programmed;
        alfs_file_t f;
        CHECK_EQ(create(&r, &f, "log", 0), 0);
        log_t lg;
        log_init(&lg, 1 << 22);
        if (rates[k]) {
            CHECK_EQ(log_run(&r, &f, &lg, rates[k], 600000, false), 0);
        } else {
            for (uint32_t t = 1; t <= 30 * 50; t++) { /* 30 s of W-HR */
                for (uint32_t i = 0; i < 480; i++)
                    lg.exp[lg.len + i] = (uint8_t)(t + i);
                CHECK_EQ(put(&r, &f, lg.exp + lg.len, 480), 0);
                lg.len += 480;
                if (t % 50 == 0)
                    CHECK_EQ(sync_done(&r, &f), 0);
            }
        }
        CHECK_EQ(close_done(&r, &f), 0);
        uint64_t progs = r.sim.programs - p0, bytes = r.sim.bytes_programmed - b0;
        alfs_status_t st;
        alfs_status(&r.fs, &st);
        uint64_t used = 256 - st.ready;
        uint64_t bound = lg.len + progs * 6 + used * 72 + 128;
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
    alfs_file_t f;
    CHECK_EQ(create(&r, &f, "log", 0), 0);
    log_t lg;
    log_init(&lg, 1 << 20);
    CHECK_EQ(log_run(&r, &f, &lg, 50, 600000, false), 0);
    CHECK_EQ(close_done(&r, &f), 0);
    uint64_t filled = (r.sim.bytes_programmed - b0 + SECTOR - 1) / SECTOR;
    uint64_t e0 = r.sim.erases;
    CHECK_EQ(CALL(&r, alfs_delete(&r.fs, "log", f.version)), 0);
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
    CHECK_EQ(remount(&r), 0);
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
        alfs_file_t f;
        CHECK_EQ(create(&r, &f, "log", 0), 0);
        uint8_t row[ROW];
        for (uint32_t i = 0; i < 1000; i++) {
            make_row(i, row);
            CHECK_EQ(put(&r, &f, row, 3), 0); /* tiny syncs: many per page */
            CHECK_EQ(sync_done(&r, &f), 0);
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
 * written, at least what was synced. Bytes never synced may be lost, or
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
static void reread(rig_t *r, uint32_t old_v, const expect_t *old, uint32_t new_v, const expect_t *now,
                   uint8_t *got, const char *what) {
    for (int round = 0; round < 2; round++) {
        CHECK_EQ(remount(r), 0);
        CHECK(clean(r) >= 0);
        for (int f = 0; f < 2; f++) {
            const expect_t *e = f ? now : old;
            int32_t n = slurp(r, NULL, f ? new_v : old_v, got, 64 * 1024);
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
 * behaviour; then a remount and a read, a new version logged (some cases
 * lose power a second time there), and remounts with erases and reads of
 * both. Sets *past when k is past the run's last operation.
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
    alfs_file_t f;
    CHECK_EQ(create(&r, &f, "log", 0), 0);
    sim_power_after(&r.sim, k, pl);
    int rc = log_run(&r, &f, &lg, 50, 20000, true);
    if (!r.sim.off) {
        free(lg.exp);
        rig_close(&r);
        *past = 1;
        CHECK_EQ(rc, 0);
        return;
    }
    uint32_t old_v = f.version;
    expect_t old = {malloc(lg.len + 1), lg.synced, lg.len};
    memcpy((uint8_t *)old.exp, lg.exp, lg.len);
    sim_reboot(&r.sim);
    CHECK_EQ(remount(&r), 0);

    uint8_t *got = malloc(64 * 1024);
    char what[64];
    snprintf(what, sizeof(what), "pl %d k %lld", pl, (long long)k);
    int32_t n = slurp(&r, NULL, old_v, got, 64 * 1024);
    if (!reads_as_expected(got, n, &old))
        printf("    %s: read %d, synced %u, written %u\n", what, n, old.synced, old.len);
    CHECK(reads_as_expected(got, n, &old));

    /* A new version after the restart. */
    lg.len = lg.synced = 0;
    if (twice)
        sim_power_after(&r.sim, (int64_t)(sim_rand(&r.sim) % 40), pl);
    f.version = 0;
    rc = create(&r, &f, "log", 0);
    if (!rc)
        rc = log_run(&r, &f, &lg, 50, 6000, true);
    if (!r.sim.off) {
        CHECK_EQ(rc, 0);
        CHECK_EQ(close_done(&r, &f), 0);
        lg.synced = lg.len;
    } else {
        sim_reboot(&r.sim);
    }
    expect_t now = {lg.exp, lg.synced, lg.len};
    reread(&r, old_v, &old, f.version, &now, got, what);
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
        alfs_file_t f;
        CHECK_EQ(create(&r, &f, "log", 0), 0);
        log_t lg;
        log_init(&lg, 1 << 16);
        CHECK_EQ(log_run(&r, &f, &lg, 50, 20000, false), 0);
        CHECK_EQ(close_done(&r, &f), 0);
        /* Flip one bit where chunks are: past the first sector's first page,
         * past the second's headers. */
        uint32_t from = trial % 2 ? 64u : PAGE;
        uint32_t sector = r.fs.cfg->region_start + (uint32_t)(trial % 2) * SECTOR;
        uint32_t at = sector + from + sim_rand(&r.sim) % (SECTOR - from);
        sim_flip(&r.sim, at, (uint8_t)(1u << (trial % 8)));
        uint8_t *got = malloc(1 << 16);
        int32_t n = slurp(&r, NULL, f.version, got, 1 << 16);
        CHECK(n >= 0);
        /* What was read is the file with at most one run cut out: the rest
         * of the damaged page. No damaged byte comes back. */
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
    alfs_file_t f;
    CHECK_EQ(create(&r, &f, "log", 0), 0);
    log_t lg;
    log_init(&lg, 1 << 16);
    int rc = log_run(&r, &f, &lg, 50, 40000, false);
    CHECK(rc == 0 || rc == ALFS_ENOSPC);
    CHECK_EQ(r.sim.sector_progs_total[4], before);
    free(lg.exp);
    DONE(&r);
    rig_close(&r);
    /* Power lost in erases is covered by every power-loss case. */
}

static void safe_prog_failure(void) {
    static const sim_bad_t modes[] = {BAD_PROGERROR, BAD_PROGNOOP, BAD_PROGFLIP};
    for (unsigned m = 0; m < 3; m++) {
        rig_t r;
        rig_open(&r, 16, 0);
        CHECK_EQ(setup(&r), 0);
        alfs_file_t f;
        CHECK_EQ(create(&r, &f, "log", 0), 0);
        log_t lg;
        log_init(&lg, 1 << 16);
        CHECK_EQ(log_run(&r, &f, &lg, 50, 3000, false), 0);
        /* The sector being written goes bad. */
        sim_mkbad(&r.sim, r.cfg.region_start / SECTOR + f.sector, modes[m]);
        for (int t = 0; t < 40; t++) {
            uint8_t row[ROW];
            make_row(lg.seq, row);
            int32_t k = (int32_t)CALL(&r, alfs_write(&r.fs, &f, row, ROW));
            CHECK_EQ(k, (int32_t)ROW); /* a failed program goes to a fresh sector in the call */
            memcpy(lg.exp + lg.len, row, ROW);
            lg.len += ROW;
            lg.seq++;
            CHECK_EQ(sync_done(&r, &f), 0);
        }
        CHECK_EQ(close_done(&r, &f), 0);
        uint8_t *got = malloc(1 << 16);
        int32_t n = slurp(&r, NULL, f.version, got, 1 << 16);
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
    alfs_file_t f;
    CHECK_EQ(create(&r, &f, "log", 0), 0);
    for (uint32_t k = 1; k <= 8; k++)
        sim_mkbad(&r.sim, k, BAD_PROGERROR);
    uint8_t row[ROW] = {0};
    CHECK_EQ(put(&r, &f, row, ROW), 0);
    CHECK_EQ(sync_done(&r, &f), ALFS_ELOST);
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
        /* After a remount it is dirty again; its next erase finds it bad
         * again, and nothing is written to it. */
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
    alfs_file_t f;
    CHECK_EQ(create(&r, &f, "log", 0), 0);
    log_t lg;
    log_init(&lg, 1 << 16);
    CHECK_EQ(log_run(&r, &f, &lg, 50, 10000, false), 0);
    CHECK_EQ(close_done(&r, &f), 0);

    r.sim.read_fail_once = 1;
    CHECK_EQ(MOUNT(&r), ALFS_EIO);
    CHECK_EQ(MOUNT(&r), 0);
    r.sim.read_fail_once = 1;
    CHECK_EQ(CALL(&r, alfs_open_read(&r.fs, &f, "log", 0, r.blk[1], r.bsize)), ALFS_EIO);
    CHECK_EQ(CALL(&r, alfs_open_read(&r.fs, &f, "log", 0, r.blk[1], r.bsize)), 0);
    uint8_t *got = malloc(1 << 16);
    uint32_t n = 0;
    for (int i = 0; i < 100000; i++) {
        if (i % 3 == 1)
            r.sim.read_fail_once = 1;
        int32_t k = (int32_t)CALL(&r, alfs_read(&r.fs, &f, got + n, 100));
        if (k == ALFS_EIO)
            continue;
        CHECK(k >= 0);
        if (k == 0)
            break;
        n += (uint32_t)k;
    }
    CHECK_EQ(close_done(&r, &f), 0);
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
    static const char *const names[] = {"f0", "f1", "f2"};
    for (uint32_t k = 0; k < 3; k++) {
        alfs_file_t f;
        if (create(r, &f, names[k], 0))
            tf_failed = 1;
        for (uint32_t i = 0; i < 300 * (k + 1); i++) {
            make_row(i, row);
            put(r, &f, row, ROW);
            if (i % 20 == 0)
                sync_done(r, &f);
        }
        close_done(r, &f);
        if (k == 1)
            del = f.version;
    }
    CALL(r, alfs_delete(&r->fs, "f1", del));
}

/* SAFE-10 and SPD-11: every call returns, on any content of the region. */
static void safe_fuzz_mount_read(void) {
    for (int iter = 0; iter < 400; iter++) {
        rig_t r;
        build_image(&r);
        r.sim.rng = 0x9E3779B9u * (uint32_t)(iter + 1) + seed;
        uint8_t *reg = r.sim.mem + r.cfg.region_start;
        switch (iter % 4) {
        case 0: /* bit flips anywhere */
            for (int i = 0; i < 1 + iter % 8; i++)
                reg[sim_rand(&r.sim) % r.cfg.region_size] ^= (uint8_t)(1u << (sim_rand(&r.sim) % 8));
            break;
        case 1: /* a sector's headers made random */
            sim_fill_random(&r.sim, r.cfg.region_start + (sim_rand(&r.sim) % 16) * SECTOR, 96);
            break;
        case 2: /* one sector's headers copied over another's */
            memcpy(reg + (sim_rand(&r.sim) % 16) * SECTOR, reg + (sim_rand(&r.sim) % 16) * SECTOR, 96);
            break;
        default: /* random bytes, keeping the first sector's stamp */
            sim_fill_random(&r.sim, r.cfg.region_start + 32, r.cfg.region_size - 32);
            break;
        }
        memset(r.sim.done, 0, r.sim.chip_size); /* bytes changed under it */
        if (remount(&r) == 0) {
            alfs_info_t in;
            uint32_t after = 0, first = 0;
            uint8_t *got = malloc(1 << 20);
            for (int guard = 0; guard < 64 && CALL(&r, alfs_list(&r.fs, after, &in)) == 0; guard++) {
                CHECK(in.version > after);
                after = in.version;
                if (!first)
                    first = in.version;
                slurp(&r, NULL, in.version, got, 1 << 20);
            }
            alfs_file_t f;
            if (create(&r, &f, "new", 0) == 0) {
                put(&r, &f, got, 100);
                close_done(&r, &f);
            }
            if (first)
                CALL(&r, alfs_delete(&r.fs, NULL, first));
            clean(&r);
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
        alfs_file_t f;
        CHECK_EQ(create(&r, &f, "log", 0), 0);
        log_t lg;
        log_init(&lg, 1 << 16);
        CHECK_EQ(log_run(&r, &f, &lg, 50, 10000, false), 0);
        CHECK_EQ(close_done(&r, &f), 0);
        sim_power_after(&r.sim, 0, (sim_pl_t)pl);
        CALL(&r, alfs_delete(&r.fs, "log", f.version));
        CHECK(r.sim.off);
        sim_reboot(&r.sim);
        CHECK_EQ(remount(&r), 0);
        uint8_t *got = malloc(1 << 16);
        int32_t n = slurp(&r, NULL, f.version, got, 1 << 16);
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
    alfs_file_t f;
    CHECK_EQ(create(&r, &f, "log", 0), 0);
    log_t lg;
    log_init(&lg, 1 << 16);
    CHECK_EQ(log_run(&r, &f, &lg, 50, 10000, false), 0);
    CHECK_EQ(close_done(&r, &f), 0);
    alfs_status_t st;
    alfs_status(&r.fs, &st);
    uint32_t used = 16u - st.ready;
    CHECK_EQ(CALL(&r, alfs_delete(&r.fs, "log", f.version)), 0);
    alfs_status(&r.fs, &st);
    CHECK_EQ(st.dirty, used);
    CHECK_EQ(MOUNT(&r), 0);
    alfs_status(&r.fs, &st);
    CHECK_EQ(st.dirty, used);
    uint8_t got[8];
    CHECK_EQ(slurp(&r, NULL, f.version, got, sizeof(got)), ALFS_ENOENT);
    free(lg.exp);
    DONE(&r);
    rig_close(&r);
}

/* The valid claim of file v's sector numbered n, or NULL. */
static uint8_t *claim_of(rig_t *r, uint32_t v, uint32_t n) {
    for (uint32_t k = 0; k < r->cfg.region_size / SECTOR; k++) {
        uint8_t *cl = r->sim.mem + r->cfg.region_start + k * SECTOR + C1;
        if (get32le(cl) == v && (uint32_t)(cl[10] | cl[11] << 8) == n && get32le(cl + 12) == crc32_ref(cl, 12))
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
        alfs_file_t f;
        CHECK_EQ(create(&r, &f, "log", 0), 0);
        log_t lg;
        log_init(&lg, 1 << 16);
        CHECK_EQ(log_run(&r, &f, &lg, 50, 10000, false), 0);
        CHECK_EQ(close_done(&r, &f), 0);
        sim_power_after(&r.sim, 0, PL_FLICKER);
        CALL(&r, alfs_delete(&r.fs, "log", f.version));
        CHECK(r.sim.off);
        sim_reboot(&r.sim);
        alfs_info_t in;
        bool deleted = false;
        for (int t = 0; t < 64 && !deleted; t++) {
            CHECK_EQ(remount(&r), 0);
            deleted = CALL(&r, alfs_list(&r.fs, 0, &in)) == ALFS_ENOENT;
        }
        CHECK(deleted);
        sim_power_after(&r.sim, k, PL_ATOMIC);
        clean(&r);
        bool past = !r.sim.off;
        sim_reboot(&r.sim);
        uint8_t *got = malloc(1 << 16);
        for (int t = 0; t < 8; t++) {
            CHECK_EQ(remount(&r), 0);
            int32_t n = slurp(&r, NULL, f.version, got, 1 << 16);
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

/* SAFE-12: a sector lost from the middle of a file is counted, and the
 * rest reads in order. */
static void safe_missing_sector(void) {
    rig_t r;
    rig_open(&r, 16, 0);
    CHECK_EQ(setup(&r), 0);
    alfs_file_t f;
    CHECK_EQ(create(&r, &f, "log", 0), 0);
    uint32_t total = 5u * 3900u;
    uint8_t *exp = malloc(total);
    for (uint32_t i = 0; i < total; i++)
        exp[i] = (uint8_t)(i * 5u + 3u);
    for (uint32_t n = 0; n < total; n += 3900) {
        CHECK_EQ(put(&r, &f, exp + n, 3900), 0);
        CHECK_EQ(sync_done(&r, &f), 0);
    }
    CHECK_EQ(close_done(&r, &f), 0);
    uint8_t *cl = claim_of(&r, f.version, 2);
    CHECK(cl != NULL);
    if (cl)
        sim_flip(&r.sim, (uint32_t)(cl + 12 - r.sim.mem), 0x01);
    alfs_status_t st;
    alfs_status(&r.fs, &st);
    uint32_t before = st.n.check_failures;
    uint8_t *got = malloc(total + 1024);
    int32_t n = slurp(&r, NULL, f.version, got, total + 1024);
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

/* SAFE-13: a create a power loss tore is absent, or empty and named as
 * created, or nameless and deleted by its version. */
static void safe_create_atomic(void) {
    for (int pl = 0; pl < PL_COUNT; pl++)
        for (int64_t k = 0;; k++) {
            rig_t r;
            rig_open(&r, 16, 0);
            CHECK_EQ(setup(&r), 0);
            CHECK_EQ(MOUNT(&r), 0); /* a fresh sector to zero first */
            alfs_file_t f;
            sim_power_after(&r.sim, k, (sim_pl_t)pl);
            int rc = create(&r, &f, "cfg", 0);
            if (!r.sim.off) {
                CHECK_EQ(rc, 0);
                rig_close(&r);
                break;
            }
            sim_reboot(&r.sim);
            uint32_t torn = 0;
            for (int t = 0; t < 8; t++) {
                CHECK_EQ(remount(&r), 0);
                alfs_info_t in;
                rc = (int)CALL(&r, alfs_list(&r.fs, 0, &in));
                if (rc == ALFS_ENOENT)
                    continue;
                CHECK_EQ(rc, 0);
                CHECK(strcmp(in.name, "cfg") == 0 || in.name[0] == 0);
                CHECK(!in.closed);
                CHECK_EQ(in.size, ALFS_SIZE_UNKNOWN);
                uint32_t v = in.version;
                uint8_t got[8];
                int32_t n = slurp(&r, NULL, v, got, sizeof(got));
                CHECK(n == 0 || n == ALFS_ENOENT);
                CHECK(CALL(&r, alfs_list(&r.fs, v, &in)) == ALFS_ENOENT); /* one file at most */
                torn = v;
            }
            if (torn) {
                rc = (int)CALL(&r, alfs_delete(&r.fs, NULL, torn));
                CHECK(rc == 0 || rc == ALFS_ENOENT);
            }
            CHECK_EQ(create(&r, &f, "cfg", 0), 0);
            CHECK(f.version > torn);
            CHECK_EQ(close_done(&r, &f), 0);
            DONE(&r);
            rig_close(&r);
            if (tf_failed) {
                printf("    behaviour %d, power lost at operation %lld\n", pl, (long long)k);
                return;
            }
        }
}

/* SAFE-14: a close a power loss tore leaves the file unclosed or closed
 * with its true size, and every byte synced readable. */
static void safe_close_atomic(void) {
    uint8_t d[1000];
    for (uint32_t i = 0; i < sizeof(d); i++)
        d[i] = (uint8_t)(i * 3u + 1u);
    for (int pl = 0; pl < PL_COUNT; pl++)
        for (int64_t k = 0;; k++) {
            rig_t r;
            rig_open(&r, 16, 0);
            CHECK_EQ(setup(&r), 0);
            alfs_file_t f;
            CHECK_EQ(create(&r, &f, "log", 0), 0);
            CHECK_EQ(put(&r, &f, d, sizeof(d)), 0);
            CHECK_EQ(sync_done(&r, &f), 0);
            sim_power_after(&r.sim, k, (sim_pl_t)pl);
            int rc = close_done(&r, &f);
            if (!r.sim.off) {
                CHECK_EQ(rc, 0);
                rig_close(&r);
                break;
            }
            sim_reboot(&r.sim);
            for (int t = 0; t < 8; t++) {
                CHECK_EQ(remount(&r), 0);
                alfs_info_t in;
                CHECK_EQ(info_of(&r, f.version, &in), 0);
                CHECK(strcmp(in.name, "log") == 0);
                CHECK_EQ(in.size, in.closed ? sizeof(d) : ALFS_SIZE_UNKNOWN);
                uint8_t got[2048];
                CHECK_EQ(slurp(&r, NULL, f.version, got, sizeof(got)), (int32_t)sizeof(d));
                CHECK(memcmp(got, d, sizeof(d)) == 0);
            }
            DONE(&r);
            rig_close(&r);
            if (tf_failed) {
                printf("    behaviour %d, power lost at operation %lld\n", pl, (long long)k);
                return;
            }
        }
}

/* ---- W-CFG ---------------------------------------------------------------- */

#define CFG_LEN 300u

static void cfg_bytes(uint32_t gen, uint8_t *out) {
    for (uint32_t i = 0; i < CFG_LEN; i++)
        out[i] = (uint8_t)(gen * 37u + i);
}

/* Every version of "config" older than the newest closed one, deleted, as
 * an application tidies after a save or at boot. */
static int cfg_tidy(rig_t *r) {
    alfs_info_t in;
    uint32_t after = 0, newest = 0, vers[16], nv = 0;
    while (CALL(r, alfs_list(&r->fs, after, &in)) == 0) {
        after = in.version;
        if (strcmp(in.name, "config"))
            continue;
        if (nv < 16)
            vers[nv++] = in.version;
        if (in.closed)
            newest = in.version;
    }
    if (r->sim.off)
        return -1;
    for (uint32_t i = 0; i < nv; i++) {
        if (vers[i] >= newest)
            continue;
        int rc = (int)CALL(r, alfs_delete(&r->fs, "config", vers[i]));
        if (rc && rc != ALFS_ENOENT)
            return rc;
    }
    return 0;
}

/* W-CFG: save a generation as a new version, then delete the older ones. */
static int cfg_save(rig_t *r, uint32_t gen) {
    uint8_t d[CFG_LEN];
    cfg_bytes(gen, d);
    alfs_file_t f;
    int rc = create(r, &f, "config", 0);
    if (!rc)
        rc = put(r, &f, d, CFG_LEN);
    if (!rc)
        rc = close_done(r, &f);
    return rc ? rc : cfg_tidy(r);
}

/* SAFE-14: power lost at every operation of a save; after it, version 0 is
 * always a whole configuration, through remounts and erases. */
static void safe_config_replace(void) {
    for (int pl = 0; pl < PL_COUNT; pl++)
        for (int64_t k = 0;; k++) {
            rig_t r;
            rig_open(&r, 32, 0);
            CHECK_EQ(setup(&r), 0);
            CHECK_EQ(cfg_save(&r, 1), 0);
            CHECK_EQ(cfg_save(&r, 2), 0);
            sim_power_after(&r.sim, k, (sim_pl_t)pl);
            int rc = cfg_save(&r, 3);
            if (!r.sim.off) {
                CHECK_EQ(rc, 0);
                rig_close(&r);
                break;
            }
            sim_reboot(&r.sim);
            CHECK_EQ(remount(&r), 0);
            CHECK_EQ(cfg_tidy(&r), 0); /* at boot */
            for (int t = 0; t < 8; t++) {
                CHECK_EQ(remount(&r), 0);
                CHECK(clean(&r) >= 0);
                uint8_t got[CFG_LEN + 64], want[CFG_LEN];
                int32_t n = slurp(&r, "config", 0, got, sizeof(got));
                if (n != (int32_t)CFG_LEN)
                    printf("    behaviour %d, loss at %lld, mount %d: read %d\n", pl, (long long)k, t, n);
                CHECK_EQ(n, (int32_t)CFG_LEN);
                bool whole = false;
                for (uint32_t gen = 2; gen <= 3 && !whole; gen++) {
                    cfg_bytes(gen, want);
                    whole = !memcmp(got, want, CFG_LEN);
                }
                CHECK(whole);
            }
            DONE(&r);
            rig_close(&r);
            if (tf_failed) {
                printf("    behaviour %d, power lost at operation %lld\n", pl, (long long)k);
                return;
            }
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
    CHECK(st.writable >= 32u * (SECTOR - 64u));
    CHECK_EQ(st.min_wear, 1);
    CHECK_EQ(st.max_wear, 1);
    alfs_file_t f;
    CHECK_EQ(create(&r, &f, "log", 0), 0);
    log_t lg;
    log_init(&lg, 1 << 18);
    CHECK_EQ(log_run(&r, &f, &lg, 50, 60000, false), 0);
    CHECK_EQ(close_done(&r, &f), 0);
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
    alfs_file_t f;
    CHECK_EQ(create(&r, &f, "log", 0), 0);
    uint8_t row[ROW] = {1};
    CHECK_EQ(put(&r, &f, row, ROW), 0);
    CHECK_EQ(close_done(&r, &f), 0);
    CHECK_EQ(CALL(&r, alfs_delete(&r.fs, "log", f.version)), 0);
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
    CHECK(sizeof(alfs_t) <= 96 + 4 * wide);
    CHECK(sizeof(alfs_file_t) <= 48 + 2 * wide);
}

int main(int argc, char **argv) {
    static const tf_test_t tests[] = {
        {"sim_torn_obeys_a9", sim_torn_obeys_a9},
        {"if_geometry", if_geometry},
        {"if_endian", if_endian},
        {"if_names", if_names},
        {"if_list_files", if_list_files},
        {"if_version_never_reused", if_version_never_reused},
        {"if_open_version0", if_open_version0},
        {"if_closed_read_only", if_closed_read_only},
        {"if_delete_open_refused", if_delete_open_refused},
        {"if_empty_file", if_empty_file},
        {"basic_round_trip", basic_round_trip},
        {"spd_no_erase_outside_erase", spd_no_erase_outside_erase},
        {"spd_write_burst_bounded", spd_write_burst_bounded},
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
        {"safe_create_atomic", safe_create_atomic},
        {"safe_close_atomic", safe_close_atomic},
        {"safe_config_replace", safe_config_replace},
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
