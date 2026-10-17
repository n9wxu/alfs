/*
 * alfs's tests, one or more for each requirement of docs/REQUIREMENTS.md.
 * Each is named in the requirement's "Verified by".
 *
 * Every call a test makes goes through CALL, OCALL or an erase wrapper,
 * which hold it to SPD-01 (no erase outside an erase) and record the
 * longest single operation (SPD-06); the configuration application's own
 * calls are its own. Every test ends with DONE, which fails it if the
 * simulator saw a violation (a program across a page, a byte programmed
 * twice, or a program into a byte a power loss touched, each but a
 * program that settles, docs/TEST_PLAN.md 2), or if a byte outside the
 * areas changed. The simulator's seed comes from ALFS_SEED.
 */
#include "alfs_log.h"
#include "alfs_obj.h"
#include "kv.h"
#include "sim.h"
#include "tf.h"

#include <stdlib.h>
#include <string.h>

#define SECTOR 4096u
#define PAGE 256u
#define ROW 22u      /* W-LOG's */
#define HR_ROW 48u   /* W-HR's */
#define RUN_ROW 200u /* the rows a run of bytes is written as */
#define ROW_MAX ALFS_ROW_MAX(PAGE)
#define BLOCK_MAX 4096u
#define LSTAMP 18u /* a log sector's stamp, then its claim (docs/SPEC.md 1) */
#define LDATA 30u  /* its first chunk */
#define CHUNK 7u   /* a chunk's header */
#define OBEGUN 16u /* a half's begun mark, then its commit (SPEC 6) */
#define OCOMMIT 17u
#define CFG_SECTORS 3u /* the configuration area's */
#define KV_SLOTS 12u

/* ---- the rig ---------------------------------------------------------- */

/* What the driver wrapper saw, in the current call (docs/TEST_PLAN.md 3). */
typedef struct {
    uint64_t claims;  /* programs starting a log sector */
    uint64_t zeroes;  /* 12-byte programs of 00h */
    uint64_t headers; /* reads of a log sector's first bytes */
} seen_t;

typedef struct {
    sim_t sim;
    alfs_driver_t raw, drv;
    alfs_area_t lcfg, ocfg, ccfg;
    alfs_log_t lg, cg; /* the flight log area, the configuration area */
    alfs_obj_t ob;
    kv_t kv;
    kv_slot_t slots[KV_SLOTS];
    uint32_t bsize; /* the blocks', a page unless a test says */
    uint8_t lscratch[PAGE], oscratch[PAGE], cscratch[PAGE], kblock[PAGE];
    uint8_t blk[4][BLOCK_MAX];
    uint8_t gap[2][SECTOR]; /* the sectors around the areas, as filled */
    seen_t seen;
    bool oready_was; /* OCALL's: the spare half was ready before the call */
} rig_t;

static int call_bad;           /* calls that broke SPD-01 */
static char call_why[96];
static uint64_t longest_stall; /* the longest operation outside an erase, us */
static uint32_t seed;          /* the simulator's, from ALFS_SEED */

static bool in_log(const rig_t *r, uint32_t addr) {
    return addr >= r->lcfg.start && addr < r->lcfg.start + r->lcfg.size;
}

static int w_read(void *ctx, uint32_t addr, void *buf, uint32_t len) {
    rig_t *r = ctx;
    if (in_log(r, addr) && addr % SECTOR < LDATA)
        r->seen.headers++;
    return r->raw.read(r->raw.ctx, addr, buf, len);
}

static int w_prog(void *ctx, uint32_t addr, const void *buf, uint32_t len) {
    rig_t *r = ctx;
    const uint8_t *p = buf;
    if (in_log(r, addr) && addr % SECTOR == 0)
        r->seen.claims++;
    bool zero = len == 12;
    for (uint32_t i = 0; zero && i < len; i++)
        zero = p[i] == 0;
    if (zero)
        r->seen.zeroes++;
    return r->raw.prog(r->raw.ctx, addr, buf, len);
}

static int w_erase(void *ctx, uint32_t addr) {
    rig_t *r = ctx;
    return r->raw.erase(r->raw.ctx, addr);
}

static void area(alfs_area_t *a, uint32_t start, uint32_t sectors, uint32_t chip) {
    memset(a, 0, sizeof(*a));
    a->start = start;
    a->size = sectors * SECTOR;
    a->chip_size = chip;
    a->sector_size = SECTOR;
    a->page_size = PAGE;
}

/* The chip: a gap sector, the configuration area, the object area, the
 * flight log area, a gap sector. */
static void rig_open(rig_t *r, uint32_t log_sectors, uint32_t obj_sectors) {
    uint32_t chip = (2u + CFG_SECTORS + obj_sectors + log_sectors) * SECTOR;
    memset(r, 0, sizeof(*r));
    sim_init(&r->sim, chip, SECTOR, PAGE, 0xFF, seed);
    sim_fill_random(&r->sim, 0, chip); /* what a used chip holds */
    memcpy(r->gap[0], r->sim.mem, SECTOR);
    memcpy(r->gap[1], r->sim.mem + chip - SECTOR, SECTOR);
    r->raw = sim_driver(&r->sim);
    r->drv.ctx = r;
    r->drv.read = w_read;
    r->drv.prog = w_prog;
    r->drv.erase = w_erase;
    area(&r->ccfg, SECTOR, CFG_SECTORS, chip);
    area(&r->ocfg, (1u + CFG_SECTORS) * SECTOR, obj_sectors, chip);
    area(&r->lcfg, (1u + CFG_SECTORS + obj_sectors) * SECTOR, log_sectors, chip);
    r->bsize = PAGE;
    call_bad = 0;
    call_why[0] = 0;
    longest_stall = 0;
}

static void rig_close(rig_t *r) {
    sim_free(&r->sim);
}

static void begin(rig_t *r) {
    sim_call_begin(&r->sim);
    memset(&r->seen, 0, sizeof(r->seen));
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

static bool spare_ready(rig_t *r) {
    alfs_obj_status_t st;
    alfs_obj_status(&r->ob, &st);
    return st.ready;
}

#define CALL(r, e) (begin(r), chk((r), (int64_t)(e), false, #e))
/* A put or delete: it may erase only when the spare half was not ready. */
#define OCALL(r, e) ((r)->oready_was = spare_ready(r), begin(r), chk((r), (int64_t)(e), !(r)->oready_was, #e))
#define LERASE(r, log) ((int)(begin(r), chk((r), alfs_log_erase(&(r)->lg, (log), 1), true, "log erase")))
#define OERASE(r) ((int)(begin(r), chk((r), alfs_obj_erase(&(r)->ob, 1), true, "obj erase")))
#define OFORMAT(r)                                                                                 \
    ((int)(begin(r), chk((r), alfs_obj_format(&(r)->ob, &(r)->ocfg, &(r)->drv, (r)->oscratch), true, "format")))
#define LMOUNT(r) ((int)CALL((r), alfs_log_mount(&(r)->lg, &(r)->lcfg, &(r)->drv, (r)->lscratch)))
#define OMOUNT(r) ((int)CALL((r), alfs_obj_mount(&(r)->ob, &(r)->ocfg, &(r)->drv, (r)->oscratch)))
#define CMOUNT(r) ((int)CALL((r), alfs_log_mount(&(r)->cg, &(r)->ccfg, &(r)->drv, (r)->cscratch)))

/* The configuration area mounted and its settings loaded. */
static int cload(rig_t *r) {
    memset(&r->cg, 0, sizeof(r->cg));
    int rc = CMOUNT(r);
    return rc ? rc : kv_load(&r->kv, &r->cg, r->slots, KV_SLOTS, r->kblock, PAGE);
}

/* A reboot: every handle gone, every area mounted again, the settings
 * loaded. ALFS_ENOFS from the object area is not an error here. */
static int remount(rig_t *r) {
    memset(&r->lg, 0, sizeof(r->lg));
    memset(&r->ob, 0, sizeof(r->ob));
    int rc = LMOUNT(r);
    int ro = OMOUNT(r);
    int rk = cload(r);
    return rc ? rc : rk ? rk : ro == ALFS_ENOFS ? 0 : ro;
}

/* Log erase steps until nothing is dirty. The number of steps, or -1. */
static int lclean_log(rig_t *r, uint32_t log) {
    for (int i = 0; i < 2000000; i++) {
        int rc = LERASE(r, log);
        if (rc == ALFS_CLEAN)
            return i;
        if ((rc < 0 && rc != ALFS_EIO) || r->sim.off)
            return -1;
    }
    return -1;
}

static int lclean(rig_t *r) {
    return lclean_log(r, 0);
}

/* Object erase steps until the spare half is ready. */
static int oclean(rig_t *r) {
    for (int i = 0; i < 100000; i++) {
        int rc = OERASE(r);
        if (rc == ALFS_CLEAN)
            return i;
        if (rc < 0 || r->sim.off)
            return -1;
    }
    return -1;
}

/* The areas mounted, the flight log area erased, the settings loaded, the
 * object area formatted. */
static int setup(rig_t *r) {
    if (LMOUNT(r) || lclean(r) < 0 || cload(r))
        return -1;
    return OFORMAT(r) ? -1 : 0;
}

static bool gaps_kept(const rig_t *r) {
    return !memcmp(r->gap[0], r->sim.mem, SECTOR) &&
           !memcmp(r->gap[1], r->sim.mem + r->sim.chip_size - SECTOR, SECTOR);
}

/* Ends a test: no violation of the flash's rules, of SPD-01, or of the
 * bytes outside the areas (SAFE-09). */
#define DONE(r)                                                                                    \
    do {                                                                                           \
        if ((r)->sim.violations)                                                                   \
            printf("    flash: %s\n", (r)->sim.why);                                               \
        if (call_bad)                                                                              \
            printf("    call: %s\n", call_why);                                                    \
        CHECK_EQ((r)->sim.violations, 0);                                                          \
        CHECK_EQ(call_bad, 0);                                                                     \
        CHECK(gaps_kept(r));                                                                       \
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

static uint32_t get16le(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8;
}

/* A W-LOG row: its sequence number and a check. */
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

static void fill(uint8_t *p, uint32_t n, uint32_t gen) {
    for (uint32_t i = 0; i < n; i++)
        p[i] = (uint8_t)(gen * 131u + i * 7u + (i >> 8));
}

/* ---- logs ------------------------------------------------------------- */

static int lopen(rig_t *r, alfs_writer_t *w, uint32_t log, uint8_t tag, int b) {
    return (int)CALL(r, alfs_log_open(&r->lg, w, log, tag, r->blk[b], r->bsize));
}

/* Writes d as rows of `row` bytes, the last one shorter, adding the bytes
 * of the rows alfs took to *taken. 0, or the error. */
static int put_rows(rig_t *r, alfs_writer_t *w, const uint8_t *d, uint32_t n, uint32_t row, uint32_t *taken) {
    for (uint32_t i = 0; i < n;) {
        uint32_t k = n - i < row ? n - i : row;
        int rc = (int)CALL(r, alfs_log_write(&r->lg, w, d + i, k));
        if (rc)
            return rc;
        i += k;
        *taken += k;
    }
    return 0;
}

static int put(rig_t *r, alfs_writer_t *w, const uint8_t *d, uint32_t n) {
    uint32_t taken = 0;
    return put_rows(r, w, d, n, RUN_ROW, &taken);
}

static int lsync(rig_t *r, alfs_writer_t *w) {
    return (int)CALL(r, alfs_log_sync(&r->lg, w));
}

static int lclose(rig_t *r, alfs_writer_t *w) {
    return (int)CALL(r, alfs_log_close(&r->lg, w));
}

/* Reads a whole stream, row after row, end to end. Its length, or an
 * error. */
static int32_t slurp(rig_t *r, uint32_t log, uint8_t tag, uint8_t *out, uint32_t cap) {
    alfs_reader_t rd;
    int rc = (int)CALL(r, alfs_log_open_read(&r->lg, &rd, log, tag, r->blk[3], r->bsize));
    if (rc)
        return rc;
    uint32_t n = 0;
    int32_t result = -201;
    for (int guard = 0; guard < 1000000; guard++) {
        int32_t k = (int32_t)CALL(r, alfs_log_read(&r->lg, &rd, out + n, cap - n));
        if (k == ALFS_EINVAL) {
            result = -200; /* a row past what was ever written */
            break;
        }
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
    CALL(r, alfs_log_close_read(&r->lg, &rd));
    return result;
}

/* The listing entry for log n, or ALFS_ENOENT. */
static int log_info(rig_t *r, uint32_t n, alfs_log_info_t *in) {
    int rc = (int)CALL(r, alfs_log_list(&r->lg, n - 1, in));
    return rc ? rc : in->log == n ? ALFS_OK : ALFS_ENOENT;
}

/* The sectors of the log area holding a valid claim of log n, by the
 * simulator's flash. */
static uint32_t claims_of(const rig_t *r, uint32_t n, int32_t *last) {
    uint32_t count = 0;
    for (uint32_t k = 0; k < r->lcfg.size / SECTOR; k++) {
        const uint8_t *c = r->sim.mem + r->lcfg.start + k * SECTOR + LSTAMP;
        if (get32le(c) == n && get32le(c + 8) == crc32_ref(c, 8)) {
            count++;
            if (last)
                *last = (int32_t)k;
        }
    }
    return count;
}

/*
 * A logging run: rows produced at `rate` a second, written every 200 ms,
 * synced every second, `ms` long, with an erase step of dirty sectors every
 * 20 ms if asked. Rows go to exp[]; synced is the length a successful sync
 * covered. Stops early when the power is lost. 0, or the first error that
 * is not a power loss.
 */
typedef struct {
    uint8_t *exp;
    uint32_t len, cap;
    uint32_t synced;
    uint32_t seq;
} run_t;

static int log_run(rig_t *r, alfs_writer_t *w, run_t *lg, uint32_t rate, uint32_t ms, bool steps) {
    uint32_t due = 0;
    for (uint32_t t = 20; t <= ms; t += 20) {
        if (steps) {
            int rc = LERASE(r, 0);
            if (r->sim.off)
                return 0;
            if (rc < 0 && rc != ALFS_EIO)
                return rc;
        }
        if (t % 200)
            continue;
        due += rate;
        uint32_t n = 0;
        while (due >= 5 && lg->len + (n + 1) * ROW <= lg->cap) {
            make_row(lg->seq++, lg->exp + lg->len + n * ROW);
            n++;
            due -= 5;
        }
        if (n) {
            uint32_t taken = 0;
            int rc = put_rows(r, w, lg->exp + lg->len, n * ROW, ROW, &taken);
            lg->len += taken;
            if (r->sim.off)
                return 0;
            if (rc)
                return rc;
        }
        if (t % 1000 == 0) {
            int rc = lsync(r, w);
            if (r->sim.off)
                return 0;
            if (rc)
                return rc;
            lg->synced = lg->len;
        }
    }
    return 0;
}

static void run_init(run_t *lg, uint32_t cap) {
    memset(lg, 0, sizeof(*lg));
    lg->exp = malloc(cap);
    lg->cap = cap;
}

/* ---- objects ---------------------------------------------------------- */

/* Puts an object in pieces of up to 100 bytes. 0, or the error. */
static int oput(rig_t *r, const char *name, const uint8_t *d, uint32_t n) {
    alfs_put_t p;
    int rc = (int)OCALL(r, alfs_obj_put(&r->ob, &p, name, n));
    for (uint32_t i = 0; !rc && i < n;) {
        uint32_t k = n - i < 100 ? n - i : 100;
        int32_t m = (int32_t)CALL(r, alfs_obj_write(&r->ob, &p, d + i, k));
        if (m < 0)
            return (int)m;
        i += (uint32_t)m;
    }
    return rc ? rc : (int)CALL(r, alfs_obj_commit(&r->ob, &p));
}

static int odelete(rig_t *r, const char *name) {
    return (int)OCALL(r, alfs_obj_delete(&r->ob, name));
}

/* Gets a whole object. Its size, or an error. */
static int32_t oget(rig_t *r, const char *name, uint8_t *out, uint32_t cap) {
    alfs_get_t g;
    int rc = (int)CALL(r, alfs_obj_open(&r->ob, &g, name));
    if (rc)
        return rc;
    uint32_t n = 0;
    int32_t result = 0;
    for (int guard = 0; guard < 100000; guard++) {
        uint32_t want = cap - n < 77 ? cap - n : 77;
        int32_t k = (int32_t)CALL(r, alfs_obj_read(&r->ob, &g, out + n, want));
        if (k <= 0) {
            result = k < 0 ? k : (int32_t)n;
            break;
        }
        n += (uint32_t)k;
        if (n > cap) {
            result = -200;
            break;
        }
    }
    CALL(r, alfs_obj_close(&r->ob, &g));
    return result;
}

/* An object reads exactly d[n]. */
static bool reads_as(rig_t *r, const char *name, const uint8_t *d, uint32_t n) {
    static uint8_t got[16384];
    int32_t m = oget(r, name, got, sizeof(got));
    return m == (int32_t)n && !memcmp(got, d, n);
}

/* ---- the configuration application ------------------------------------ */

static int kset(rig_t *r, const char *key, const uint8_t *v, uint32_t n) {
    return kv_set(&r->kv, key, v, n);
}

static int kdel(rig_t *r, const char *key) {
    return kv_set(&r->kv, key, NULL, 0);
}

/* A key's value into out: its length, or an error. */
static int32_t kget(rig_t *r, const char *key, uint8_t *out, uint32_t cap) {
    const uint8_t *v;
    uint32_t len = 0;
    int rc = kv_get(&r->kv, key, &v, &len);
    if (rc)
        return rc;
    if (len > cap)
        return -200;
    memcpy(out, v, len);
    return (int32_t)len;
}

static bool kreads(rig_t *r, const char *key, const uint8_t *v, uint32_t n) {
    uint8_t got[PAGE];
    return kget(r, key, got, sizeof(got)) == (int32_t)n && !memcmp(got, v, n);
}

/* W-CFG's sets and deletes: ten keys, values of 1 to 150 bytes (len 0: a
 * delete), each byte a function of a generation. */
typedef struct {
    char key[8];
    uint32_t len, gen;
} kv_op_t;

static kv_op_t kv_step(int i, uint32_t most) {
    kv_op_t op;
    snprintf(op.key, sizeof(op.key), "k%d", (i * 7) % 10);
    op.gen = (uint32_t)i + 1u;
    op.len = i % 9 == 8 ? 0u : 1u + (uint32_t)(i * 37) % most;
    return op;
}

typedef struct {
    uint32_t len[10], gen[10];
    bool present[10];
} kv_state_t;

static int kv_do(rig_t *r, kv_op_t op) {
    uint8_t v[PAGE];
    fill(v, op.len, op.gen);
    return kset(r, op.key, v, op.len); /* an empty value deletes */
}

static void kv_apply(kv_state_t *st, kv_op_t op) {
    int i = op.key[1] - '0';
    st->present[i] = op.len != 0;
    st->len[i] = op.len;
    st->gen[i] = op.gen;
}

static bool kv_reads(rig_t *r, const kv_state_t *st, int i) {
    char key[4] = {'k', (char)('0' + i), 0};
    uint8_t want[PAGE], got[PAGE];
    if (!st->present[i])
        return kget(r, key, got, sizeof(got)) == ALFS_ENOENT;
    fill(want, st->len[i], st->gen[i]);
    return kreads(r, key, want, st->len[i]);
}

/* ---- the simulator ------------------------------------------------------ */

/* 4.4: a torn program moves only bits its data moves (A-9), into erased
 * bytes and over programmed ones, under every behaviour. */
static void sim_torn_obeys_a9(void) {
    for (int pl = 0; pl < PL_COUNT; pl++)
        for (int t = 0; t < 500; t++) {
            sim_t s;
            sim_init(&s, 4 * SECTOR, SECTOR, PAGE, 0xFF, seed + (uint32_t)t);
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
    rig_open(&r, 8, 4);
    for (int store = 0; store < 2; store++) {
        alfs_area_t *a = store ? &r.ocfg : &r.lcfg, good = *a;
        struct {
            const char *what;
            uint32_t *field;
            uint32_t value;
        } bad[] = {
            {"area not sector-aligned", &a->start, a->start + 1},
            {"area past the chip", &a->size, 64 * SECTOR},
            {"sector not a power of two", &a->sector_size, 3000},
            {"sector under four pages", &a->sector_size, 512},
            {"page not a power of two", &a->page_size, 200},
            {"page under 128 bytes", &a->page_size, 64},
            {"empty area", &a->size, 0},
        };
        for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            *a = good;
            *bad[i].field = bad[i].value;
            int rc = store ? OFORMAT(&r) : LMOUNT(&r);
            if (rc != ALFS_EGEOMETRY && rc != ALFS_EINVAL)
                printf("    store %d accepted: %s (%d)\n", store, bad[i].what, rc);
            CHECK(rc == ALFS_EGEOMETRY || rc == ALFS_EINVAL);
            CHECK_EQ(r.sim.c_ops, 0);
            if (store) {
                CHECK(OMOUNT(&r) < 0);
                CHECK_EQ(r.sim.c_ops, 0);
            }
        }
        *a = good;
    }
    r.ocfg.size = 3 * SECTOR; /* halves must be equal */
    CHECK_EQ(OFORMAT(&r), ALFS_EGEOMETRY);
    CHECK_EQ(r.sim.c_ops, 0);
    r.ocfg.size = 4 * SECTOR;

    /* An area of one geometry does not mount as another. */
    CHECK_EQ(setup(&r), 0);
    alfs_log_status_t st;
    alfs_log_status(&r.lg, &st);
    CHECK_EQ(st.ready, 8);
    r.lcfg.size = 6 * SECTOR;
    CHECK_EQ(remount(&r), 0);
    alfs_log_status(&r.lg, &st);
    CHECK_EQ(st.ready, 0);
    CHECK_EQ(st.dirty, 6);
    r.lcfg.size = 8 * SECTOR;
    r.ocfg.size = 2 * SECTOR;
    memset(&r.ob, 0, sizeof(r.ob));
    CHECK_EQ(OMOUNT(&r), ALFS_ENOFS);
    DONE(&r);
    rig_close(&r);
}

/* A log sector's and a half's bytes, little-endian, where docs/SPEC.md
 * puts them. */
static void if_endian(void) {
    rig_t r;
    rig_open(&r, 8, 2);
    CHECK_EQ(setup(&r), 0);
    const uint8_t *st = r.sim.mem + r.lcfg.start;
    CHECK(memcmp(st, "alfl", 4) == 0);
    CHECK_EQ(st[4], 5);  /* version           */
    CHECK_EQ(st[5], 12); /* log2(sector size) */
    CHECK_EQ(st[6], 8);  /* log2(page size)   */
    CHECK_EQ(st[7], 0);
    CHECK_EQ(get16le(st + 8), 8);
    CHECK(get32le(st + 10) >= 1); /* sequence floor */
    CHECK_EQ(get32le(st + 14), crc32_ref(st, 14));

    alfs_writer_t w;
    CHECK_EQ(lopen(&r, &w, 0, 5, 0), 0);
    const uint8_t data[3] = {7, 8, 9};
    CHECK_EQ(put(&r, &w, data, 3), 0);
    CHECK_EQ(lclose(&r, &w), 0);
    int32_t k = -1;
    CHECK_EQ(claims_of(&r, w.log, &k), 1);
    const uint8_t *sec = r.sim.mem + r.lcfg.start + (uint32_t)k * SECTOR;
    const uint8_t *cl = sec + LSTAMP;
    CHECK_EQ(get32le(cl), w.log);     /* log number                      */
    CHECK_EQ(get32le(cl + 4), w.log); /* sequence: a log's first claim's */
    CHECK_EQ(get32le(cl + 8), crc32_ref(cl, 8));
    const uint8_t *ch = sec + LDATA;
    CHECK_EQ(get16le(ch), 4); /* the row and its length */
    CHECK_EQ(ch[2], 5);       /* tag */
    uint8_t body[7] = {ch[0], ch[1], ch[2], 3, 7, 8, 9};
    CHECK_EQ(get32le(ch + 3), crc32_ref(body, 7));
    CHECK_EQ(ch[7], 3); /* the row's length */
    CHECK(memcmp(ch + 8, data, 3) == 0);

    uint8_t obj[5] = {1, 2, 3, 4, 5};
    CHECK_EQ(oput(&r, "ab", obj, 5), 0);
    alfs_obj_status_t os;
    alfs_obj_status(&r.ob, &os);
    const uint8_t *h = r.sim.mem + r.ocfg.start + (os.seq % 2 ? 0 : SECTOR); /* commit 1 in half 0, 2 in 1 */
    CHECK(memcmp(h, "alfo", 4) == 0);
    CHECK_EQ(h[4], 5);
    CHECK_EQ(h[5], 12);
    CHECK_EQ(h[6], 8);
    CHECK_EQ(h[7], 0);
    CHECK_EQ(get16le(h + 8), 1); /* sectors in a half */
    CHECK_EQ(get16le(h + 10), 0);
    CHECK_EQ(get32le(h + 12), crc32_ref(h, 12));
    CHECK_EQ(h[OBEGUN], 0);
    const uint8_t *cm = h + OCOMMIT;
    CHECK_EQ(get32le(cm), os.seq);
    CHECK_EQ(get32le(cm + 4), PAGE + 32 + 4 + 5 + 4); /* end */
    CHECK_EQ(get32le(cm + 8), crc32_ref(cm, 8));
    const uint8_t *ob = h + PAGE;
    CHECK(memcmp(ob, "ab", 3) == 0);
    for (int i = 3; i < 32; i++)
        CHECK_EQ(ob[i], 0);
    CHECK_EQ(get32le(ob + 32), 5);
    CHECK(memcmp(ob + 36, obj, 5) == 0);
    CHECK_EQ(get32le(ob + 41), crc32_ref(ob, 41));
    DONE(&r);
    rig_close(&r);
}

static void if_object_names(void) {
    static const char *const good[] = {"a", "0123456789012345678901234567890", "lua/main.lua"};
    rig_t r;
    rig_open(&r, 4, 4);
    CHECK_EQ(setup(&r), 0);
    for (int i = 0; i < 3; i++) {
        uint8_t b = (uint8_t)i;
        CHECK_EQ(oput(&r, good[i], &b, 1), 0);
    }
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < 3; i++) {
            uint8_t b = (uint8_t)i;
            CHECK(reads_as(&r, good[i], &b, 1));
        }
        CHECK_EQ(remount(&r), 0);
    }
    static const char *const bad[] = {"", "01234567890123456789012345678901"};
    alfs_put_t p;
    alfs_get_t g;
    for (int i = 0; i < 2; i++) {
        CHECK_EQ(OCALL(&r, alfs_obj_put(&r.ob, &p, bad[i], 1)), ALFS_EINVAL);
        CHECK_EQ(r.sim.c_ops, 0);
        CHECK_EQ(CALL(&r, alfs_obj_open(&r.ob, &g, bad[i])), ALFS_EINVAL);
    }
    CHECK_EQ(OCALL(&r, alfs_obj_put(&r.ob, &p, NULL, 1)), ALFS_EINVAL);
    CHECK_EQ(r.sim.c_ops, 0);
    CHECK_EQ(CALL(&r, alfs_obj_open(&r.ob, &g, "lua")), ALFS_ENOENT); /* a prefix is not the name */
    CHECK_EQ(CALL(&r, alfs_obj_open(&r.ob, &g, "lua/main.lua2")), ALFS_ENOENT);
    DONE(&r);
    rig_close(&r);
}

static void if_put_replaces(void) {
    rig_t r;
    rig_open(&r, 4, 4);
    CHECK_EQ(setup(&r), 0);
    uint8_t v1[300], v2[400], other[50];
    fill(v1, sizeof(v1), 1);
    fill(v2, sizeof(v2), 2);
    fill(other, sizeof(other), 3);
    CHECK_EQ(oput(&r, "cfg", v1, sizeof(v1)), 0);
    CHECK_EQ(oput(&r, "other", other, sizeof(other)), 0);

    alfs_put_t p;
    CHECK_EQ(OCALL(&r, alfs_obj_put(&r.ob, &p, "cfg", sizeof(v2))), 0);
    CHECK_EQ(CALL(&r, alfs_obj_write(&r.ob, &p, v2, 200)), 200);
    CHECK(reads_as(&r, "cfg", v1, sizeof(v1))); /* the old bytes until the commit */
    CHECK_EQ(CALL(&r, alfs_obj_commit(&r.ob, &p)), ALFS_EINVAL); /* short */
    CHECK_EQ(CALL(&r, alfs_obj_write(&r.ob, &p, v2 + 200, 201)), ALFS_EINVAL); /* past the size */
    CHECK_EQ(CALL(&r, alfs_obj_write(&r.ob, &p, v2 + 200, 200)), 200);
    CHECK_EQ(CALL(&r, alfs_obj_commit(&r.ob, &p)), 0);
    CHECK(reads_as(&r, "cfg", v2, sizeof(v2)));
    CHECK(reads_as(&r, "other", other, sizeof(other)));

    CHECK_EQ(odelete(&r, "cfg"), 0);
    uint8_t got[8];
    CHECK_EQ(oget(&r, "cfg", got, sizeof(got)), ALFS_ENOENT);
    CHECK_EQ(oclean(&r) >= 0, 1);
    CHECK_EQ(odelete(&r, "cfg"), ALFS_ENOENT);
    CHECK_EQ(r.sim.c_ops, 0);
    CHECK_EQ(remount(&r), 0);
    CHECK_EQ(oget(&r, "cfg", got, sizeof(got)), ALFS_ENOENT);
    CHECK(reads_as(&r, "other", other, sizeof(other)));
    uint8_t empty = 0;
    CHECK_EQ(oput(&r, "empty", &empty, 0), 0);
    CHECK(reads_as(&r, "empty", &empty, 0));
    DONE(&r);
    rig_close(&r);
}

static void if_put_one_at_a_time(void) {
    rig_t r;
    rig_open(&r, 4, 4);
    CHECK_EQ(setup(&r), 0);
    uint8_t a[100], b[100];
    fill(a, sizeof(a), 1);
    fill(b, sizeof(b), 2);
    CHECK_EQ(oput(&r, "a", a, sizeof(a)), 0);
    alfs_put_t p, q;
    CHECK_EQ(OCALL(&r, alfs_obj_put(&r.ob, &p, "a", sizeof(b))), 0);
    CHECK_EQ(OCALL(&r, alfs_obj_put(&r.ob, &q, "b", sizeof(b))), ALFS_EOPEN);
    CHECK_EQ(odelete(&r, "a"), ALFS_EOPEN);
    CHECK_EQ(OERASE(&r), ALFS_EOPEN);
    CHECK_EQ(CALL(&r, alfs_obj_write(&r.ob, &q, b, 10)), ALFS_EINVAL); /* not the put in progress */
    CHECK_EQ(CALL(&r, alfs_obj_write(&r.ob, &p, b, 50)), 50);
    CHECK_EQ(CALL(&r, alfs_obj_abort(&r.ob, &p)), 0);
    CHECK_EQ(CALL(&r, alfs_obj_write(&r.ob, &p, b, 50)), ALFS_EINVAL);
    CHECK(reads_as(&r, "a", a, sizeof(a)));
    CHECK_EQ(oput(&r, "b", b, sizeof(b)), 0);
    CHECK(reads_as(&r, "a", a, sizeof(a)));
    CHECK(reads_as(&r, "b", b, sizeof(b)));
    DONE(&r);
    rig_close(&r);
}

static void if_get_snapshot(void) {
    rig_t r;
    rig_open(&r, 4, 4);
    CHECK_EQ(setup(&r), 0);
    static uint8_t old[4096], now[4096], got[4096];
    fill(old, sizeof(old), 1);
    fill(now, sizeof(now), 2);
    CHECK_EQ(oput(&r, "lua", old, sizeof(old)), 0);
    CHECK(oclean(&r) >= 0);
    alfs_get_t g;
    CHECK_EQ(CALL(&r, alfs_obj_open(&r.ob, &g, "lua")), 0);
    CHECK_EQ(g.size, sizeof(old));
    CHECK_EQ(CALL(&r, alfs_obj_read(&r.ob, &g, got, 1000)), 1000);
    CHECK_EQ(oput(&r, "lua", now, sizeof(now)), 0); /* the spare was ready: no erase */
    uint32_t n = 1000;
    while (n < sizeof(got)) {
        int32_t k = (int32_t)CALL(&r, alfs_obj_read(&r.ob, &g, got + n, sizeof(got) - n));
        CHECK(k > 0);
        n += (uint32_t)k;
    }
    CHECK_EQ(CALL(&r, alfs_obj_read(&r.ob, &g, got, 1)), 0);
    CHECK(memcmp(got, old, sizeof(old)) == 0);
    /* The reader holds the half a put would now erase. */
    alfs_put_t p;
    CHECK_EQ(OCALL(&r, alfs_obj_put(&r.ob, &p, "x", 1)), ALFS_EOPEN);
    CHECK_EQ(odelete(&r, "lua"), ALFS_EOPEN);
    CHECK_EQ(OERASE(&r), ALFS_EOPEN);
    CHECK_EQ(r.sim.c_ops, 0);
    CHECK_EQ(CALL(&r, alfs_obj_close(&r.ob, &g)), 0);
    CHECK(reads_as(&r, "lua", now, sizeof(now)));
    CHECK_EQ(oput(&r, "x", old, 1), 0);
    CHECK(reads_as(&r, "lua", now, sizeof(now)));
    DONE(&r);
    rig_close(&r);
}

static void if_object_list(void) {
    rig_t r;
    rig_open(&r, 4, 4);
    CHECK_EQ(setup(&r), 0);
    uint8_t d[300];
    fill(d, sizeof(d), 9);
    CHECK_EQ(oput(&r, "a", d, 10), 0);
    CHECK_EQ(oput(&r, "b", d, 20), 0);
    CHECK_EQ(oput(&r, "c", d, 30), 0);
    CHECK_EQ(odelete(&r, "b"), 0);
    CHECK_EQ(oput(&r, "a", d, 300), 0); /* a put goes after the rest */
    for (int pass = 0; pass < 2; pass++) {
        static const char *const want[] = {"c", "a"};
        static const uint32_t size[] = {30, 300};
        alfs_obj_info_t in;
        for (uint32_t i = 0; i < 2; i++) {
            CHECK_EQ(CALL(&r, alfs_obj_list(&r.ob, i, &in)), 0);
            CHECK(strcmp(in.name, want[i]) == 0);
            CHECK_EQ(in.size, size[i]);
        }
        CHECK_EQ(CALL(&r, alfs_obj_list(&r.ob, 2, &in)), ALFS_ENOENT);
        CHECK_EQ(remount(&r), 0);
    }
    alfs_obj_status_t st;
    alfs_obj_status(&r.ob, &st);
    CHECK_EQ(st.used, 2 * 40 + 30 + 300);
    CHECK_EQ(st.capacity, 2 * SECTOR - PAGE);
    DONE(&r);
    rig_close(&r);
}

/* Every log number listed, and those given, never twice. */
typedef struct {
    uint32_t n[256];
    int count;
} numbers_t;

static bool note(numbers_t *s, uint32_t v) {
    for (int i = 0; i < s->count; i++)
        if (s->n[i] == v)
            return false;
    if (s->count < 256)
        s->n[s->count++] = v;
    return true;
}

static void if_log_numbers_never_reused(void) {
    rig_t r;
    rig_open(&r, 128, 2);
    CHECK_EQ(setup(&r), 0);
    numbers_t seen = {{0}, 0};
    uint32_t last = 0;
    uint8_t row[ROW];
    make_row(1, row);
    for (int i = 0; i < 3; i++) {
        alfs_writer_t w;
        CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
        CHECK(w.log > last);
        CHECK(note(&seen, w.log));
        last = w.log;
        CHECK_EQ(put(&r, &w, row, ROW), 0);
        CHECK_EQ(lclose(&r, &w), 0);
    }
    CHECK(lclean_log(&r, last) >= 0); /* the newest erased */
    CHECK_EQ(remount(&r), 0);
    alfs_writer_t w;
    CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
    CHECK(w.log > last);
    CHECK(note(&seen, w.log));
    CHECK_EQ(lclose(&r, &w), 0);

    static const sim_pl_t torn[] = {PL_FLICKER, PL_METASTABLE, PL_SOMEBITS};
    for (int t = 0; t < 6; t++) {
        CHECK(lclean(&r) >= 0);
        if (t < 3) { /* the log's first claim, programmed by its open */
            sim_power_after(&r.sim, 0, torn[t]);
            CHECK(lopen(&r, &w, 0, 1, 0) != 0);
        } else { /* its first chunk */
            CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
            CHECK(note(&seen, w.log));
            sim_power_after(&r.sim, 0, torn[t - 3]);
            put(&r, &w, row, ROW);
            lsync(&r, &w);
        }
        CHECK(r.sim.off);
        sim_reboot(&r.sim);
        for (int m = 0; m < 8; m++) {
            CHECK_EQ(remount(&r), 0);
            alfs_log_info_t in;
            uint32_t after = 0;
            while (CALL(&r, alfs_log_list(&r.lg, after, &in)) == 0) {
                after = in.log;
                note(&seen, in.log); /* listed: may be one given already */
            }
            CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
            if (!note(&seen, w.log))
                printf("    torn %d, remount %d: log %u given twice\n", t, m, w.log);
            CHECK(seen.n[seen.count - 1] == w.log);
            CHECK_EQ(put(&r, &w, row, ROW), 0);
            CHECK_EQ(lclose(&r, &w), 0);
        }
    }
    DONE(&r);
    rig_close(&r);
}

static void if_log_streams(void) {
    rig_t r;
    rig_open(&r, 16, 2);
    CHECK_EQ(setup(&r), 0);
    static uint8_t a[6000], b[9000], got[16000];
    fill(a, sizeof(a), 1);
    fill(b, sizeof(b), 2);
    alfs_writer_t wa, wb;
    CHECK_EQ(lopen(&r, &wa, 0, 1, 0), 0);
    CHECK_EQ(lopen(&r, &wb, wa.log, 2, 1), 0);
    CHECK_EQ(wb.log, wa.log);
    uint32_t ia = 0, ib = 0, taken = 0;
    for (int i = 0; ia < sizeof(a) || ib < sizeof(b); i++) {
        uint32_t ka = sizeof(a) - ia < 22 ? sizeof(a) - ia : 22, kb = sizeof(b) - ib < 160 ? sizeof(b) - ib : 160;
        CHECK_EQ(put_rows(&r, &wa, a + ia, ka, 22, &taken), 0);
        CHECK_EQ(put_rows(&r, &wb, b + ib, kb, 160, &taken), 0);
        ia += ka;
        ib += kb;
        if (i % 7 == 6) {
            CHECK_EQ(lsync(&r, &wa), 0);
            CHECK_EQ(lsync(&r, &wb), 0);
        }
    }
    CHECK_EQ(lclose(&r, &wa), 0);
    CHECK_EQ(lclose(&r, &wb), 0);
    for (int pass = 0; pass < 2; pass++) {
        CHECK_EQ(slurp(&r, wa.log, 1, got, sizeof(got)), (int32_t)sizeof(a));
        CHECK(memcmp(got, a, sizeof(a)) == 0);
        CHECK_EQ(slurp(&r, wa.log, 2, got, sizeof(got)), (int32_t)sizeof(b));
        CHECK(memcmp(got, b, sizeof(b)) == 0);
        CHECK_EQ(slurp(&r, wa.log, 3, got, sizeof(got)), 0);
        CHECK_EQ(remount(&r), 0);
    }
    DONE(&r);
    rig_close(&r);
}

static void if_log_new_after_mount(void) {
    rig_t r;
    rig_open(&r, 16, 2);
    CHECK_EQ(setup(&r), 0);
    static uint8_t d[5000], got[6000];
    fill(d, sizeof(d), 4);
    alfs_writer_t w;
    CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
    uint32_t old = w.log;
    CHECK_EQ(put(&r, &w, d, sizeof(d)), 0);
    CHECK_EQ(lsync(&r, &w), 0);
    int32_t last = -1;
    claims_of(&r, old, &last);
    CHECK_EQ(remount(&r), 0); /* never closed */
    CHECK_EQ(lopen(&r, &w, old, 1, 0), ALFS_EINVAL);
    CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
    CHECK(w.log > old);
    CHECK_EQ(put(&r, &w, d, 100), 0);
    CHECK_EQ(lclose(&r, &w), 0);
    int32_t first = -1;
    CHECK_EQ(claims_of(&r, w.log, &first), 1);
    CHECK(first != last); /* a sector of its own */
    CHECK_EQ(slurp(&r, old, 1, got, sizeof(got)), (int32_t)sizeof(d));
    CHECK(memcmp(got, d, sizeof(d)) == 0);
    CHECK_EQ(slurp(&r, w.log, 1, got, sizeof(got)), 100);
    DONE(&r);
    rig_close(&r);
}

static void if_log_list(void) {
    rig_t r;
    rig_open(&r, 32, 2);
    CHECK_EQ(setup(&r), 0);
    static uint8_t d[30000];
    fill(d, sizeof(d), 5);
    static const uint32_t len[] = {100, 9000, 30000};
    uint32_t num[3];
    for (int i = 0; i < 3; i++) {
        alfs_writer_t w;
        CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
        CHECK_EQ(put(&r, &w, d, len[i]), 0);
        CHECK_EQ(lclose(&r, &w), 0);
        num[i] = w.log;
    }
    for (int pass = 0; pass < 2; pass++) {
        alfs_log_info_t in;
        uint32_t after = 0;
        for (int i = 0; i < 3; i++) {
            CHECK_EQ(CALL(&r, alfs_log_list(&r.lg, after, &in)), 0);
            CHECK_EQ(in.log, num[i]);
            CHECK_EQ(in.sectors, claims_of(&r, num[i], NULL));
            CHECK(in.sectors >= (len[i] + SECTOR - 1) / SECTOR);
            after = in.log;
        }
        CHECK_EQ(CALL(&r, alfs_log_list(&r.lg, after, &in)), ALFS_ENOENT);
        CHECK_EQ(remount(&r), 0);
    }
    DONE(&r);
    rig_close(&r);
}

static void if_log_refusals(void) {
    rig_t r;
    rig_open(&r, 16, 2);
    CHECK_EQ(setup(&r), 0);
    uint8_t d[100];
    fill(d, sizeof(d), 6);
    alfs_writer_t w, w2;
    CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
    uint32_t a = w.log;
    CHECK_EQ(put(&r, &w, d, sizeof(d)), 0);
    CHECK_EQ(lsync(&r, &w), 0);
    CHECK_EQ(LERASE(&r, a), ALFS_EOPEN);
    CHECK_EQ(lopen(&r, &w2, 0, 2, 1), ALFS_EOPEN);
    CHECK_EQ(LMOUNT(&r), ALFS_EOPEN);
    CHECK_EQ(lclose(&r, &w), 0);
    CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
    uint32_t b = w.log;
    CHECK_EQ(put(&r, &w, d, sizeof(d)), 0);
    CHECK_EQ(lclose(&r, &w), 0);
    alfs_reader_t rd;
    CHECK_EQ(CALL(&r, alfs_log_open_read(&r.lg, &rd, a, 1, r.blk[3], r.bsize)), 0);
    CHECK_EQ(LERASE(&r, b), ALFS_EOPEN);
    CHECK_EQ(LERASE(&r, 0), ALFS_EOPEN);
    CHECK_EQ(LMOUNT(&r), ALFS_EOPEN);
    CHECK_EQ(r.sim.c_ops, 0);
    CHECK_EQ(CALL(&r, alfs_log_close_read(&r.lg, &rd)), 0);
    CHECK(lclean_log(&r, a) >= 0);
    uint8_t got[200];
    CHECK_EQ(slurp(&r, a, 1, got, sizeof(got)), ALFS_ENOENT);
    CHECK_EQ(slurp(&r, b, 1, got, sizeof(got)), (int32_t)sizeof(d));
    CHECK_EQ(LMOUNT(&r), 0);
    DONE(&r);
    rig_close(&r);
}

static void basic_round_trip(void) {
    static const uint32_t sizes[] = {1, 21, 22, 255, 256, 4097, 20000};
    static uint8_t d[20000], got[20480];
    for (int b = 0; b < 2; b++)
        for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            rig_t r;
            rig_open(&r, 16, 2);
            r.bsize = b ? BLOCK_MAX : PAGE;
            CHECK_EQ(setup(&r), 0);
            fill(d, sizes[i], i);
            alfs_writer_t w;
            CHECK_EQ(lopen(&r, &w, 0, 0, 0), 0);
            CHECK_EQ(put(&r, &w, d, sizes[i]), 0);
            CHECK_EQ(lclose(&r, &w), 0);
            for (int pass = 0; pass < 2; pass++) {
                CHECK_EQ(slurp(&r, w.log, 0, got, sizeof(got)), (int32_t)sizes[i]);
                CHECK(memcmp(got, d, sizes[i]) == 0);
                CHECK_EQ(remount(&r), 0);
            }
            DONE(&r);
            rig_close(&r);
        }
}

/* ---- SPD -------------------------------------------------------------- */

/* W-CFG, step i: what it puts or deletes. */
typedef struct {
    const char *name;
    uint32_t size, gen; /* size 0: a delete */
} cfg_op_t;

static cfg_op_t cfg_step(int i) {
    static const cfg_op_t script[] = {
        {"cfg", 300, 1}, {"lua/main.lua", 4096, 1}, {"cfg2", 120, 1}, {"cfg", 300, 2},
        {"cfg2", 0, 0},  {"cfg", 300, 3},           {"cfg2", 120, 2}, {"lua/main.lua", 4096, 2},
    };
    cfg_op_t op = script[i % 8];
    if (op.size)
        op.gen += (uint32_t)(i / 8) * 10u;
    return op;
}

static int cfg_do(rig_t *r, cfg_op_t op) {
    static uint8_t d[4096];
    if (!op.size)
        return odelete(r, op.name);
    fill(d, op.size, op.gen);
    return oput(r, op.name, d, op.size);
}

static void spd_no_erase_outside_erase(void) {
    for (int rate = 0; rate < 2; rate++) {
        rig_t r;
        rig_open(&r, 64, 4);
        CHECK_EQ(LMOUNT(&r), 0);
        for (int i = 0; i < 8; i++)
            CHECK(LERASE(&r, 0) >= 0);
        CHECK_EQ(OFORMAT(&r), 0);
        run_t lg;
        run_init(&lg, 1 << 20);
        alfs_writer_t w;
        CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
        CHECK_EQ(log_run(&r, &w, &lg, rate ? 50 : 1, 120000, true), 0);
        CHECK_EQ(lclose(&r, &w), 0);
        for (int i = 0; i < 16; i++) {
            if (i % 2)
                CHECK(oclean(&r) >= 0); /* then the put may not erase */
            CHECK_EQ(cfg_do(&r, cfg_step(i)), 0);
        }
        free(lg.exp);
        DONE(&r);
        rig_close(&r);
    }
}

static void spd_write_burst_bounded(void) {
    for (int b = 0; b < 2; b++)
        for (int kind = 0; kind < 2; kind++) {
            rig_t r;
            rig_open(&r, 256, 2);
            r.bsize = b ? BLOCK_MAX : PAGE;
            CHECK_EQ(setup(&r), 0);
            alfs_writer_t w;
            CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
            static uint8_t d[HR_ROW];
            uint32_t each = kind ? HR_ROW : ROW, full = PAGE - 37 - each; /* the least a full chunk holds */
            for (uint32_t t = 0; t < 1500; t++) {
                for (uint32_t j = 0; j < 10; j++) { /* rows a 20 ms tick */
                    fill(d, each, t * 10 + j);
                    uint32_t before = w.used;
                    CHECK_EQ(CALL(&r, alfs_log_write(&r.lg, &w, d, each)), 0);
                    uint32_t x = before + 1 + each - w.used;
                    uint64_t bound = (x + full - 1) / full + 2 + 1;
                    if (r.sim.c_programs > bound)
                        printf("    row of %u: %llu programs for %u bytes\n", each,
                               (unsigned long long)r.sim.c_programs, x);
                    CHECK(r.sim.c_programs <= bound);
                }
                if (t % 50 == 49) {
                    uint32_t x = w.used;
                    CHECK_EQ(lsync(&r, &w), 0);
                    CHECK(r.sim.c_programs <= (x + full - 1) / full + 2 + 1);
                }
            }
            uint32_t x = w.used;
            CHECK_EQ(lclose(&r, &w), 0);
            CHECK(r.sim.c_programs <= (x + full - 1) / full + 2 + 1);
            DONE(&r);
            rig_close(&r);
        }
}

static void spd_enospc_without_erase(void) {
    rig_t r;
    rig_open(&r, 8, 2);
    CHECK_EQ(setup(&r), 0);
    static uint8_t d[40000], got[40000];
    fill(d, sizeof(d), 7);
    alfs_writer_t w;
    CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
    uint32_t old = w.log;
    CHECK_EQ(put(&r, &w, d, 3 * SECTOR), 0);
    CHECK_EQ(lclose(&r, &w), 0);
    CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
    uint32_t taken = 0;
    int rc = put_rows(&r, &w, d, sizeof(d), RUN_ROW, &taken);
    CHECK_EQ(rc, ALFS_ENOSPC);
    CHECK_EQ(r.sim.c_erases, 0);
    CHECK(taken < sizeof(d)); /* the refused row not taken */
    CHECK_EQ(lsync(&r, &w), ALFS_ENOSPC);
    CHECK_EQ(r.sim.c_erases, 0);
    CHECK(w.used > 0); /* held in the block */
    CHECK_EQ(LERASE(&r, old), ALFS_OK);
    CHECK_EQ(lclose(&r, &w), 0);
    CHECK_EQ(slurp(&r, w.log, 1, got, sizeof(got)), (int32_t)taken);
    CHECK(memcmp(got, d, taken) == 0);
    DONE(&r);
    rig_close(&r);
}

static void spd_mount_reads_bounded(void) {
    rig_t r;
    rig_open(&r, 128, 8);
    CHECK_EQ(setup(&r), 0);
    static uint8_t d[20000];
    fill(d, sizeof(d), 8);
    for (int i = 0; i < 4; i++) {
        alfs_writer_t w;
        CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
        CHECK_EQ(put(&r, &w, d, sizeof(d)), 0);
        CHECK_EQ(lclose(&r, &w), 0);
        CHECK_EQ(oput(&r, i % 2 ? "a" : "b", d, 1000u * (uint32_t)i), 0);
    }
    memset(&r.lg, 0, sizeof(r.lg));
    CHECK_EQ(LMOUNT(&r), 0);
    CHECK(r.sim.c_bytes_read <= 30u * 128u);
    CHECK_EQ(r.sim.c_ops, 0);
    memset(&r.ob, 0, sizeof(r.ob));
    CHECK_EQ(OMOUNT(&r), 0);
    CHECK(r.sim.c_bytes_read <= 2u * 29u);
    CHECK_EQ(r.sim.c_ops, 0);
    for (int i = 0; i < 100; i++)
        CHECK_EQ(kv_do(&r, kv_step(i, 150)) <= 0, 1);
    memset(&r.cg, 0, sizeof(r.cg));
    CHECK_EQ(CMOUNT(&r), 0);
    CHECK(r.sim.c_bytes_read <= 30u * CFG_SECTORS);
    CHECK_EQ(r.sim.c_ops, 0);
    DONE(&r);
    rig_close(&r);
}

static void spd_wlog_longest_stall(void) {
    rig_t r;
    rig_open(&r, 256, 2);
    CHECK_EQ(setup(&r), 0);
    run_t lg;
    run_init(&lg, 1 << 20);
    alfs_writer_t w;
    CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
    CHECK_EQ(log_run(&r, &w, &lg, 50, 600000, false), 0);
    CHECK_EQ(lclose(&r, &w), 0);
    CHECK(longest_stall <= SIM_T_PROG_US);
    free(lg.exp);
    DONE(&r);
    rig_close(&r);
}

/* The bytes read by the writes and syncs of the same 30 s of W-HR. */
static uint64_t hr_reads(uint32_t sectors) {
    rig_t r;
    rig_open(&r, sectors, 2);
    if (setup(&r)) {
        rig_close(&r);
        return 0;
    }
    alfs_writer_t w;
    lopen(&r, &w, 0, 1, 0);
    static uint8_t d[HR_ROW];
    uint64_t total = 0;
    for (uint32_t t = 0; t < 200; t++) { /* 98 kB: within 32 sectors */
        for (uint32_t j = 0; j < 10; j++) {
            fill(d, sizeof(d), t * 10 + j);
            CALL(&r, alfs_log_write(&r.lg, &w, d, sizeof(d)));
            total += r.sim.c_bytes_read;
        }
        if (t % 50 == 49) {
            lsync(&r, &w);
            total += r.sim.c_bytes_read;
        }
    }
    rig_close(&r);
    return total;
}

static void spd_cpu_linear(void) {
    uint64_t small = hr_reads(32), large = hr_reads(2048);
    CHECK(small > 0);
    if (small != large)
        printf("    reads: %llu on 32 sectors, %llu on 2048\n", (unsigned long long)small,
               (unsigned long long)large);
    CHECK_EQ(small, large);
}

static void spd_read_next_bounded(void) {
    rig_t r;
    rig_open(&r, 64, 2);
    CHECK_EQ(setup(&r), 0);
    /* Dirty sectors in the way: every fifth one's stamp broken. */
    uint32_t skipped = 0;
    for (uint32_t k = 2; k < 40; k += 5) {
        sim_flip(&r.sim, r.lcfg.start + k * SECTOR, 1);
        skipped++;
    }
    CHECK_EQ(remount(&r), 0);
    static uint8_t d[100000], got[100000];
    fill(d, sizeof(d), 10);
    alfs_writer_t w;
    CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
    CHECK_EQ(put(&r, &w, d, sizeof(d)), 0);
    CHECK_EQ(lclose(&r, &w), 0);
    uint32_t sectors = claims_of(&r, w.log, NULL);
    alfs_reader_t rd;
    CHECK_EQ(CALL(&r, alfs_log_open_read(&r.lg, &rd, w.log, 1, r.blk[3], r.bsize)), 0);
    uint64_t headers = 0; /* but the read that finds the end, which scans the area */
    uint32_t n = 0;
    for (;;) {
        int32_t k = (int32_t)CALL(&r, alfs_log_read(&r.lg, &rd, got + n, 300));
        CHECK(k >= 0);
        if (!k)
            break;
        headers += r.seen.headers;
        n += (uint32_t)k;
    }
    CHECK_EQ(n, sizeof(d));
    CHECK(memcmp(got, d, sizeof(d)) == 0);
    if (headers > sectors + skipped + 2)
        printf("    %llu headers read for %u sectors and %u between\n", (unsigned long long)headers,
               sectors, skipped);
    CHECK(headers <= sectors + skipped + 2);
    CHECK_EQ(CALL(&r, alfs_log_close_read(&r.lg, &rd)), 0);
    DONE(&r);
    rig_close(&r);
}

/* ---- LIFE ------------------------------------------------------------- */

/*
 * W-RING: W-LOG at 50 rows a second, a new log every `every` rows, the
 * oldest erased while fewer than two logs' worth of sectors are ready, an
 * erase step every 20 ms, until every sector has been erased `laps` times
 * or `max_ms` passes. 0, or the first error.
 */
static int ring(rig_t *r, uint32_t every, uint32_t laps, uint64_t max_ms) {
    uint32_t per_sector = SECTOR / PAGE * ((PAGE - CHUNK) / (ROW + 1)) - 1; /* rows, at least */
    uint32_t sectors = r->lcfg.size / SECTOR, per_log = every / per_sector + 2;
    uint8_t rows[10 * ROW];
    uint32_t seq = 0, in_log = 0;
    alfs_writer_t w;
    if (lopen(r, &w, 0, 1, 0))
        return -1;
    for (uint64_t t = 20; t < max_ms; t += 20) {
        alfs_log_status_t st;
        alfs_log_status(&r->lg, &st);
        uint32_t oldest = 0;
        alfs_log_info_t in;
        if (st.ready < 2 * per_log && CALL(r, alfs_log_list(&r->lg, 0, &in)) == 0 && in.log != w.log)
            oldest = in.log;
        int rc = LERASE(r, oldest);
        if (rc < 0 && rc != ALFS_EOPEN)
            return rc;
        if (t % 200)
            continue;
        for (int i = 0; i < 10; i++)
            make_row(seq++, rows + i * ROW);
        uint32_t taken = 0;
        rc = put_rows(r, &w, rows, sizeof(rows), ROW, &taken);
        if (rc && rc != ALFS_ENOSPC)
            return rc;
        in_log += 10;
        if (t % 1000 == 0 && lsync(r, &w) < 0 && !r->sim.violations)
            continue;
        if (in_log >= every) {
            if (lclose(r, &w) == 0) {
                if (lopen(r, &w, 0, 1, 0))
                    return -2;
                in_log = 0;
            }
        }
        uint32_t least = UINT32_MAX;
        for (uint32_t k = 0; k < sectors; k++) {
            uint32_t idx = r->lcfg.start / SECTOR + k;
            if (r->sim.bad[idx] == BAD_NONE && r->sim.wear[idx] < least)
                least = r->sim.wear[idx];
        }
        if (least >= laps)
            break;
    }
    lclose(r, &w);
    return 0;
}

static void life_erase_only_dirty(void) {
    rig_t r;
    rig_open(&r, 32, 4);
    CHECK_EQ(setup(&r), 0);
    uint64_t before = r.sim.ready_erases;
    CHECK_EQ(ring(&r, 3000, 3, 10000000), 0);
    CHECK_EQ(r.sim.ready_erases, before);
    for (int i = 0; i < 12; i++) {
        bool was = spare_ready(&r);
        uint64_t e0 = r.sim.erases;
        CHECK_EQ(cfg_do(&r, cfg_step(i)), 0);
        CHECK_EQ(r.sim.erases - e0, was ? 0u : 2u); /* two sectors a half */
        if (i % 3 == 0)
            CHECK(oclean(&r) >= 0);
    }
    DONE(&r);
    rig_close(&r);
}

/* LIFE-02's bound for the call just made, which took x bytes from the
 * block. */
static uint64_t life02_bound(const rig_t *r, uint64_t x) {
    return x + CHUNK * r->sim.c_programs + LDATA * r->seen.claims + 12u * r->seen.zeroes;
}

static void life_write_amplification(void) {
    for (int kind = 0; kind < 3; kind++) {
        rig_t r;
        rig_open(&r, 512, 2);
        CHECK_EQ(setup(&r), 0);
        alfs_writer_t w;
        CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
        static uint8_t d[HR_ROW];
        uint64_t programmed = 0, bound = 0;
        uint32_t rows = kind == 0 ? 1 : kind == 1 ? 50 : 500, len = kind == 2 ? HR_ROW : ROW; /* a sync's */
        for (uint32_t s = 0; s < (kind == 2 ? 60u : 600u); s++) { /* W-HR: a minute */
            for (uint32_t i = 0; i < rows; i++) {
                fill(d, len, s * rows + i);
                uint64_t b0 = r.sim.bytes_programmed;
                uint32_t before = w.used;
                CHECK_EQ(CALL(&r, alfs_log_write(&r.lg, &w, d, len)), 0);
                programmed += r.sim.bytes_programmed - b0;
                bound += life02_bound(&r, before + 1u + len - w.used);
            }
            uint64_t b0 = r.sim.bytes_programmed, x = w.used;
            CHECK_EQ(lsync(&r, &w), 0);
            programmed += r.sim.bytes_programmed - b0;
            bound += life02_bound(&r, x);
        }
        if (programmed > bound)
            printf("    %u rows a sync: %llu programmed, bound %llu\n", rows, (unsigned long long)programmed,
                   (unsigned long long)bound);
        CHECK(programmed <= bound);
        CHECK_EQ(lclose(&r, &w), 0);
        DONE(&r);
        rig_close(&r);
    }
}

static void life_wlog_erases(void) {
    rig_t r;
    rig_open(&r, 256, 2);
    CHECK_EQ(setup(&r), 0);
    run_t lg;
    run_init(&lg, 1 << 20);
    alfs_writer_t w;
    CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
    uint64_t p0 = r.sim.bytes_programmed, e0 = r.sim.erases;
    CHECK_EQ(log_run(&r, &w, &lg, 50, 600000, false), 0);
    CHECK_EQ(lclose(&r, &w), 0);
    uint64_t filled = (r.sim.bytes_programmed - p0 + SECTOR - 1) / SECTOR;
    CHECK(lclean_log(&r, w.log) >= 0);
    uint64_t erases = r.sim.erases - e0;
    printf("    W-LOG: %llu sector erases for %llu sectors filled\n", (unsigned long long)erases,
           (unsigned long long)filled);
    CHECK(erases <= filled + 1);
    free(lg.exp);
    DONE(&r);
    rig_close(&r);
}

static void life_wear_level(void) {
    rig_t r;
    rig_open(&r, 32, 2);
    CHECK_EQ(setup(&r), 0);
    CHECK_EQ(ring(&r, 3000, 10, 100000000), 0);
    uint32_t lo = UINT32_MAX, hi = 0;
    for (uint32_t k = 0; k < 32; k++) {
        uint32_t wv = r.sim.wear[r.lcfg.start / SECTOR + k];
        lo = wv < lo ? wv : lo;
        hi = wv > hi ? wv : hi;
    }
    if (hi - lo > 2)
        printf("    wear %u to %u\n", lo, hi);
    CHECK(lo >= 10);
    CHECK(hi - lo <= 2);
    DONE(&r);
    rig_close(&r);
}

/* ---- SAFE ------------------------------------------------------------- */

/*
 * What a stream may read after a power loss (SAFE-01): a prefix of what was
 * written, at least what was synced. Bytes never synced may be lost, or
 * come back at a later mount in their place.
 */
typedef struct {
    const uint8_t *exp;
    uint32_t synced, len;
} expect_t;

static bool reads_as_expected(const uint8_t *got, int32_t n, const expect_t *e) {
    if (n < 0 && e->synced == 0)
        return true; /* nothing synced: the log may be gone */
    return n >= (int32_t)e->synced && n <= (int32_t)e->len && !memcmp(got, e->exp, (size_t)n);
}

/* Remounts, erases until clean, and reads both logs: twice. */
static void reread(rig_t *r, uint32_t old_n, const expect_t *old, uint32_t new_n, const expect_t *now,
                   uint8_t *got, const char *what) {
    for (int round = 0; round < 2; round++) {
        CHECK_EQ(remount(r), 0);
        CHECK(lclean(r) >= 0);
        for (int f = 0; f < 2; f++) {
            const expect_t *e = f ? now : old;
            int32_t n = slurp(r, f ? new_n : old_n, 1, got, 64 * 1024);
            bool ok = reads_as_expected(got, n, e);
            if (!ok)
                printf("    %s, round %d, %s log: read %d of %u (synced %u)\n", what, round, f ? "new" : "old",
                       n, e->len, e->synced);
            CHECK(ok);
        }
    }
}

/*
 * Power lost at operation k of a logging run that also erases, with each
 * behaviour; then a remount and a read, a new log written (some cases lose
 * power a second time there), and remounts with erases and reads of both.
 * Sets *past when k is past the run's last operation.
 */
static void powerloss_case(sim_pl_t pl, int64_t k, bool twice, int *past) {
    rig_t r;
    rig_open(&r, 10, 2);
    *past = 1;
    CHECK_EQ(LMOUNT(&r), 0);
    for (int i = 0; i < 3; i++)
        CHECK(LERASE(&r, 0) >= 0);
    *past = 0;
    run_t lg;
    run_init(&lg, 64 * 1024);
    alfs_writer_t w;
    CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
    sim_power_after(&r.sim, k, pl);
    int rc = log_run(&r, &w, &lg, 50, 20000, true);
    if (!r.sim.off) {
        free(lg.exp);
        rig_close(&r);
        *past = 1;
        CHECK_EQ(rc, 0);
        return;
    }
    uint32_t old_n = w.log;
    expect_t old = {malloc(lg.len + 1), lg.synced, lg.len};
    memcpy((uint8_t *)old.exp, lg.exp, lg.len);
    sim_reboot(&r.sim);
    CHECK_EQ(remount(&r), 0);

    uint8_t *got = malloc(64 * 1024);
    char what[64];
    snprintf(what, sizeof(what), "pl %d k %lld", pl, (long long)k);
    int32_t n = slurp(&r, old_n, 1, got, 64 * 1024);
    if (!reads_as_expected(got, n, &old))
        printf("    %s: read %d, synced %u, written %u\n", what, n, old.synced, old.len);
    CHECK(reads_as_expected(got, n, &old));

    /* A new log after the restart. */
    lg.len = lg.synced = 0;
    if (twice)
        sim_power_after(&r.sim, (int64_t)(sim_rand(&r.sim) % 40), pl);
    w.log = 0;
    rc = lopen(&r, &w, 0, 1, 0);
    if (!rc)
        rc = log_run(&r, &w, &lg, 50, 6000, true);
    if (!r.sim.off) {
        CHECK_EQ(rc, 0);
        rc = lclose(&r, &w); /* the second loss may land here */
        if (!r.sim.off) {
            CHECK_EQ(rc, 0);
            lg.synced = lg.len;
        }
    }
    sim_reboot(&r.sim); /* a second loss the run outlasted does not happen */
    expect_t now = {lg.exp, lg.synced, lg.len};
    reread(&r, old_n, &old, w.log, &now, got, what);
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

/* got[n] is exp[len] with at most one run cut out. */
static bool one_cut(const uint8_t *got, uint32_t n, const uint8_t *exp, uint32_t len) {
    if (n > len)
        return false;
    uint32_t head = 0;
    while (head < n && got[head] == exp[head])
        head++;
    uint32_t cut = len - n;
    return !memcmp(got + head, exp + head + cut, n - head);
}

static void safe_no_false_data(void) {
    for (int trial = 0; trial < 200; trial++) {
        rig_t r;
        rig_open(&r, 16, 2);
        CHECK_EQ(setup(&r), 0);
        static uint8_t a[3000], b[3000], got[4000];
        fill(a, sizeof(a), (uint32_t)trial);
        fill(b, sizeof(b), (uint32_t)trial + 1000);
        alfs_writer_t wa, wb;
        CHECK_EQ(lopen(&r, &wa, 0, 1, 0), 0);
        CHECK_EQ(lopen(&r, &wb, wa.log, 2, 1), 0);
        for (uint32_t i = 0; i < sizeof(a); i += 100) {
            CHECK_EQ(put(&r, &wa, a + i, 100), 0);
            CHECK_EQ(put(&r, &wb, b + i, 100), 0);
            if (i % 500 == 0) {
                CHECK_EQ(lsync(&r, &wa), 0);
                CHECK_EQ(lsync(&r, &wb), 0);
            }
        }
        CHECK_EQ(lclose(&r, &wa), 0);
        CHECK_EQ(lclose(&r, &wb), 0);
        /* One bit flipped somewhere in the log's chunks. */
        int32_t first = -1, last = -1;
        claims_of(&r, wa.log, &last);
        for (uint32_t k = 0; k < 16; k++) {
            const uint8_t *c = r.sim.mem + r.lcfg.start + k * SECTOR + LSTAMP;
            if (get32le(c) == wa.log && get32le(c + 8) == crc32_ref(c, 8)) {
                first = (int32_t)k;
                break;
            }
        }
        uint32_t span = (uint32_t)(last - first + 1) * SECTOR, at;
        for (;;) {
            at = sim_rand(&r.sim) % span;
            if (at % SECTOR >= LDATA && r.sim.mem[r.lcfg.start + (uint32_t)first * SECTOR + at] != 0xFF)
                break;
        }
        sim_flip(&r.sim, r.lcfg.start + (uint32_t)first * SECTOR + at, (uint8_t)(1u << (sim_rand(&r.sim) % 8)));
        alfs_log_status_t st;
        alfs_log_status(&r.lg, &st);
        uint32_t fails0 = st.n.check_failures;
        for (int s = 0; s < 2; s++) {
            const uint8_t *exp = s ? b : a;
            int32_t n = slurp(&r, wa.log, (uint8_t)(1 + s), got, sizeof(got));
            CHECK(n >= 0);
            CHECK(one_cut(got, (uint32_t)n, exp, sizeof(a)));
            CHECK(sizeof(a) - (uint32_t)n <= PAGE); /* a page's chunks at most */
            CHECK_EQ((sizeof(a) - (uint32_t)n) % 100, 0); /* in whole rows */
        }
        alfs_log_status(&r.lg, &st);
        CHECK(st.n.check_failures - fails0 <= 2);
        DONE(&r);
        rig_close(&r);
    }
}

static void safe_write_only_verified_ready(void) {
    rig_t r;
    rig_open(&r, 8, 4);
    sim_mkbad(&r.sim, r.lcfg.start / SECTOR + 3, BAD_ERASENOOP);
    CHECK_EQ(setup(&r), 0);
    alfs_log_status_t st;
    alfs_log_status(&r.lg, &st);
    CHECK_EQ(st.bad, 1);
    static uint8_t d[20000], got[20480];
    fill(d, sizeof(d), 11);
    alfs_writer_t w;
    CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
    CHECK_EQ(put(&r, &w, d, sizeof(d)), 0);
    CHECK_EQ(lclose(&r, &w), 0);
    CHECK_EQ(r.sim.sector_progs[r.lcfg.start / SECTOR + 3], 0);
    CHECK_EQ(slurp(&r, w.log, 1, got, sizeof(got)), (int32_t)sizeof(d));

    /* The object area: an ERASENOOP sector in the spare half. */
    uint8_t c[300];
    fill(c, sizeof(c), 1);
    CHECK_EQ(oput(&r, "cfg", c, sizeof(c)), 0);
    alfs_obj_status_t os;
    alfs_obj_status(&r.ob, &os);
    uint32_t spare = os.seq % 2 ? 1 : 0; /* commit 1 went to half 0, and so on */
    sim_mkbad(&r.sim, r.ocfg.start / SECTOR + spare * 2 + 1, BAD_ERASENOOP);
    sim_fill_random(&r.sim, r.ocfg.start + (spare * 2 + 1) * SECTOR, SECTOR);
    uint8_t c2[300];
    fill(c2, sizeof(c2), 2);
    CHECK_EQ(oput(&r, "cfg", c2, sizeof(c2)), ALFS_EPROG);
    CHECK(reads_as(&r, "cfg", c, sizeof(c)));
    CHECK_EQ(remount(&r), 0);
    CHECK(reads_as(&r, "cfg", c, sizeof(c)));
    DONE(&r);
    rig_close(&r);
}

/* got[n] is W-LOG rows end to end, each whole. */
static bool rows_whole(const uint8_t *got, uint32_t n) {
    if (n % ROW)
        return false;
    for (uint32_t i = 0; i < n; i += ROW)
        if (get32le(got + i + ROW - 4) != crc32_ref(got + i, ROW - 4))
            return false;
    return true;
}

/* got[n] is exp[len] with runs cut out, as greedy matching finds them. */
static bool runs_cut(const uint8_t *got, uint32_t n, const uint8_t *exp, uint32_t len) {
    uint32_t j = 0;
    for (uint32_t i = 0; i < n; i++) {
        while (j < len && exp[j] != got[i])
            j++;
        if (j == len)
            return false;
        j++;
    }
    return true;
}

static void safe_prog_failure(void) {
    static const sim_bad_t modes[] = {BAD_PROGERROR, BAD_PROGNOOP, BAD_PROGFLIP};
    for (int m = 0; m < 3; m++) {
        rig_t r;
        rig_open(&r, 16, 2);
        CHECK_EQ(setup(&r), 0);
        run_t lg;
        run_init(&lg, 1 << 18);
        alfs_writer_t w;
        CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
        CHECK_EQ(log_run(&r, &w, &lg, 50, 5000, false), 0);
        int32_t k = -1;
        claims_of(&r, w.log, &k);
        sim_mkbad(&r.sim, r.lcfg.start / SECTOR + (uint32_t)k, modes[m]); /* the sector being written */
        CHECK_EQ(log_run(&r, &w, &lg, 50, 20000, false), 0);
        CHECK_EQ(lclose(&r, &w), 0);
        static uint8_t got[1 << 18];
        int32_t n = slurp(&r, w.log, 1, got, sizeof(got));
        CHECK(n > 0);
        alfs_log_status_t st;
        alfs_log_status(&r.lg, &st);
        if ((uint32_t)n + st.n.bytes_lost != lg.len)
            printf("    mode %d: read %d + lost %u != written %u\n", modes[m], n, st.n.bytes_lost, lg.len);
        CHECK_EQ((uint32_t)n + st.n.bytes_lost, lg.len);
        CHECK(st.n.prog_failures >= 1);
        CHECK(runs_cut(got, (uint32_t)n, lg.exp, lg.len));
        CHECK(rows_whole(got, (uint32_t)n));
        free(lg.exp);
        DONE(&r);
        rig_close(&r);
    }
}

static void safe_bad_sector(void) {
    static const sim_bad_t modes[] = {BAD_ERASEERROR, BAD_ERASENOOP};
    for (int m = 0; m < 2; m++) {
        rig_t r;
        rig_open(&r, 8, 2);
        uint32_t bad = r.lcfg.start / SECTOR + 5;
        sim_mkbad(&r.sim, bad, modes[m]);
        CHECK_EQ(setup(&r), 0);
        alfs_log_status_t st;
        alfs_log_status(&r.lg, &st);
        CHECK_EQ(st.bad, 1);
        CHECK_EQ(st.ready, 7);
        CHECK_EQ(remount(&r), 0);
        alfs_log_status(&r.lg, &st);
        CHECK_EQ(st.dirty, 1);
        CHECK(lclean(&r) >= 0);
        alfs_log_status(&r.lg, &st);
        CHECK_EQ(st.bad, 1);
        CHECK_EQ(r.sim.sector_progs[bad], 0);
        DONE(&r);
        rig_close(&r);
    }
    {
        rig_t r;
        rig_open(&r, 32, 2);
        r.sim.wear_limit = 6;
        CHECK_EQ(setup(&r), 0);
        CHECK_EQ(ring(&r, 3000, 20, 3000000), 0);
        alfs_log_status_t st;
        alfs_log_status(&r.lg, &st);
        CHECK(st.bad > 0);
        DONE(&r);
        rig_close(&r);
    }
    {
        rig_t r;
        rig_open(&r, 4, 4);
        CHECK_EQ(setup(&r), 0);
        uint8_t c[300], c2[300];
        fill(c, sizeof(c), 1);
        fill(c2, sizeof(c2), 2);
        CHECK_EQ(oput(&r, "cfg", c, sizeof(c)), 0);
        CHECK_EQ(oput(&r, "lua", c2, sizeof(c2)), 0);
        alfs_obj_status_t os;
        alfs_obj_status(&r.ob, &os);
        uint32_t spare = os.seq % 2 ? 1 : 0;
        sim_mkbad(&r.sim, r.ocfg.start / SECTOR + spare * 2, BAD_ERASEERROR);
        for (int i = 0; i < 3; i++) {
            CHECK_EQ(oput(&r, "cfg", c2, sizeof(c2)), ALFS_EPROG);
            CHECK(reads_as(&r, "cfg", c, sizeof(c)));
            CHECK(reads_as(&r, "lua", c2, sizeof(c2)));
        }
        alfs_obj_status(&r.ob, &os);
        CHECK(os.n.erase_failures >= 3);
        DONE(&r);
        rig_close(&r);
    }
}

static void safe_read_retry(void) {
    rig_t r;
    rig_open(&r, 16, 4);
    CHECK_EQ(setup(&r), 0);
    static uint8_t d[9000], got[9000];
    fill(d, sizeof(d), 12);
    alfs_writer_t w;
    CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
    CHECK_EQ(put(&r, &w, d, sizeof(d)), 0);
    CHECK_EQ(lclose(&r, &w), 0);
    CHECK_EQ(oput(&r, "cfg", d, 2000), 0);

    memset(&r.lg, 0, sizeof(r.lg));
    r.sim.read_fail_once = 1;
    CHECK_EQ(LMOUNT(&r), ALFS_EIO);
    CHECK_EQ(LMOUNT(&r), 0);
    memset(&r.ob, 0, sizeof(r.ob));
    r.sim.read_fail_once = 1;
    CHECK_EQ(OMOUNT(&r), ALFS_EIO);
    CHECK_EQ(OMOUNT(&r), 0);

    alfs_reader_t rd;
    r.sim.read_fail_once = 1;
    CHECK_EQ(CALL(&r, alfs_log_open_read(&r.lg, &rd, w.log, 1, r.blk[3], r.bsize)), ALFS_EIO);
    CHECK_EQ(CALL(&r, alfs_log_open_read(&r.lg, &rd, w.log, 1, r.blk[3], r.bsize)), 0);
    uint32_t n = 0;
    for (int i = 0; n < sizeof(d); i++) {
        if (i % 3 == 1)
            r.sim.read_fail_once = 1;
        int32_t k = (int32_t)CALL(&r, alfs_log_read(&r.lg, &rd, got + n, 300));
        if (k == ALFS_EIO)
            continue;
        CHECK(k > 0);
        n += (uint32_t)k;
    }
    CHECK(memcmp(got, d, sizeof(d)) == 0);
    CHECK_EQ(CALL(&r, alfs_log_close_read(&r.lg, &rd)), 0);

    alfs_get_t g;
    r.sim.read_fail_once = 1;
    CHECK_EQ(CALL(&r, alfs_obj_open(&r.ob, &g, "cfg")), ALFS_EIO);
    CHECK_EQ(CALL(&r, alfs_obj_open(&r.ob, &g, "cfg")), 0);
    n = 0;
    for (int i = 0; n < 2000; i++) {
        if (i % 2)
            r.sim.read_fail_once = 1;
        int32_t k = (int32_t)CALL(&r, alfs_obj_read(&r.ob, &g, got + n, 300));
        if (k == ALFS_EIO)
            continue;
        CHECK(k > 0);
        n += (uint32_t)k;
    }
    CHECK(memcmp(got, d, 2000) == 0);
    CHECK_EQ(CALL(&r, alfs_obj_close(&r.ob, &g)), 0);
    DONE(&r);
    rig_close(&r);
}

static void safe_foreign_area(void) {
    for (int blank = 0; blank < 2; blank++) {
        rig_t r;
        rig_open(&r, 16, 4);
        if (blank)
            memset(r.sim.mem + SECTOR, 0xFF, r.sim.chip_size - 2 * SECTOR);
        CHECK_EQ(LMOUNT(&r), 0);
        CHECK_EQ(r.sim.c_ops, 0);
        alfs_log_status_t st;
        alfs_log_status(&r.lg, &st);
        CHECK_EQ(st.ready, 0);
        CHECK_EQ(st.dirty, 16);
        alfs_writer_t w;
        CHECK_EQ(lopen(&r, &w, 0, 1, 0), ALFS_ENOSPC); /* a log takes a ready sector at once */
        CHECK_EQ(r.sim.c_ops, 0);
        CHECK_EQ(OMOUNT(&r), ALFS_ENOFS);
        CHECK_EQ(r.sim.c_ops, 0);
        CHECK_EQ(cload(&r), 0); /* no log: no settings */
        alfs_log_status(&r.cg, &st);
        CHECK_EQ(st.ready, 0);
        CHECK_EQ(st.dirty, CFG_SECTORS);
        CHECK_EQ(r.sim.ops, 0);
        uint8_t got[8];
        CHECK_EQ(kget(&r, "k0", got, sizeof(got)), ALFS_ENOENT);
        DONE(&r);
        rig_close(&r);
    }
    rig_t r;
    rig_open(&r, 16, 4);
    r.lcfg.size = 17 * SECTOR + SECTOR; /* past the chip */
    CHECK_EQ(LMOUNT(&r), ALFS_EGEOMETRY);
    r.ocfg.chip_size = 4 * SECTOR;
    CHECK_EQ(OMOUNT(&r), ALFS_EGEOMETRY);
    CHECK_EQ(OFORMAT(&r), ALFS_EGEOMETRY);
    CHECK_EQ(r.sim.ops, 0);
    DONE(&r);
    rig_close(&r);
}

/* A log or half header with random fields under a valid stamp. */
static void plant(rig_t *r, uint32_t addr, bool log) {
    uint8_t h[32];
    for (int i = 0; i < 32; i++)
        h[i] = (uint8_t)sim_rand(&r->sim);
    if (log) {
        memcpy(h, "alfl", 4);
        h[4] = 5;
        h[5] = 12;
        h[6] = 8;
        h[7] = 0;
        h[8] = (uint8_t)(r->lcfg.size / SECTOR);
        h[9] = 0;
        uint32_t c = crc32_ref(h, 14);
        memcpy(h + 14, &c, 4);
        if (sim_rand(&r->sim) % 2) {
            c = crc32_ref(h + 18, 8);
            memcpy(h + 26, &c, 4);
        }
    } else {
        memcpy(h, "alfo", 4);
        h[4] = 5;
        h[5] = 12;
        h[6] = 8;
        h[7] = 0;
        h[8] = (uint8_t)(r->ocfg.size / SECTOR / 2);
        h[9] = h[10] = h[11] = 0;
        uint32_t c = crc32_ref(h, 12);
        memcpy(h + 12, &c, 4);
        if (sim_rand(&r->sim) % 2) {
            uint32_t end = PAGE + sim_rand(&r->sim) % (SECTOR - PAGE);
            memcpy(h + OCOMMIT + 4, &end, 4);
            c = crc32_ref(h + OCOMMIT, 8);
            memcpy(h + OCOMMIT + 8, &c, 4);
        }
    }
    memcpy(r->sim.mem + addr, h, sizeof(h));
}

static void safe_fuzz(void) {
    for (int trial = 0; trial < 300; trial++) {
        rig_t r;
        rig_open(&r, 16, 4);
        CHECK_EQ(setup(&r), 0);
        static uint8_t d[12000], got[16384];
        fill(d, sizeof(d), (uint32_t)trial);
        for (int i = 0; i < 3; i++) {
            alfs_writer_t w;
            if (lopen(&r, &w, 0, (uint8_t)i, 0) == 0) {
                put(&r, &w, d, 3000u * (uint32_t)(i + 1));
                lclose(&r, &w);
            }
            oput(&r, i ? "a" : "b", d, 700u * (uint32_t)i);
        }
        uint32_t lo = SECTOR, hi = r.sim.chip_size - SECTOR;
        switch (trial % 4) {
        case 0: /* bits flipped */
            for (int i = 0; i < 40; i++)
                sim_flip(&r.sim, lo + sim_rand(&r.sim) % (hi - lo), (uint8_t)(1u << (sim_rand(&r.sim) % 8)));
            break;
        case 1: /* headers randomised under valid stamps */
            for (uint32_t k = 0; k < 16; k++)
                if (sim_rand(&r.sim) % 3 == 0)
                    plant(&r, r.lcfg.start + k * SECTOR, true);
            plant(&r, r.ocfg.start + (sim_rand(&r.sim) % 2) * 2 * SECTOR, false);
            break;
        case 2: /* headers copied from one sector to another */
            for (int i = 0; i < 6; i++) {
                uint32_t a = sim_rand(&r.sim) % 16, b = sim_rand(&r.sim) % 16;
                memcpy(r.sim.mem + r.lcfg.start + b * SECTOR, r.sim.mem + r.lcfg.start + a * SECTOR, 64);
            }
            memcpy(r.sim.mem + r.ocfg.start + 2 * SECTOR, r.sim.mem + r.ocfg.start, 64);
            break;
        default: /* everything random but one stamp */
            sim_fill_random(&r.sim, lo, hi - lo);
            plant(&r, r.lcfg.start + (sim_rand(&r.sim) % 16) * SECTOR, true);
            plant(&r, r.ocfg.start, false);
            break;
        }
        r.sim.violations = 0; /* the planted bytes are not alfs's programs */
        memset(&r.lg, 0, sizeof(r.lg));
        memset(&r.ob, 0, sizeof(r.ob));
        if (alfs_log_mount(&r.lg, &r.lcfg, &r.drv, r.lscratch) == 0) {
            alfs_log_info_t in;
            uint32_t after = 0;
            for (int i = 0; i < 64 && alfs_log_list(&r.lg, after, &in) == 0; i++) {
                after = in.log;
                for (uint8_t t = 0; t < 3; t++) {
                    alfs_reader_t rd;
                    if (alfs_log_open_read(&r.lg, &rd, in.log, t, r.blk[3], r.bsize) == 0) {
                        for (int j = 0; j < 400 && alfs_log_read(&r.lg, &rd, got, 300) > 0; j++)
                            ;
                        alfs_log_read(&r.lg, &rd, got, 1); /* a row it cannot hold */
                        alfs_log_close_read(&r.lg, &rd);
                    }
                }
            }
            alfs_writer_t w;
            if (alfs_log_open(&r.lg, &w, 0, 1, r.blk[0], r.bsize) == 0) {
                for (uint32_t i = 0; i < 2000; i += 100)
                    alfs_log_write(&r.lg, &w, d + i, 100);
                alfs_log_close(&r.lg, &w);
            }
            alfs_log_erase(&r.lg, after, 4);
            for (int i = 0; i < 40 && alfs_log_erase(&r.lg, 0, 1) == ALFS_OK; i++)
                ;
        }
        if (alfs_obj_mount(&r.ob, &r.ocfg, &r.drv, r.oscratch) == 0) {
            alfs_obj_info_t in;
            for (uint32_t i = 0; i < 64 && alfs_obj_list(&r.ob, i, &in) == 0; i++) {
                alfs_get_t g;
                if (alfs_obj_open(&r.ob, &g, in.name) == 0) {
                    for (int j = 0; j < 400 && alfs_obj_read(&r.ob, &g, got, 300) > 0; j++)
                        ;
                    alfs_obj_close(&r.ob, &g);
                }
            }
            alfs_put_t p;
            if (alfs_obj_put(&r.ob, &p, "new", 100) == 0) {
                alfs_obj_write(&r.ob, &p, d, 100);
                alfs_obj_commit(&r.ob, &p);
            }
            alfs_obj_delete(&r.ob, "a");
        }
        CHECK(gaps_kept(&r));
        rig_close(&r);
    }
}

/* Every chunk a log reads is one written to it: a prefix-free subset. */
static void safe_log_erase(void) {
    for (int pl = 0; pl < PL_COUNT; pl++)
        for (int64_t k = 0;; k++) {
            rig_t r;
            rig_open(&r, 16, 2);
            CHECK_EQ(setup(&r), 0);
            static uint8_t a[20000], b[5000], got[24000];
            fill(a, sizeof(a), 1);
            fill(b, sizeof(b), 2);
            alfs_writer_t w;
            CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
            uint32_t la = w.log;
            CHECK_EQ(put(&r, &w, a, sizeof(a)), 0);
            CHECK_EQ(lclose(&r, &w), 0);
            CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
            uint32_t lb = w.log;
            CHECK_EQ(put(&r, &w, b, sizeof(b)), 0);
            CHECK_EQ(lclose(&r, &w), 0);
            sim_power_after(&r.sim, k, (sim_pl_t)pl);
            int rc = lclean_log(&r, la);
            bool past = !r.sim.off;
            sim_reboot(&r.sim);
            if (past) {
                CHECK(rc >= 0);
                rig_close(&r);
                break;
            }
            CHECK_EQ(remount(&r), 0);
            int32_t n = slurp(&r, la, 1, got, sizeof(got));
            CHECK(n == ALFS_ENOENT || (n >= 0 && runs_cut(got, (uint32_t)n, a, sizeof(a))));
            CHECK(lclean_log(&r, la) >= 0);
            for (int m = 0; m < 8; m++) {
                CHECK_EQ(remount(&r), 0);
                alfs_log_info_t in;
                if (log_info(&r, la, &in) == 0)
                    printf("    pl %d k %lld: log back after its erase, remount %d\n", pl, (long long)k, m);
                CHECK_EQ(log_info(&r, la, &in), ALFS_ENOENT);
                CHECK_EQ(slurp(&r, lb, 1, got, sizeof(got)), (int32_t)sizeof(b));
                CHECK(memcmp(got, b, sizeof(b)) == 0);
            }
            DONE(&r);
            rig_close(&r);
            if (tf_failed) {
                printf("    behaviour %d, power lost at operation %lld\n", pl, (long long)k);
                return;
            }
        }
    /* A first claim torn by FLICKER, then the log erased. */
    rig_t r;
    rig_open(&r, 16, 2);
    CHECK_EQ(setup(&r), 0);
    uint8_t row[ROW];
    make_row(3, row);
    alfs_writer_t w;
    CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
    uint32_t la = w.log;
    sim_power_after(&r.sim, 0, PL_FLICKER);
    put(&r, &w, row, ROW);
    lsync(&r, &w);
    sim_reboot(&r.sim);
    CHECK_EQ(remount(&r), 0);
    CHECK(lclean_log(&r, la) >= 0);
    for (int m = 0; m < 8; m++) {
        CHECK_EQ(remount(&r), 0);
        alfs_log_info_t in;
        CHECK_EQ(log_info(&r, la, &in), ALFS_ENOENT);
    }
    DONE(&r);
    rig_close(&r);
}

/* W-CFG's expected contents: each object's bytes, or absent. */
typedef struct {
    uint32_t size[3], gen[3];
    bool present[3];
} cfg_state_t;

static int cfg_index(const char *name) {
    return !strcmp(name, "cfg") ? 0 : !strcmp(name, "lua/main.lua") ? 1 : 2;
}

static void cfg_apply(cfg_state_t *s, cfg_op_t op) {
    int i = cfg_index(op.name);
    s->present[i] = op.size != 0;
    s->size[i] = op.size;
    s->gen[i] = op.gen;
}

static bool cfg_reads(rig_t *r, const cfg_state_t *s, int i) {
    static const char *const names[] = {"cfg", "lua/main.lua", "cfg2"};
    static uint8_t want[4096];
    if (!s->present[i]) {
        uint8_t got[8];
        return oget(r, names[i], got, sizeof(got)) == ALFS_ENOENT;
    }
    fill(want, s->size[i], s->gen[i]);
    return reads_as(r, names[i], want, s->size[i]);
}

/* Each object reads as `done` has it, or as `next` would. */
static bool cfg_whole(rig_t *r, const cfg_state_t *done, const cfg_state_t *next) {
    for (int i = 0; i < 3; i++)
        if (!cfg_reads(r, done, i) && !cfg_reads(r, next, i))
            return false;
    return true;
}

static void safe_put_atomic(void) {
    for (int pl = 0; pl < PL_COUNT; pl++)
        for (int64_t k = 0;; k++) {
            rig_t r;
            rig_open(&r, 4, 4);
            CHECK_EQ(setup(&r), 0);
            cfg_state_t done = {{0}, {0}, {false}}, next;
            sim_power_after(&r.sim, k, (sim_pl_t)pl);
            int i = 0;
            for (; i < 12; i++) {
                next = done;
                cfg_apply(&next, cfg_step(i));
                int rc = cfg_do(&r, cfg_step(i));
                if (r.sim.off)
                    break;
                CHECK_EQ(rc, 0);
                done = next;
                if (i % 4 == 3 && (oclean(&r) < 0 || r.sim.off))
                    break;
            }
            bool past = !r.sim.off;
            sim_reboot(&r.sim);
            if (past) {
                rig_close(&r);
                break;
            }
            /* After the loss: whole, through eight remounts; then a put, and
             * eight more. Which of the two a half-made commit reads as may
             * change between mounts, but each object is whole each time. */
            for (int m = 0; m < 8; m++) {
                CHECK_EQ(remount(&r), 0);
                if (!cfg_whole(&r, &done, &next))
                    printf("    pl %d k %lld, step %d, remount %d: an object is not whole\n", pl, (long long)k, i, m);
                CHECK(cfg_whole(&r, &done, &next));
            }
            /* Settle on what reads now, then put. */
            cfg_state_t now = done;
            for (int j = 0; j < 3; j++)
                if (!cfg_reads(&r, &done, j)) {
                    now.present[j] = next.present[j];
                    now.size[j] = next.size[j];
                    now.gen[j] = next.gen[j];
                }
            cfg_op_t op = {"cfg", 300, 99};
            CHECK_EQ(cfg_do(&r, op), 0);
            cfg_apply(&now, op);
            for (int m = 0; m < 8; m++) {
                CHECK_EQ(remount(&r), 0);
                for (int j = 0; j < 3; j++)
                    CHECK(cfg_reads(&r, &now, j));
            }
            DONE(&r);
            rig_close(&r);
            if (tf_failed) {
                printf("    behaviour %d, power lost at operation %lld\n", pl, (long long)k);
                return;
            }
        }
}

static void safe_obj_check(void) {
    for (int trial = 0; trial < 120; trial++) {
        rig_t r;
        rig_open(&r, 4, 4);
        CHECK_EQ(setup(&r), 0);
        static uint8_t d[3][500];
        static const char *const names[] = {"one", "two", "three"};
        for (int i = 0; i < 3; i++) {
            fill(d[i], sizeof(d[i]), (uint32_t)(trial * 3 + i));
            CHECK_EQ(oput(&r, names[i], d[i], sizeof(d[i])), 0);
        }
        alfs_obj_status_t os;
        alfs_obj_status(&r.ob, &os);
        uint32_t half = os.seq % 2 ? 0 : 1;
        int hit = trial % 3;
        uint32_t obj_at = r.ocfg.start + half * 2 * SECTOR + PAGE + (uint32_t)hit * (40 + 500);
        int part = (trial / 3) % 4; /* name, size, payload, check */
        static const uint32_t off[] = {0, 32, 36, 536};
        static const uint32_t len[] = {3, 4, 500, 4};
        uint32_t at = obj_at + off[part] + sim_rand(&r.sim) % len[part];
        sim_flip(&r.sim, at, (uint8_t)(1u << (sim_rand(&r.sim) % 8)));
        for (int i = 0; i < 3; i++) {
            static uint8_t got[1000];
            int32_t n = oget(&r, names[i], got, sizeof(got));
            if (i == hit) {
                CHECK(n == ALFS_ECHECK || n == ALFS_ENOENT);
            } else if (i < hit || part != 1) {
                CHECK_EQ(n, 500);
                CHECK(memcmp(got, d[i], 500) == 0);
            } else {
                CHECK(n < 0 || (n == 500 && !memcmp(got, d[i], 500)));
            }
        }
        DONE(&r);
        rig_close(&r);
    }
}

/* ---- rows ------------------------------------------------------------- */

static void if_rows(void) {
    rig_t r;
    rig_open(&r, 16, 2);
    CHECK_EQ(setup(&r), 0);
    alfs_log_status_t st;
    alfs_log_status(&r.lg, &st);
    CHECK_EQ(st.row_max, ROW_MAX);
    uint8_t d[256], got[256];
    fill(d, ROW_MAX + 1, 1);
    alfs_writer_t w;
    CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
    uint32_t edges = w.log;
    CHECK_EQ(CALL(&r, alfs_log_write(&r.lg, &w, d, 1)), 0);
    CHECK_EQ(CALL(&r, alfs_log_write(&r.lg, &w, d, ROW_MAX)), 0);
    CHECK_EQ(CALL(&r, alfs_log_write(&r.lg, &w, d, 0)), ALFS_EINVAL);
    CHECK_EQ(CALL(&r, alfs_log_write(&r.lg, &w, d, ROW_MAX + 1)), ALFS_EINVAL);
    CHECK_EQ(CALL(&r, alfs_log_write(&r.lg, &w, NULL, 1)), ALFS_EINVAL);
    CHECK_EQ(r.sim.c_ops, 0);
    CHECK_EQ(lclose(&r, &w), 0);
    CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
    uint32_t every = w.log;
    for (uint32_t len = 1; len <= ROW_MAX; len++) {
        fill(d, len, len);
        CHECK_EQ(CALL(&r, alfs_log_write(&r.lg, &w, d, len)), 0);
    }
    CHECK_EQ(lclose(&r, &w), 0);
    uint8_t e[256];
    fill(e, ROW_MAX, 1);
    for (int pass = 0; pass < 2; pass++) {
        alfs_reader_t rd;
        CHECK_EQ(CALL(&r, alfs_log_open_read(&r.lg, &rd, edges, 1, r.blk[3], r.bsize)), 0);
        CHECK_EQ(CALL(&r, alfs_log_read(&r.lg, &rd, got, 0)), ALFS_EINVAL); /* consumes nothing */
        CHECK_EQ(CALL(&r, alfs_log_read(&r.lg, &rd, got, 1)), 1);
        CHECK_EQ(got[0], e[0]);
        CHECK_EQ(CALL(&r, alfs_log_read(&r.lg, &rd, got, ROW_MAX - 1)), ALFS_EINVAL);
        CHECK_EQ(CALL(&r, alfs_log_read(&r.lg, &rd, got, ROW_MAX)), (int32_t)ROW_MAX);
        CHECK(memcmp(got, e, ROW_MAX) == 0);
        CHECK_EQ(CALL(&r, alfs_log_read(&r.lg, &rd, got, sizeof(got))), 0);
        CHECK_EQ(CALL(&r, alfs_log_close_read(&r.lg, &rd)), 0);
        CHECK_EQ(CALL(&r, alfs_log_open_read(&r.lg, &rd, every, 1, r.blk[3], r.bsize)), 0);
        for (uint32_t len = 1; len <= ROW_MAX; len++) {
            fill(d, len, len);
            int32_t n = (int32_t)CALL(&r, alfs_log_read(&r.lg, &rd, got, sizeof(got)));
            if (n != (int32_t)len)
                printf("    row %u read as %d bytes\n", len, n);
            CHECK(n == (int32_t)len && !memcmp(got, d, len));
        }
        CHECK_EQ(CALL(&r, alfs_log_read(&r.lg, &rd, got, sizeof(got))), 0);
        CHECK_EQ(CALL(&r, alfs_log_close_read(&r.lg, &rd)), 0);
        CHECK_EQ(remount(&r), 0);
    }
    DONE(&r);
    rig_close(&r);
}

/* A chunk with a valid check whose row lengths do not fill it, three ways:
 * a length past the payload, a length one short, a length of 0. */
static void safe_rows_whole(void) {
    for (int kind = 0; kind < 3; kind++) {
        rig_t r;
        rig_open(&r, 8, 2);
        CHECK_EQ(setup(&r), 0);
        uint8_t d[40][30], got[40 * 30];
        alfs_writer_t w;
        CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
        for (uint32_t i = 0; i < 40; i++) {
            fill(d[i], 30, i);
            CHECK_EQ(CALL(&r, alfs_log_write(&r.lg, &w, d[i], 30)), 0);
        }
        CHECK_EQ(lclose(&r, &w), 0);
        int32_t k = -1;
        CHECK_EQ(claims_of(&r, w.log, &k), 1);
        uint8_t *ch = r.sim.mem + r.lcfg.start + (uint32_t)k * SECTOR + LDATA, *pay = ch + CHUNK;
        uint32_t len = get16le(ch), rows = len / 31; /* the first chunk's rows */
        CHECK(len % 31 == 0 && rows >= 2);
        if (kind == 0)
            pay[0] = 200;
        else if (kind == 1)
            pay[len - 31] = 29;
        else
            pay[31] = 0;
        uint8_t body[PAGE];
        memcpy(body, ch, 3);
        memcpy(body + 3, pay, len);
        uint32_t c = crc32_ref(body, 3 + len);
        memcpy(ch + 3, &c, 4); /* the check passes */
        alfs_log_status_t st;
        alfs_log_status(&r.lg, &st);
        uint32_t fails = st.n.check_failures;
        int32_t n = slurp(&r, w.log, 1, got, sizeof(got));
        CHECK_EQ(n, (int32_t)((40 - rows) * 30)); /* the rest of the page holds no chunk */
        for (uint32_t i = rows; n > 0 && i < 40; i++)
            CHECK(memcmp(got + (i - rows) * 30, d[i], 30) == 0);
        alfs_log_status(&r.lg, &st);
        CHECK_EQ(st.n.check_failures, fails + 1);
        DONE(&r);
        rig_close(&r);
    }
}

/* ---- the configuration application ------------------------------------ */

/* The configuration area's least and greatest erase counts. */
static void cfg_wear(const rig_t *r, uint32_t *lo, uint32_t *hi) {
    *lo = UINT32_MAX;
    *hi = 0;
    for (uint32_t k = 0; k < CFG_SECTORS; k++) {
        uint32_t wv = r->sim.wear[r->ccfg.start / SECTOR + k];
        *lo = wv < *lo ? wv : *lo;
        *hi = wv > *hi ? wv : *hi;
    }
}

static void cfg_round_trip(void) {
    rig_t r;
    rig_open(&r, 4, 2);
    CHECK_EQ(setup(&r), 0);
    kv_state_t st;
    memset(&st, 0, sizeof(st));
    for (int i = 0; i < 300; i++) {
        kv_op_t op = kv_step(i, 150);
        bool had = st.present[op.key[1] - '0'];
        int rc = kv_do(&r, op);
        CHECK(rc == 0 || (rc == ALFS_ENOENT && !op.len && !had));
        kv_apply(&st, op);
        if (i % 37 == 36)
            CHECK_EQ(remount(&r), 0);
        for (int j = 0; j < 10; j++)
            CHECK(kv_reads(&r, &st, j));
    }
    uint8_t v[256];
    fill(v, sizeof(v), 3);
    uint64_t p0 = r.sim.programs;
    CHECK_EQ(kdel(&r, "absent"), ALFS_ENOENT);
    CHECK_EQ(kset(&r, "", v, 1), ALFS_EINVAL);
    CHECK_EQ(kset(&r, NULL, v, 1), ALFS_EINVAL);
    CHECK_EQ(kset(&r, "01234567890123456789012345678901", v, 1), ALFS_EINVAL); /* 32 bytes */
    CHECK_EQ(kset(&r, "a", v, ROW_MAX - 1), ALFS_EINVAL);                       /* a row of R + 1 */
    CHECK_EQ(r.sim.programs, p0);
    CHECK_EQ(kset(&r, "a", v, ROW_MAX - 2), 0); /* a row of R */
    CHECK_EQ(kset(&r, "0123456789012345678901234567890", v, 10), 0);
    CHECK_EQ(remount(&r), 0);
    CHECK(kreads(&r, "a", v, ROW_MAX - 2));
    CHECK(kreads(&r, "0123456789012345678901234567890", v, 10));
    for (int j = 0; j < 10; j++)
        CHECK(kv_reads(&r, &st, j));
    uint32_t lo, hi;
    cfg_wear(&r, &lo, &hi);
    CHECK(lo >= 3); /* the ring has turned */
    DONE(&r);
    rig_close(&r);
}

static void life_cfg_wear(void) {
    rig_t r;
    rig_open(&r, 4, 2);
    CHECK_EQ(setup(&r), 0);
    uint32_t lo = 0, hi = 0;
    for (int i = 0; i < 100000 && lo < 10; i++) {
        if (i % 50 == 49)
            CHECK_EQ(remount(&r), 0);
        CHECK_EQ(kv_do(&r, kv_step(i % 8, 150)), 0); /* no deletes */
        cfg_wear(&r, &lo, &hi);
    }
    if (hi - lo > 2)
        printf("    wear %u to %u\n", lo, hi);
    CHECK(lo >= 10);
    CHECK(hi - lo <= 2);
    DONE(&r);
    rig_close(&r);
}

/* Key i: 1 when it reads as `a` has it, 2 when as `b`, 0 when as neither. */
static int kv_match(rig_t *r, const kv_state_t *a, const kv_state_t *b, int i) {
    char key[4] = {'k', (char)('0' + i), 0};
    uint8_t got[PAGE], want[PAGE];
    int32_t n = kget(r, key, got, sizeof(got));
    const kv_state_t *st[2] = {a, b};
    for (int w = 0; w < 2; w++) {
        fill(want, st[w]->len[i], st[w]->gen[i]);
        if (st[w]->present[i] ? n == (int32_t)st[w]->len[i] && !memcmp(got, want, (size_t)n) : n == ALFS_ENOENT)
            return w + 1;
    }
    return 0;
}

/* Each key reads as `done` has it, or as `next` would. */
static bool kv_whole(rig_t *r, const kv_state_t *done, const kv_state_t *next) {
    for (int i = 0; i < 10; i++)
        if (!kv_match(r, done, next, i))
            return false;
    return true;
}

static void safe_cfg_atomic(void) {
    for (int pl = 0; pl < PL_COUNT; pl++)
        for (int64_t k = 0;; k++) {
            rig_t r;
            rig_open(&r, 4, 2);
            CHECK_EQ(setup(&r), 0);
            kv_state_t done, next;
            memset(&done, 0, sizeof(done));
            sim_power_after(&r.sim, k, (sim_pl_t)pl);
            int i = 0;
            for (; i < 60; i++) {
                kv_op_t op = kv_step(i, 150);
                next = done;
                kv_apply(&next, op);
                int rc = kv_do(&r, op);
                if (r.sim.off)
                    break;
                CHECK(rc == 0 || (rc == ALFS_ENOENT && !op.len));
                done = next;
                if (i == 30 && (remount(&r) || r.sim.off)) /* a reload in the middle */
                    break;
            }
            bool past = !r.sim.off;
            sim_reboot(&r.sim);
            if (past) {
                rig_close(&r);
                break;
            }
            for (int m = 0; m < 8; m++) {
                CHECK_EQ(remount(&r), 0);
                if (!kv_whole(&r, &done, &next))
                    printf("    pl %d k %lld, step %d, reload %d: a key reads wrong\n", pl, (long long)k, i, m);
                CHECK(kv_whole(&r, &done, &next));
            }
            /* A set starts a new log from the settings as this load read
             * them; then every reload reads the same. */
            kv_op_t op = {"k0", 99, 999};
            kv_state_t now = done;
            for (int j = 0; j < 10; j++)
                if (kv_match(&r, &done, &next, j) == 2) {
                    now.present[j] = next.present[j];
                    now.len[j] = next.len[j];
                    now.gen[j] = next.gen[j];
                }
            CHECK_EQ(kv_do(&r, op), 0);
            kv_apply(&now, op);
            for (int m = 0; m < 8; m++) {
                CHECK_EQ(remount(&r), 0);
                for (int j = 0; j < 10; j++)
                    CHECK(kv_reads(&r, &now, j));
            }
            if (r.sim.violations)
                printf("    pl %d k %lld, step %d\n", pl, (long long)k, i);
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
    rig_open(&r, 32, 4);
    CHECK_EQ(setup(&r), 0);
    alfs_log_status_t st;
    alfs_log_status(&r.lg, &st);
    CHECK_EQ(st.sectors, 32);
    CHECK_EQ(st.ready, 32);
    CHECK_EQ(st.dirty, 0);
    CHECK_EQ(st.n.erases, 32);
    CHECK_EQ(st.writable, 32 * (SECTOR - LDATA - CHUNK));
    CHECK_EQ(st.row_max, PAGE - 38);
    uint64_t p0 = r.sim.programs;
    run_t lg;
    run_init(&lg, 1 << 18);
    alfs_writer_t w;
    CHECK_EQ(lopen(&r, &w, 0, 1, 0), 0);
    CHECK_EQ(log_run(&r, &w, &lg, 50, 60000, false), 0);
    alfs_log_status(&r.lg, &st);
    CHECK_EQ(st.writing, w.log);
    CHECK_EQ(lclose(&r, &w), 0);
    alfs_log_status(&r.lg, &st);
    CHECK_EQ(st.n.programs - 32, r.sim.programs - p0);
    CHECK_EQ(st.ready, 32 - claims_of(&r, w.log, NULL));
    CHECK_EQ(st.n.prog_failures, 0);
    CHECK_EQ(st.n.bytes_lost, 0);
    CHECK_EQ(st.bad, 0);
    uint8_t c[300];
    fill(c, sizeof(c), 1);
    CHECK_EQ(oput(&r, "cfg", c, sizeof(c)), 0);
    alfs_obj_status_t os;
    alfs_obj_status(&r.ob, &os);
    CHECK_EQ(os.used, 340);
    CHECK_EQ(os.capacity, 2 * SECTOR - PAGE);
    CHECK_EQ(os.seq, 2);
    CHECK(!os.ready);
    CHECK_EQ(remount(&r), 0);
    alfs_obj_status(&r.ob, &os);
    CHECK_EQ(os.used, 340);
    CHECK_EQ(os.seq, 2);
    alfs_log_status(&r.lg, &st);
    CHECK_EQ(st.writing, 0);
    free(lg.exp);
    DONE(&r);
    rig_close(&r);
}

static void obs_clean(void) {
    rig_t r;
    rig_open(&r, 8, 4);
    CHECK_EQ(LMOUNT(&r), 0);
    alfs_log_status_t st;
    alfs_log_status(&r.lg, &st);
    CHECK(!st.clean);
    CHECK(lclean(&r) >= 0);
    alfs_log_status(&r.lg, &st);
    CHECK(st.clean);
    sim_flip(&r.sim, r.lcfg.start + 2 * SECTOR + 3, 1); /* a stamp broken */
    CHECK_EQ(remount(&r), 0);
    alfs_log_status(&r.lg, &st);
    CHECK(!st.clean);
    CHECK(lclean(&r) >= 0);
    alfs_log_status(&r.lg, &st);
    CHECK(st.clean);
    CHECK_EQ(OFORMAT(&r), 0);
    CHECK(spare_ready(&r));
    uint8_t c[10] = {1};
    CHECK_EQ(oput(&r, "cfg", c, sizeof(c)), 0);
    CHECK(!spare_ready(&r));
    CHECK(oclean(&r) >= 0);
    CHECK(spare_ready(&r));
    CHECK_EQ(remount(&r), 0);
    CHECK(spare_ready(&r));
    DONE(&r);
    rig_close(&r);
}

/* ---- SIMP ------------------------------------------------------------- */

static void simp_ram(void) {
    /* The budgets are for a 32-bit target; a host's pointers are wider. */
    size_t wide = sizeof(void *) - 4;
    CHECK(sizeof(alfs_log_t) <= 96 + 3 * wide);
    CHECK(sizeof(alfs_obj_t) <= 96 + 4 * wide);
    CHECK(sizeof(alfs_writer_t) <= 32 + wide);
    CHECK(sizeof(alfs_reader_t) <= 32 + wide);
    CHECK(sizeof(alfs_put_t) <= 16);
    CHECK(sizeof(alfs_get_t) <= 16);
}

int main(int argc, char **argv) {
    static const tf_test_t tests[] = {
        {"sim_torn_obeys_a9", sim_torn_obeys_a9},
        {"if_geometry", if_geometry},
        {"if_endian", if_endian},
        {"if_object_names", if_object_names},
        {"if_put_replaces", if_put_replaces},
        {"if_put_one_at_a_time", if_put_one_at_a_time},
        {"if_get_snapshot", if_get_snapshot},
        {"if_object_list", if_object_list},
        {"if_log_numbers_never_reused", if_log_numbers_never_reused},
        {"if_log_streams", if_log_streams},
        {"if_log_new_after_mount", if_log_new_after_mount},
        {"if_log_list", if_log_list},
        {"if_log_refusals", if_log_refusals},
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
        {"safe_powerloss_durable", safe_powerloss_durable},
        {"safe_powerloss_bound", safe_powerloss_bound},
        {"safe_no_program_after_torn", safe_no_program_after_torn},
        {"safe_no_false_data", safe_no_false_data},
        {"safe_write_only_verified_ready", safe_write_only_verified_ready},
        {"safe_prog_failure", safe_prog_failure},
        {"safe_bad_sector", safe_bad_sector},
        {"safe_read_retry", safe_read_retry},
        {"safe_foreign_area", safe_foreign_area},
        {"safe_fuzz", safe_fuzz},
        {"safe_log_erase", safe_log_erase},
        {"safe_put_atomic", safe_put_atomic},
        {"safe_obj_check", safe_obj_check},
        {"if_rows", if_rows},
        {"safe_rows_whole", safe_rows_whole},
        {"cfg_round_trip", cfg_round_trip},
        {"life_cfg_wear", life_cfg_wear},
        {"safe_cfg_atomic", safe_cfg_atomic},
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
