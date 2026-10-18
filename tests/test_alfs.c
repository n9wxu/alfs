/*
 * alfs's tests, one or more for each requirement of docs/REQUIREMENTS.md.
 * Each is named in the requirement's "Verified by".
 *
 * Every call a test makes goes through CALL, which holds it to SPD-01 (no
 * erase) and records the longest single operation (SPD-06), or ECALL, for
 * the cleanup, the one call SPD-01 lets erase. Every test ends with DONE, which fails it if
 * the simulator saw a violation (a program across a page, a byte
 * programmed twice, or a program into a byte a power loss touched, each but
 * a program that settles, docs/TEST_PLAN.md 2), if an erase took a sector
 * that read erased (LIFE-01), or if a byte outside the area changed. Byte
 * checks read the simulator's flash by docs/SPEC.md. The simulator's seed
 * comes from ALFS_SEED.
 */
#include "alfs_fs.h"
#include "sim.h"
#include "tf.h"

#include <stdlib.h>
#include <string.h>

#define SECTOR 4096u
#define PAGE 256u
#define PPS (SECTOR / PAGE)  /* pages a sector */
#define HDR 14u                  /* a page header (docs/SPEC.md 2) */
#define FBLOCK 51u               /* a frame block (SPEC 1.1) */
#define NAME_AT (HDR + 4u)       /* the filename, on a first page */
#define VERSION_AT (HDR + 36u)   /* the version, on a first page */
#define OPEN_AT (HDR + 37u)      /* the open check, on a first page */
#define CLOSE_AT (HDR + 41u)     /* the close fields, on a first page */
#define D (PAGE - HDR - 2u - 4u) /* data a page holds: byte count and check apart */
#define D0 (D - FBLOCK)          /* data a file's first page holds */
#define CACHE 64u                /* records in the rig's cache */
#define ROW 22u
#define LINK_NONE 0xFFFFFFFFu
#define MAX_PROGS 64

/* ---- the rig ---------------------------------------------------------- */

typedef struct {
    sim_t sim;
    alfs_driver_t raw, drv;
    alfs_area_t cfg;
    alfs_fs_t fs;
    uint32_t bsize; /* the writer's block, a page unless a test says */
    uint8_t scratch[PAGE];
    alfs_rec_t cache[CACHE];
    uint16_t ncache; /* the cache's records, CACHE unless a test says */
    uint8_t wblk[SECTOR], rblk[PAGE];
    uint8_t gap[2][SECTOR]; /* the sectors around the area, as filled */
    uint64_t blank_erases;  /* erases of a sector that read erased */
    uint32_t plen[MAX_PROGS], paddr[MAX_PROGS]; /* this call's programs */
    int nprog;
} rig_t;

static int call_bad;           /* calls that broke SPD-01 */
static char call_why[96];
static uint64_t longest_stall; /* the longest operation outside an erase, us */
static uint32_t seed;          /* the simulator's, from ALFS_SEED */

static int w_read(void *ctx, uint32_t addr, void *buf, uint32_t len) {
    rig_t *r = ctx;
    return r->raw.read(r->raw.ctx, addr, buf, len);
}

static int w_prog(void *ctx, uint32_t addr, const void *buf, uint32_t len) {
    rig_t *r = ctx;
    if (r->nprog < MAX_PROGS) {
        r->paddr[r->nprog] = addr;
        r->plen[r->nprog] = len;
    }
    r->nprog++;
    return r->raw.prog(r->raw.ctx, addr, buf, len);
}

static int w_erase(void *ctx, uint32_t addr) {
    rig_t *r = ctx;
    bool blank = true;
    for (uint32_t i = 0; blank && i < SECTOR; i++)
        blank = r->sim.mem[addr + i] == 0xFF;
    if (blank && !r->sim.off)
        r->blank_erases++;
    return r->raw.erase(r->raw.ctx, addr);
}

/* The chip: a gap sector, the area of `sectors` sectors, a gap sector. */
static void rig_open(rig_t *r, uint32_t sectors) {
    uint32_t chip = (2u + sectors) * SECTOR;
    memset(r, 0, sizeof(*r));
    sim_init(&r->sim, chip, SECTOR, PAGE, 0xFF, seed);
    sim_fill_random(&r->sim, 0, chip); /* what a used chip holds */
    memcpy(r->gap[0], r->sim.mem, SECTOR);
    memcpy(r->gap[1], r->sim.mem + chip - SECTOR, SECTOR);
    r->raw = sim_driver(&r->sim);
    r->drv = (alfs_driver_t){r, w_read, w_prog, w_erase};
    r->cfg = (alfs_area_t){SECTOR, sectors * SECTOR, chip, SECTOR, PAGE};
    r->bsize = PAGE;
    r->ncache = CACHE;
    call_bad = 0;
    call_why[0] = 0;
    longest_stall = 0;
}

static void rig_close(rig_t *r) {
    sim_free(&r->sim);
}

static void begin(rig_t *r) {
    sim_call_begin(&r->sim);
    r->nprog = 0;
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

#define CALL(r, e) (begin(r), chk((r), (int64_t)(e), false, #e))
#define ECALL(r, e) (begin(r), chk((r), (int64_t)(e), true, #e))

static bool gaps_kept(const rig_t *r) {
    return !memcmp(r->gap[0], r->sim.mem, SECTOR) &&
           !memcmp(r->gap[1], r->sim.mem + r->sim.chip_size - SECTOR, SECTOR);
}

/* Ends a test: no violation of the flash's rules, of SPD-01, of LIFE-01's
 * erased sectors, or of the bytes outside the area (SAFE-09). */
#define DONE(r)                                                                                    \
    do {                                                                                           \
        if ((r)->sim.violations)                                                                   \
            printf("    flash: %s\n", (r)->sim.why);                                               \
        if (call_bad)                                                                              \
            printf("    call: %s\n", call_why);                                                    \
        CHECK_EQ((r)->sim.violations, 0);                                                          \
        CHECK_EQ(call_bad, 0);                                                                     \
        CHECK_EQ((r)->blank_erases, 0);                                                            \
        CHECK(gaps_kept(r));                                                                       \
    } while (0)

/* ---- data ------------------------------------------------------------- */

static uint32_t crc32_more(uint32_t c, const uint8_t *p, uint32_t n) {
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c;
}

static uint32_t crc32_ref(const uint8_t *p, uint32_t n) {
    return ~crc32_more(0xFFFFFFFFu, p, n);
}

static uint32_t get32le(const uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t get16le(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8;
}

static void put32le(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* A page's data check (SPEC 2): the frame ID, the page count, the byte
 * count, the data. */
static uint32_t page_check(uint32_t id, uint32_t count, const uint8_t *data, uint32_t n) {
    uint8_t h[8] = {(uint8_t)id,    (uint8_t)(id >> 8),    (uint8_t)(id >> 16), (uint8_t)(id >> 24),
                    (uint8_t)count, (uint8_t)(count >> 8), (uint8_t)n,          (uint8_t)(n >> 8)};
    return ~crc32_more(crc32_more(0xFFFFFFFFu, h, 8), data, n);
}

static void fill(uint8_t *p, uint32_t n, uint32_t gen) {
    for (uint32_t i = 0; i < n; i++)
        p[i] = (uint8_t)(gen * 131u + i * 7u + (i >> 8));
}

/* A W-LOG row: its sequence number and a check. */
static void make_row(uint32_t seq, uint8_t *row) {
    put32le(row, seq);
    for (uint32_t i = 4; i < ROW - 4; i++)
        row[i] = (uint8_t)(seq * 31u + i);
    put32le(row + ROW - 4, crc32_ref(row, ROW - 4));
}

/* The pages a file of n bytes takes (SPEC 2), at least one. */
static uint32_t pages_for(uint32_t n) {
    return n <= D0 ? 1u : 1u + (n - D0 + D - 1u) / D;
}

/* The bytes of n written that complete pages hold. */
static uint32_t whole_pages(uint32_t n) {
    return n < D0 ? 0u : D0 + (n - D0) / D * D;
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

/* ---- the store's calls -------------------------------------------------- */

static int fmount(rig_t *r) {
    return (int)CALL(r, alfs_fs_mount(&r->fs, &r->cfg, &r->drv, r->scratch, r->cache, r->ncache));
}

/* A reboot: every handle gone, the area mounted again. */
static int remount(rig_t *r) {
    memset(&r->fs, 0, sizeof(r->fs));
    return fmount(r);
}

/* Cleanup steps until none is left: the steps, or -1. */
static int clean_all(rig_t *r) {
    for (int i = 0; i < 100000; i++) {
        int rc = (int)ECALL(r, alfs_fs_cleanup(&r->fs, 1));
        if (rc == ALFS_CLEAN)
            return i;
        if ((rc < 0 && rc != ALFS_EIO) || r->sim.off)
            return -1;
    }
    return -1;
}

/* The area mounted and every page made free. */
static int setup(rig_t *r) {
    return remount(r) || clean_all(r) < 0 ? -1 : 0;
}

static int falloc(rig_t *r, const char *name, uint32_t bytes, uint32_t *id) {
    uint32_t got = 0;
    int rc = (int)CALL(r, alfs_file_alloc(&r->fs, name, bytes, &got));
    if (id)
        *id = got;
    return rc;
}

static int fopenw(rig_t *r, alfs_file_t *f, const char *name, unsigned flags) {
    return (int)CALL(r, alfs_file_open(&r->fs, f, name, flags, r->wblk, r->bsize));
}

/* Writes d in pieces of up to 100 bytes; *taken: the bytes alfs took. 0, or
 * the error. */
static int put(rig_t *r, alfs_file_t *f, const uint8_t *d, uint32_t n, uint32_t *taken) {
    uint32_t t = 0;
    int rc = 0;
    while (t < n) {
        uint32_t k = n - t < 100 ? n - t : 100;
        int32_t m = (int32_t)CALL(r, alfs_file_write(&r->fs, f, d + t, k));
        if (m <= 0) {
            rc = m ? (int)m : -100;
            break;
        }
        t += (uint32_t)m;
    }
    if (taken)
        *taken = t;
    return rc;
}

static int fsyncw(rig_t *r, alfs_file_t *f) {
    return (int)CALL(r, alfs_file_sync(&r->fs, f));
}

static int fflushw(rig_t *r, alfs_file_t *f) {
    return (int)CALL(r, alfs_file_flush(&r->fs, f));
}

static int fclosew(rig_t *r, alfs_file_t *f) {
    return (int)CALL(r, alfs_file_close(&r->fs, f));
}

/* Reads a whole file in pieces of up to 77 bytes into out: its length, or
 * an error, after which *n holds the bytes read before it. */
static int32_t slurp_n(rig_t *r, const char *name, uint8_t *out, uint32_t cap, uint32_t *got) {
    alfs_reader_t rd;
    uint32_t n = 0;
    int32_t result = -201;
    int rc = (int)CALL(r, alfs_file_open_read(&r->fs, &rd, name, r->rblk, PAGE));
    if (rc) {
        if (got)
            *got = 0;
        return rc;
    }
    for (int g = 0; g < 1000000; g++) {
        uint32_t want = cap - n < 77 ? cap - n : 77;
        int32_t k = (int32_t)CALL(r, alfs_file_read(&r->fs, &rd, out + n, want));
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
    CALL(r, alfs_file_close_read(&r->fs, &rd));
    if (got)
        *got = n;
    return result;
}

static int32_t slurp(rig_t *r, const char *name, uint8_t *out, uint32_t cap) {
    return slurp_n(r, name, out, cap, NULL);
}

static bool reads_as(rig_t *r, const char *name, const uint8_t *d, uint32_t n) {
    static uint8_t got[65536];
    int32_t m = slurp(r, name, got, sizeof(got));
    return m == (int32_t)n && !memcmp(got, d, n);
}

/* Every file listed has name a or name b: a power loss leaves no file under
 * a filename no one wrote. */
static bool lists_only(rig_t *r, const char *a, const char *b) {
    alfs_file_info_t in;
    uint32_t after = 0;
    for (uint32_t i = 0; i < CACHE && alfs_file_list(&r->fs, after, &in) == 0; i++) {
        after = in.id;
        if (strcmp(in.name, a) && strcmp(in.name, b))
            return false;
    }
    return true;
}

/* Allocates `reserve` bytes, writes d, closes. 0, or the first error. Its
 * block is its own, so another writer's stays as it is. */
static int write_file(rig_t *r, const char *name, const uint8_t *d, uint32_t n, uint32_t reserve) {
    static uint8_t blk[PAGE];
    alfs_file_t f;
    int rc = falloc(r, name, reserve, NULL);
    if (!rc)
        rc = (int)CALL(r, alfs_file_open(&r->fs, &f, name, 0, blk, sizeof(blk)));
    if (!rc)
        rc = put(r, &f, d, n, NULL);
    return rc ? rc : fclosew(r, &f);
}

static int fdelete(rig_t *r, const char *name) {
    return (int)CALL(r, alfs_file_delete(&r->fs, name));
}

/* The listing entry for name, or ALFS_ENOENT. */
static int finfo(rig_t *r, const char *name, alfs_file_info_t *in) {
    uint32_t after = 0;
    for (int i = 0; i < 1000; i++) {
        int rc = (int)CALL(r, alfs_file_list(&r->fs, after, in));
        if (rc)
            return rc;
        if (!strcmp(in->name, name))
            return ALFS_OK;
        after = in->id;
    }
    return ALFS_ENOENT;
}

static alfs_fs_status_t fstatus(rig_t *r) {
    alfs_fs_status_t st;
    memset(&st, 0, sizeof(st));
    CALL(r, alfs_fs_status(&r->fs, &st));
    return st;
}

/* ---- the flash, by docs/SPEC.md ----------------------------------------- */

static uint8_t *pg(rig_t *r, uint32_t page) {
    return r->sim.mem + r->cfg.start + page * PAGE;
}

static bool erased(const uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (p[i] != 0xFF)
            return false;
    return true;
}

/* The first page of the copy file name names (SPEC 2), by the pages'
 * bytes: the newest closed copy, or the newest; 0 for none. */
static uint32_t first_of(rig_t *r, const char *name) {
    uint32_t best = 0;
    int best_closed = 0;
    uint8_t best_v = 0;
    for (uint32_t p = 1; p < r->cfg.size / PAGE; p++) {
        const uint8_t *h = pg(r, p);
        if (get32le(h + 6) != crc32_ref(h, 6) || get16le(h + 4) != 0 || get32le(h + HDR) != get32le(h) ||
            get32le(h + OPEN_AT) != crc32_ref(h + HDR, 37) ||
            strncmp((const char *)h + NAME_AT, name, 32) || !name[0])
            continue;
        int closed = get32le(h + CLOSE_AT + 6) == crc32_ref(h + HDR, 47);
        uint8_t v = h[VERSION_AT];
        if (!best || closed > best_closed || (closed == best_closed && (int8_t)(uint8_t)(v - best_v) > 0)) {
            best = p;
            best_closed = closed;
            best_v = v;
        }
    }
    return best;
}

/* A list by its links (SPEC 2), from page first: its pages into out. */
static uint32_t list_of(rig_t *r, uint32_t first, uint32_t *out, uint32_t max) {
    uint32_t n = 0, p = first, id = get32le(pg(r, first));
    while (n < max && p >= 1 && p < r->cfg.size / PAGE) {
        const uint8_t *h = pg(r, p);
        if (get32le(h + 6) != crc32_ref(h, 6) || get32le(h) != id || get16le(h + 4) != n)
            break;
        out[n++] = p;
        uint32_t next = get32le(h + 10);
        if (next == 0 || next == LINK_NONE)
            break;
        p = next;
    }
    return n;
}

/* File name's pages, by the pages and the links. */
static uint32_t pages_of(rig_t *r, const char *name, uint32_t *out, uint32_t max) {
    uint32_t first = first_of(r, name);
    return first ? list_of(r, first, out, max) : 0u;
}

/* ---- the simulator ------------------------------------------------------ */

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
    rig_open(&r, 8);
    alfs_area_t good = r.cfg;
    struct {
        const char *what;
        uint32_t *field;
        uint32_t value;
    } bad[] = {
        {"area not sector-aligned", &r.cfg.start, SECTOR + 1},
        {"area past the chip", &r.cfg.size, 64 * SECTOR},
        {"sector not a power of two", &r.cfg.sector_size, 3000},
        {"sector under four pages", &r.cfg.sector_size, 512},
        {"page not a power of two", &r.cfg.page_size, 200},
        {"page under 128 bytes", &r.cfg.page_size, 64},
        {"area under two sectors", &r.cfg.size, SECTOR},
        {"empty area", &r.cfg.size, 0},
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        r.cfg = good;
        *bad[i].field = bad[i].value;
        int rc = remount(&r);
        if (rc != ALFS_EGEOMETRY && rc != ALFS_EINVAL)
            printf("    mount accepted: %s (%d)\n", bad[i].what, rc);
        CHECK(rc == ALFS_EGEOMETRY || rc == ALFS_EINVAL);
        CHECK_EQ(r.sim.ops, 0);
        CHECK_EQ(r.sim.bytes_read, 0);
    }
    r.cfg = good;
    CHECK_EQ(setup(&r), 0);
    DONE(&r);
    rig_close(&r);
}

static void if_mount(void) {
    rig_t r;
    rig_open(&r, 8);
    CHECK_EQ(remount(&r), 0); /* a used chip's bytes: no file, nothing written */
    CHECK_EQ(r.sim.c_ops, 0);
    alfs_file_info_t in;
    CHECK_EQ(CALL(&r, alfs_file_list(&r.fs, 0, &in)), ALFS_ENOENT);
    alfs_fs_status_t st = fstatus(&r);
    CHECK_EQ(st.pages, 8 * PPS - 1); /* page 0 holds no file */
    CHECK_EQ(st.live, 0);
    CHECK_EQ(st.records, 0);
    CHECK_EQ(st.records_max, CACHE);
    CHECK(!st.clean);
    CHECK(clean_all(&r) >= 0);
    st = fstatus(&r);
    CHECK_EQ(st.free, 8 * PPS - 1);
    CHECK_EQ(st.dead, 0);
    CHECK(st.clean);
    uint8_t d[100];
    fill(d, sizeof(d), 1);
    CHECK_EQ(write_file(&r, "a", d, 100, 100), 0);
    CHECK_EQ(write_file(&r, "b", d, 100, 100), 0);
    CHECK_EQ(write_file(&r, "c", d, 100, 100), 0);
    CHECK_EQ(remount(&r), 0);
    CHECK_EQ(fstatus(&r).records, 3);
    CHECK(reads_as(&r, "b", d, 100));
    r.ncache = 2; /* a table too small for the files */
    CHECK_EQ(remount(&r), ALFS_ENOSPC);
    CHECK_EQ(r.sim.c_ops, 0);
    r.ncache = CACHE;
    CHECK_EQ(remount(&r), 0);
    DONE(&r);
    rig_close(&r);
}

/* The catalog's, a page's, a frame block's and a data check's bytes where
 * docs/SPEC.md puts them. */
static void if_endian(void) {
    rig_t r;
    rig_open(&r, 8);
    CHECK_EQ(setup(&r), 0);
    uint8_t d[300];
    fill(d, sizeof(d), 1);
    uint32_t id = 0;
    CHECK_EQ(falloc(&r, "ab", 300, &id), 0);
    alfs_file_t f;
    CHECK_EQ(fopenw(&r, &f, "ab", 0), 0);
    CHECK_EQ(f.id, id);
    CHECK_EQ(put(&r, &f, d, sizeof(d), NULL), 0);
    CHECK_EQ(fclosew(&r, &f), 0);
    uint32_t first = first_of(&r, "ab"), pages[4];
    CHECK(first >= 1);
    CHECK_EQ(list_of(&r, first, pages, 4), 2); /* 300 bytes: 185 and 115 */
    const uint8_t *p0 = pg(&r, pages[0]), *p1 = pg(&r, pages[1]);
    CHECK_EQ(get32le(p0), id);
    CHECK_EQ(get16le(p0 + 4), 0);
    CHECK_EQ(get32le(p0 + 6), crc32_ref(p0, 6));
    CHECK_EQ(get32le(p0 + 10), pages[1]);
    CHECK_EQ(get32le(p0 + HDR), id);
    CHECK(memcmp(p0 + NAME_AT, "ab", 3) == 0);
    for (int i = 3; i < 32; i++)
        CHECK_EQ(p0[NAME_AT + i], 0);
    CHECK_EQ(get32le(p0 + OPEN_AT), crc32_ref(p0 + HDR, 37));
    const uint8_t *c = p0 + CLOSE_AT;
    CHECK_EQ(get16le(c), 2);       /* page count */
    CHECK_EQ(get32le(c + 2), 300); /* size       */
    CHECK_EQ(get32le(c + 6), crc32_ref(p0 + HDR, 47));
    const uint8_t *b0 = p0 + HDR + FBLOCK, *b1 = p1 + HDR; /* byte counts */
    CHECK_EQ(get16le(b0), D0);
    CHECK(memcmp(b0 + 2, d, D0) == 0);
    CHECK_EQ(get32le(b0 + 2 + D0), page_check(id, 0, d, D0));
    CHECK_EQ(get16le(p1 + 4), 1);
    CHECK_EQ(get32le(p1 + 6), crc32_ref(p1, 6));
    CHECK_EQ(get32le(p1 + 10), 0);   /* the last page */
    CHECK_EQ(get16le(b1), 300 - D0); /* partial: the close's */
    CHECK(memcmp(b1 + 2, d + D0, 300 - D0) == 0);
    CHECK_EQ(get32le(b1 + 2 + 300 - D0), page_check(id, 1, d + D0, 300 - D0));
    CHECK(erased(b1 + 2 + 300 - D0 + 4, PAGE - HDR - 2 - (300 - D0) - 4));
    CHECK(erased(pg(&r, 0), PAGE)); /* page 0 holds no file */
    DONE(&r);
    rig_close(&r);
}

static void if_names(void) {
    static const char *const good[] = {"a", "0123456789012345678901234567890", "lua/main.lua"};
    rig_t r;
    rig_open(&r, 8);
    CHECK_EQ(setup(&r), 0);
    for (int i = 0; i < 3; i++) {
        uint8_t b = (uint8_t)i;
        CHECK_EQ(write_file(&r, good[i], &b, 1, 1), 0);
    }
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < 3; i++) {
            uint8_t b = (uint8_t)i;
            CHECK(reads_as(&r, good[i], &b, 1));
        }
        CHECK_EQ(remount(&r), 0);
    }
    static const char *const bad[] = {"", "01234567890123456789012345678901"};
    alfs_file_t f;
    alfs_reader_t rd;
    uint32_t id;
    for (int i = 0; i < 2; i++) {
        CHECK_EQ(falloc(&r, bad[i], 10, &id), ALFS_EINVAL);
        CHECK_EQ(r.sim.c_ops, 0);
        CHECK_EQ(fopenw(&r, &f, bad[i], 0), ALFS_EINVAL);
        CHECK_EQ(CALL(&r, alfs_file_open_read(&r.fs, &rd, bad[i], r.rblk, PAGE)), ALFS_EINVAL);
        CHECK_EQ(fdelete(&r, bad[i]), ALFS_EINVAL);
    }
    CHECK_EQ(falloc(&r, NULL, 10, &id), ALFS_EINVAL);
    CHECK_EQ(CALL(&r, alfs_file_open_read(&r.fs, &rd, "lua", r.rblk, PAGE)), ALFS_ENOENT); /* a prefix */
    CHECK_EQ(CALL(&r, alfs_file_open_read(&r.fs, &rd, "lua/main.lua2", r.rblk, PAGE)), ALFS_ENOENT);
    CHECK_EQ(fopenw(&r, &f, "a", 0), ALFS_EEXIST); /* closed: final */
    CHECK_EQ(r.sim.c_ops, 0);
    DONE(&r);
    rig_close(&r);
}

static void if_alloc(void) {
    rig_t r;
    rig_open(&r, 8);
    CHECK_EQ(setup(&r), 0);
    uint32_t id = 0, want = pages_for(1000); /* 5 pages */
    CHECK_EQ(falloc(&r, "log", 1000, &id), 0);
    CHECK_EQ(r.sim.c_programs, want + 1); /* each page's header; the first page's frame block before it */
    uint32_t pages[16];
    CHECK_EQ(pages_of(&r, "log", pages, 16), want);
    for (uint32_t i = 0; i < want; i++) {
        const uint8_t *p = pg(&r, pages[i]);
        uint32_t h = i ? HDR : HDR + FBLOCK;
        CHECK_EQ(get32le(p), id);
        CHECK(erased(p + h, PAGE - h)); /* no data, no data check */
        if (i + 1 < want)
            CHECK_EQ(get32le(p + 10), pages[i + 1]);
    }
    CHECK_EQ(get32le(pg(&r, pages[want - 1]) + 10), LINK_NONE); /* the reservation's end */
    CHECK(erased(pg(&r, pages[0]) + CLOSE_AT, 10));            /* not closed */
    alfs_file_info_t in;
    CHECK_EQ(finfo(&r, "log", &in), 0);
    CHECK_EQ(in.id, id);
    CHECK(!in.closed);
    CHECK_EQ(in.size, ALFS_SIZE_OPEN);
    /* Too few free pages: ENOSPC, nothing written; a cleanup, then room. */
    static uint8_t d[30000];
    fill(d, sizeof(d), 2);
    CHECK_EQ(fdelete(&r, "log"), 0);
    alfs_fs_status_t st = fstatus(&r);
    uint32_t big = D0 + (st.free - 1) * D; /* every free page */
    CHECK_EQ(falloc(&r, "big", big + D, NULL), ALFS_ENOSPC);
    CHECK_EQ(r.sim.c_ops, 0);
    CHECK(clean_all(&r) >= 0); /* the deleted file's sector */
    CHECK_EQ(falloc(&r, "big", big + D, NULL), 0);
    DONE(&r);
    rig_close(&r);
}

/* Each program a write or sync makes is one full page's byte count, data
 * and check. */
static bool whole_page_programs(const rig_t *r) {
    for (int i = 0; i < r->nprog && i < MAX_PROGS; i++) {
        uint32_t at = (r->paddr[i] - r->cfg.start) % PAGE, len = r->plen[i];
        if (!((at == HDR && len == D + 6) || (at == HDR + FBLOCK && len == D0 + 6)))
            return false;
    }
    return true;
}

static void if_write_pages(void) {
    for (int b = 0; b < 2; b++) {
        rig_t r;
        rig_open(&r, 8);
        r.bsize = b ? SECTOR : PAGE;
        CHECK_EQ(setup(&r), 0);
        static uint8_t d[8000], got[8000];
        fill(d, sizeof(d), 3);
        CHECK_EQ(falloc(&r, "w", sizeof(d), NULL), 0);
        alfs_file_t f;
        CHECK_EQ(fopenw(&r, &f, "w", 0), 0);
        for (uint32_t i = 0; i < sizeof(d); i += 50) {
            uint32_t before = f.used;
            CHECK_EQ(CALL(&r, alfs_file_write(&r.fs, &f, d + i, 50)), 50);
            CHECK(whole_page_programs(&r));
            if (before + 50 < r.bsize)
                CHECK_EQ(r.sim.c_programs, 0); /* no program until the block fills */
            if (i % 1000 == 950) {
                CHECK_EQ(fsyncw(&r, &f), 0);
                CHECK(whole_page_programs(&r));
                CHECK(f.used < D); /* the rest of a page waits */
                uint32_t n = 0;
                CHECK(slurp_n(&r, "w", got, sizeof(got), &n) >= 0);
                CHECK_EQ(n, whole_pages(i + 50)); /* complete pages only */
                CHECK(memcmp(got, d, n) == 0);
            }
        }
        CHECK_EQ(fclosew(&r, &f), 0);
        CHECK(reads_as(&r, "w", d, sizeof(d)));
        DONE(&r);
        rig_close(&r);
    }
}

static void if_close(void) {
    rig_t r;
    rig_open(&r, 10);
    CHECK_EQ(setup(&r), 0);
    static uint8_t d[12000];
    fill(d, sizeof(d), 4);
    uint32_t used = 20, n = D0 + (used - 1) * D; /* 20 pages, full */
    CHECK_EQ(falloc(&r, "c", D0 + 39 * D, NULL), 0); /* 40 pages: 2.5 sectors */
    uint32_t pages[64];
    CHECK_EQ(pages_of(&r, "c", pages, 64), 40);
    alfs_file_t f;
    CHECK_EQ(fopenw(&r, &f, "c", 0), 0);
    CHECK_EQ(put(&r, &f, d, n, NULL), 0);
    CHECK_EQ(fclosew(&r, &f), 0); /* CALL: no erase */
    const uint8_t *c = pg(&r, pages[0]) + CLOSE_AT;
    CHECK_EQ(get16le(c), used);
    CHECK_EQ(get32le(c + 2), n);
    CHECK_EQ(get32le(c + 6), crc32_ref(pg(&r, pages[0]) + HDR, 47));
    CHECK_EQ(get32le(pg(&r, pages[used - 1]) + 10), 0); /* the list's end */
    CHECK_EQ(get32le(pg(&r, pages[39])), get32le(pg(&r, pages[0]))); /* unused, dead, kept */
    CHECK(clean_all(&r) > 0);
    for (uint32_t i = used; i < 40; i++) {
        uint32_t k = pages[i] / PPS; /* its sector */
        bool live = false;
        for (uint32_t j = 0; j < used; j++)
            live |= pages[j] / PPS == k;
        if (live)
            CHECK_EQ(get32le(pg(&r, pages[i])), get32le(pg(&r, pages[0]))); /* shared: kept */
        else
            CHECK(erased(pg(&r, k * PPS), SECTOR)); /* erased by the cleanup */
    }
    CHECK(reads_as(&r, "c", d, n));
    alfs_file_info_t in;
    CHECK_EQ(finfo(&r, "c", &in), 0);
    CHECK(in.closed);
    CHECK_EQ(in.size, n);
    CHECK_EQ(in.pages, used);
    CHECK_EQ(remount(&r), 0);
    CHECK(reads_as(&r, "c", d, n));
    DONE(&r);
    rig_close(&r);
}

static void if_grow(void) {
    static uint8_t d[40000], got[40000];
    fill(d, sizeof(d), 5);
    {
        rig_t r; /* without ALFS_GROW: the reservation's bytes, then ENOSPC */
        rig_open(&r, 8);
        CHECK_EQ(setup(&r), 0);
        CHECK_EQ(falloc(&r, "g", D0 + D, NULL), 0);
        alfs_fs_status_t st0 = fstatus(&r);
        alfs_file_t f;
        CHECK_EQ(fopenw(&r, &f, "g", 0), 0);
        uint32_t taken = 0;
        CHECK_EQ(put(&r, &f, d, 1000, &taken), ALFS_ENOSPC);
        CHECK_EQ(taken, D0 + D);
        CHECK_EQ(r.sim.c_erases, 0);
        CHECK_EQ(fclosew(&r, &f), 0);
        CHECK(reads_as(&r, "g", d, D0 + D));
        uint32_t pages[8];
        CHECK_EQ(pages_of(&r, "g", pages, 8), 2);
        alfs_fs_status_t st1 = fstatus(&r);
        CHECK_EQ(st1.free, st0.free); /* no page taken */
        DONE(&r);
        rig_close(&r);
    }
    {
        rig_t r; /* with ALFS_GROW: free pages linked on, none erased */
        rig_open(&r, 8);
        CHECK_EQ(setup(&r), 0);
        CHECK_EQ(falloc(&r, "g", 10, NULL), 0);
        alfs_file_t f;
        CHECK_EQ(fopenw(&r, &f, "g", ALFS_GROW), 0);
        CHECK_EQ(put(&r, &f, d, 10 * D, NULL), 0);
        CHECK_EQ(fclosew(&r, &f), 0);
        CHECK(reads_as(&r, "g", d, 10 * D));
        uint32_t pages[16];
        CHECK_EQ(pages_of(&r, "g", pages, 16), pages_for(10 * D));
        DONE(&r);
        rig_close(&r);
    }
    {
        rig_t r; /* with ALFS_GROW until no page is free */
        rig_open(&r, 6);
        CHECK_EQ(setup(&r), 0);
        CHECK_EQ(write_file(&r, "other", d, D0 + 15 * D, D0 + 15 * D), 0); /* a sector of its own */
        CHECK_EQ(falloc(&r, "g", 10, NULL), 0);
        alfs_file_t f;
        CHECK_EQ(fopenw(&r, &f, "g", ALFS_GROW), 0);
        uint32_t taken = 0;
        CHECK_EQ(put(&r, &f, d, sizeof(d), &taken), ALFS_ENOSPC);
        CHECK(taken < sizeof(d));
        CHECK_EQ(fstatus(&r).free, 0);
        CHECK_EQ(fdelete(&r, "other"), 0);
        CHECK(clean_all(&r) >= 0); /* frees other's pages, and leaves g's */
        CHECK_EQ(fclosew(&r, &f), 0);
        int32_t n = slurp(&r, "g", got, sizeof(got));
        CHECK_EQ(n, (int32_t)taken);
        CHECK(memcmp(got, d, taken) == 0);
        DONE(&r);
        rig_close(&r);
    }
}

static void if_reopen_unclosed(void) {
    rig_t r;
    rig_open(&r, 8);
    CHECK_EQ(setup(&r), 0);
    static uint8_t a[2000], b[600], got[4000];
    fill(a, sizeof(a), 6);
    fill(b, sizeof(b), 7);
    uint32_t n = D0 + 2 * D + 100; /* three pages, and 100 bytes in the block */
    CHECK_EQ(falloc(&r, "u", 5000, NULL), 0);
    uint32_t pages[32];
    CHECK(pages_of(&r, "u", pages, 32) >= 8);
    alfs_file_t f;
    CHECK_EQ(fopenw(&r, &f, "u", 0), 0);
    CHECK_EQ(put(&r, &f, a, n, NULL), 0);
    CHECK_EQ(fsyncw(&r, &f), 0);
    CHECK_EQ(remount(&r), 0); /* never closed: the block's 100 bytes are lost */
    CHECK_EQ(fopenw(&r, &f, "u", 0), 0);
    CHECK_EQ(get16le(pg(&r, pages[3]) + HDR), 0); /* skipped */
    CHECK_EQ(put(&r, &f, b, sizeof(b), NULL), 0);
    CHECK_EQ(fclosew(&r, &f), 0);
    CHECK_EQ(get16le(pg(&r, pages[4]) + HDR), D); /* the writing went on here */
    for (int pass = 0; pass < 2; pass++) {
        int32_t m = slurp(&r, "u", got, sizeof(got));
        CHECK_EQ(m, (int32_t)(n - 100 + sizeof(b)));
        CHECK(memcmp(got, a, n - 100) == 0);
        CHECK(memcmp(got + n - 100, b, sizeof(b)) == 0);
        CHECK_EQ(remount(&r), 0);
    }
    DONE(&r);
    rig_close(&r);
}

static void if_flush(void) {
    rig_t r;
    rig_open(&r, 8);
    CHECK_EQ(setup(&r), 0);
    static uint8_t d[1000], got[1000];
    fill(d, sizeof(d), 37);
    CHECK_EQ(falloc(&r, "f", 3000, NULL), 0);
    uint32_t pages[16];
    CHECK_EQ(pages_of(&r, "f", pages, 16), pages_for(3000));
    alfs_file_t f;
    CHECK_EQ(fopenw(&r, &f, "f", 0), 0);
    CHECK_EQ(put(&r, &f, d, 100, NULL), 0);
    CHECK_EQ(fflushw(&r, &f), 0);
    CHECK_EQ(r.sim.c_programs, 1);                           /* one partial page */
    CHECK_EQ(get16le(pg(&r, pages[0]) + HDR + FBLOCK), 100); /* its byte count */
    CHECK_EQ(put(&r, &f, d + 100, 300, NULL), 0);
    CHECK_EQ(fflushw(&r, &f), 0); /* the file went on in the next page */
    CHECK_EQ(get16le(pg(&r, pages[1]) + HDR), D);
    CHECK_EQ(get16le(pg(&r, pages[2]) + HDR), 300 - D);
    CHECK_EQ(slurp(&r, "f", got, sizeof(got)), 400);
    CHECK(memcmp(got, d, 400) == 0);
    CHECK_EQ(remount(&r), 0); /* never closed: every flushed byte stays */
    CHECK_EQ(slurp(&r, "f", got, sizeof(got)), 400);
    CHECK(memcmp(got, d, 400) == 0);
    CHECK_EQ(fopenw(&r, &f, "f", 0), 0); /* after a mount: page 3 skipped */
    CHECK_EQ(put(&r, &f, d + 400, 50, NULL), 0);
    CHECK_EQ(fclosew(&r, &f), 0);
    CHECK_EQ(get16le(pg(&r, pages[3]) + HDR), 0);
    CHECK_EQ(get16le(pg(&r, pages[4]) + HDR), 50);
    CHECK_EQ(slurp(&r, "f", got, sizeof(got)), 450);
    CHECK(memcmp(got, d, 450) == 0);
    alfs_file_info_t in;
    CHECK_EQ(finfo(&r, "f", &in), 0);
    CHECK_EQ(in.size, 450);
    CHECK_EQ(in.pages, 5);
    DONE(&r);
    rig_close(&r);
}

static void if_read(void) {
    static const uint32_t sizes[] = {0, 1, D0 - 1, D0, D0 + 1, D0 + D, 5000, 20000};
    static uint8_t d[20000], got[20480];
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        rig_t r;
        rig_open(&r, 16);
        CHECK_EQ(setup(&r), 0);
        fill(d, sizes[i], i);
        CHECK_EQ(write_file(&r, "r", d, sizes[i], sizes[i]), 0);
        for (int pass = 0; pass < 2; pass++) {
            CHECK_EQ(slurp(&r, "r", got, sizeof(got)), (int32_t)sizes[i]);
            CHECK(memcmp(got, d, sizes[i]) == 0);
            CHECK_EQ(remount(&r), 0);
        }
        DONE(&r);
        rig_close(&r);
    }
    rig_t r; /* a file not closed: its complete pages */
    rig_open(&r, 16);
    CHECK_EQ(setup(&r), 0);
    fill(d, 5000, 99);
    CHECK_EQ(falloc(&r, "open", 5000, NULL), 0);
    alfs_file_t f;
    CHECK_EQ(fopenw(&r, &f, "open", 0), 0);
    CHECK_EQ(put(&r, &f, d, 3000, NULL), 0);
    CHECK_EQ(fsyncw(&r, &f), 0);
    CHECK_EQ(slurp(&r, "open", got, sizeof(got)), (int32_t)whole_pages(3000));
    CHECK(memcmp(got, d, whole_pages(3000)) == 0);
    CHECK_EQ(remount(&r), 0);
    CHECK_EQ(slurp(&r, "open", got, sizeof(got)), (int32_t)whole_pages(3000));
    DONE(&r);
    rig_close(&r);
}

static void if_delete(void) {
    rig_t r;
    rig_open(&r, 8);
    CHECK_EQ(setup(&r), 0);
    uint8_t a[500], b[500], got[600];
    fill(a, sizeof(a), 10);
    fill(b, sizeof(b), 11);
    CHECK_EQ(write_file(&r, "a", a, 500, 500), 0);
    CHECK_EQ(write_file(&r, "b", b, 500, 500), 0);
    uint32_t first = first_of(&r, "a");
    CHECK(first >= 1);
    CHECK_EQ(fdelete(&r, "a"), 0);
    for (int i = 0; i < 32; i++)
        CHECK_EQ(pg(&r, first)[NAME_AT + i], 0); /* blocked out */
    CHECK_EQ(slurp(&r, "a", got, sizeof(got)), ALFS_ENOENT);
    CHECK(reads_as(&r, "b", b, 500));
    CHECK_EQ(remount(&r), 0);
    CHECK_EQ(slurp(&r, "a", got, sizeof(got)), ALFS_ENOENT);
    CHECK_EQ(fdelete(&r, "a"), ALFS_ENOENT);
    alfs_file_t f;
    CHECK_EQ(falloc(&r, "w", 100, NULL), 0);
    CHECK_EQ(fopenw(&r, &f, "w", 0), 0);
    CHECK_EQ(fdelete(&r, "w"), ALFS_EOPEN);
    CHECK_EQ(fclosew(&r, &f), 0);
    alfs_reader_t rd;
    CHECK_EQ(CALL(&r, alfs_file_open_read(&r.fs, &rd, "b", r.rblk, PAGE)), 0);
    CHECK_EQ(fdelete(&r, "b"), ALFS_EOPEN);
    CHECK_EQ(CALL(&r, alfs_file_close_read(&r.fs, &rd)), 0);
    /* Every copy: an update under way, then the delete. */
    CHECK_EQ(write_file(&r, "two", a, 300, 300), 0);
    uint32_t old = first_of(&r, "two");
    CHECK_EQ(falloc(&r, "two", 300, NULL), 0); /* a new copy, not closed */
    CHECK_EQ(fdelete(&r, "two"), 0);
    CHECK_EQ(pg(&r, old)[NAME_AT], 0);
    CHECK_EQ(first_of(&r, "two"), 0);
    CHECK_EQ(remount(&r), 0);
    CHECK_EQ(slurp(&r, "two", got, sizeof(got)), ALFS_ENOENT);
    DONE(&r);
    rig_close(&r);
}

static void if_update(void) {
    rig_t r;
    rig_open(&r, 10);
    CHECK_EQ(setup(&r), 0);
    static uint8_t a[400], b[400];
    fill(a, sizeof(a), 40);
    fill(b, sizeof(b), 41);
    CHECK_EQ(write_file(&r, "cfg", a, 300, 300), 0);
    uint32_t old = first_of(&r, "cfg"), id = 0;
    uint8_t v = pg(&r, old)[VERSION_AT];
    CHECK_EQ(falloc(&r, "cfg", 400, &id), 0); /* a new copy */
    alfs_file_t f;
    CHECK_EQ(fopenw(&r, &f, "cfg", 0), 0);
    CHECK_EQ(f.id, id);
    CHECK_EQ(put(&r, &f, b, 400, NULL), 0);
    CHECK(reads_as(&r, "cfg", a, 300)); /* the old copy until the close */
    CHECK_EQ(fclosew(&r, &f), 0);
    CHECK(reads_as(&r, "cfg", b, 400));
    CHECK_EQ(pg(&r, old)[NAME_AT], 0); /* the old copy blocked out */
    uint32_t now = first_of(&r, "cfg");
    CHECK_EQ(pg(&r, now)[VERSION_AT], (uint8_t)(v + 1));
    /* An update a reboot cuts short: the old copy reads; the next update
     * blocks out the cut one. */
    CHECK_EQ(falloc(&r, "cfg", 400, NULL), 0);
    CHECK_EQ(fopenw(&r, &f, "cfg", 0), 0);
    CHECK_EQ(put(&r, &f, a, 300, NULL), 0);
    CHECK_EQ(remount(&r), 0);
    CHECK(reads_as(&r, "cfg", b, 400));
    uint32_t cut = 0, pages[4];
    for (uint32_t p = 1; p < r.cfg.size / PAGE && !cut; p++)
        if (p != now && list_of(&r, p, pages, 4) && get16le(pg(&r, p) + 4) == 0 &&
            !strncmp((const char *)pg(&r, p) + NAME_AT, "cfg", 32))
            cut = p;
    CHECK(cut != 0);
    CHECK_EQ(write_file(&r, "cfg", a, 300, 300), 0);
    CHECK_EQ(pg(&r, cut)[NAME_AT], 0);
    CHECK(reads_as(&r, "cfg", a, 300));
    /* 300 updates: the version wraps, and the newest is always read. */
    for (int i = 0; i < 300; i++) {
        fill(b, 200, (uint32_t)i);
        int rc = write_file(&r, "cfg", b, 200, 200);
        if (rc == ALFS_ENOSPC) {
            CHECK(clean_all(&r) >= 0);
            rc = write_file(&r, "cfg", b, 200, 200);
        }
        CHECK_EQ(rc, 0);
        CHECK(reads_as(&r, "cfg", b, 200));
        if (i % 37 == 36) {
            CHECK_EQ(remount(&r), 0);
            CHECK(reads_as(&r, "cfg", b, 200));
        }
    }
    DONE(&r);
    rig_close(&r);
}

static void if_list(void) {
    rig_t r;
    rig_open(&r, 8);
    CHECK_EQ(setup(&r), 0);
    uint8_t d[1000];
    fill(d, sizeof(d), 12);
    uint32_t id[3];
    CHECK_EQ(write_file(&r, "one", d, 100, 100), 0);
    CHECK_EQ(write_file(&r, "two", d, 700, 700), 0);
    CHECK_EQ(falloc(&r, "three", 300, NULL), 0); /* not closed */
    for (int pass = 0; pass < 2; pass++) {
        static const char *const names[] = {"one", "two", "three"};
        static const uint32_t sizes[] = {100, 700, ALFS_SIZE_OPEN};
        alfs_file_info_t in;
        uint32_t after = 0;
        for (int i = 0; i < 3; i++) {
            CHECK_EQ(CALL(&r, alfs_file_list(&r.fs, after, &in)), 0);
            CHECK(strcmp(in.name, names[i]) == 0);
            CHECK(in.id > after);
            CHECK_EQ(in.closed, i < 2);
            CHECK_EQ(in.size, sizes[i]);
            CHECK_EQ(in.version, pg(&r, first_of(&r, names[i]))[VERSION_AT]);
            if (i < 2)
                CHECK_EQ(in.pages, pages_for(sizes[i]));
            id[i] = in.id;
            after = in.id;
        }
        CHECK_EQ(CALL(&r, alfs_file_list(&r.fs, after, &in)), ALFS_ENOENT);
        CHECK(id[0] < id[1] && id[1] < id[2]);
        CHECK_EQ(remount(&r), 0);
    }
    DONE(&r);
    rig_close(&r);
}

/* Every frame ID seen, never twice. */
typedef struct {
    uint32_t n[512];
    int count;
} ids_t;

static bool note(ids_t *s, uint32_t v) {
    for (int i = 0; i < s->count; i++)
        if (s->n[i] == v)
            return false;
    if (s->count < 512)
        s->n[s->count++] = v;
    return true;
}

static void if_ids_never_reused(void) {
    rig_t r;
    rig_open(&r, 16);
    CHECK_EQ(setup(&r), 0);
    ids_t seen = {{0}, 0};
    uint32_t last = 0, id = 0;
    uint8_t d[100];
    fill(d, sizeof(d), 13);
    for (int i = 0; i < 6; i++) {
        CHECK_EQ(falloc(&r, "x", 100, &id), 0);
        CHECK(id > last);
        CHECK(note(&seen, id));
        last = id;
        CHECK_EQ(fdelete(&r, "x"), 0);
        if (i % 2)
            CHECK_EQ(remount(&r), 0);
    }
    static const sim_pl_t torn[] = {PL_FLICKER, PL_METASTABLE, PL_SOMEBITS};
    for (int t = 0; t < 3; t++) {
        sim_power_after(&r.sim, 0, torn[t]); /* the entry's program */
        falloc(&r, "torn", 100, &id);
        CHECK(r.sim.off);
        sim_reboot(&r.sim);
        for (int m = 0; m < 8; m++) {
            CHECK_EQ(remount(&r), 0);
            alfs_file_info_t in;
            uint32_t after = 0;
            while (CALL(&r, alfs_file_list(&r.fs, after, &in)) == 0) {
                after = in.id;
                note(&seen, in.id); /* listed: may be one given already */
            }
            char name[8];
            snprintf(name, sizeof(name), "m%d%d", t, m);
            CHECK_EQ(falloc(&r, name, 100, &id), 0);
            if (!note(&seen, id))
                printf("    torn %d, remount %d: frame ID %u given twice\n", t, m, id);
            CHECK(seen.n[seen.count - 1] == id);
            CHECK_EQ(fdelete(&r, name), 0);
        }
    }
    DONE(&r);
    rig_close(&r);
}

static void if_cleanup(void) {
    rig_t r;
    rig_open(&r, 10);
    CHECK_EQ(setup(&r), 0);
    CHECK_EQ(clean_all(&r), 0); /* nothing to do */
    static uint8_t d[9000];
    fill(d, sizeof(d), 14);
    CHECK_EQ(write_file(&r, "gone", d, 9000, 9000), 0);
    CHECK_EQ(write_file(&r, "kept", d, 2000, 2000), 0);
    uint32_t gone[64], kept[16];
    uint32_t ng = pages_of(&r, "gone", gone, 64), nk = pages_of(&r, "kept", kept, 16);
    CHECK_EQ(fdelete(&r, "gone"), 0);
    CHECK(!fstatus(&r).clean);
    int steps = clean_all(&r);
    CHECK(steps > 0);
    for (uint32_t i = 0; i < ng; i++) {
        bool shared = false;
        for (uint32_t j = 0; j < nk; j++)
            shared |= kept[j] / PPS == gone[i] / PPS;
        if (!shared)
            CHECK(erased(pg(&r, gone[i] / PPS * PPS), SECTOR));
    }
    CHECK(reads_as(&r, "kept", d, 2000));
    alfs_fs_status_t st = fstatus(&r);
    CHECK_EQ(st.live, nk);
    CHECK_EQ(st.free + st.dead + st.live, st.pages);
    CHECK_EQ(ECALL(&r, alfs_fs_cleanup(&r.fs, 1)), ALFS_CLEAN);
    CHECK_EQ(r.sim.c_erases, 0);
    DONE(&r);
    rig_close(&r);
}

static void if_refusals(void) {
    rig_t r;
    rig_open(&r, 10);
    CHECK_EQ(setup(&r), 0);
    static uint8_t d[3000], got[4000];
    fill(d, sizeof(d), 15);
    CHECK_EQ(write_file(&r, "old", d, 1000, 1000), 0);
    CHECK_EQ(falloc(&r, "w", 3000, NULL), 0);
    alfs_file_t f, f2;
    CHECK_EQ(fopenw(&r, &f, "w", 0), 0);
    CHECK_EQ(put(&r, &f, d, 1000, NULL), 0);
    CHECK_EQ(fopenw(&r, &f2, "w", 0), ALFS_EOPEN);
    CHECK_EQ(fmount(&r), ALFS_EOPEN);
    CHECK_EQ(r.sim.c_ops, 0);
    /* Others' calls leave the open file as it was. */
    CHECK_EQ(write_file(&r, "new", d, 500, 500), 0);
    CHECK_EQ(fdelete(&r, "old"), 0);
    CHECK(clean_all(&r) >= 0);
    CHECK_EQ(put(&r, &f, d + 1000, 2000, NULL), 0);
    CHECK_EQ(fclosew(&r, &f), 0);
    CHECK_EQ(slurp(&r, "w", got, sizeof(got)), 3000);
    CHECK(memcmp(got, d, 3000) == 0);
    CHECK(reads_as(&r, "new", d, 500));
    CHECK_EQ(remount(&r), 0);
    DONE(&r);
    rig_close(&r);
}

/* ---- SPD and LIFE ------------------------------------------------------- */

/*
 * A logging run into an open file: rows produced at `rate` a second,
 * written every 200 ms, synced (or flushed) every second, `ms` long, with a
 * cleanup step every 20 ms if asked. Rows go to exp[]; synced is the length
 * the last successful sync or flush covered. Stops early when the power is lost. 0, or
 * the first error that is not a power loss.
 */
typedef struct {
    uint8_t *exp;
    uint32_t len, cap;
    uint32_t synced;
    uint32_t seq;
    bool flush; /* each second a flush, not a sync */
} run_t;

static void run_init(run_t *lg, uint32_t cap) {
    memset(lg, 0, sizeof(*lg));
    lg->exp = malloc(cap);
    lg->cap = cap;
}

static int log_run(rig_t *r, alfs_file_t *f, run_t *lg, uint32_t rate, uint32_t ms, bool steps) {
    uint32_t due = 0;
    for (uint32_t t = 20; t <= ms; t += 20) {
        if (steps) {
            int rc = (int)ECALL(r, alfs_fs_cleanup(&r->fs, 1));
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
            int rc = put(r, f, lg->exp + lg->len, n * ROW, &taken);
            lg->len += taken;
            if (r->sim.off)
                return 0;
            if (rc)
                return rc;
        }
        if (t % 1000 == 0) {
            int rc = lg->flush ? fflushw(r, f) : fsyncw(r, f);
            if (r->sim.off)
                return 0;
            if (rc)
                return rc;
            lg->synced = lg->len;
        }
    }
    return 0;
}

/* W-CFG, step i: the file it updates, or deletes (size 0). */
typedef struct {
    const char *name;
    uint32_t size, gen;
} cfg_op_t;

static cfg_op_t cfg_step(int i) {
    static const cfg_op_t script[] = {
        {"cfg", 300, 1}, {"lua/main.lua", 4096, 1}, {"lua/beep.lua", 300, 1}, {"cfg", 300, 2},
        {"lua/beep.lua", 0, 0}, {"cfg", 300, 3}, {"lua/beep.lua", 300, 2}, {"lua/main.lua", 4096, 2},
    };
    cfg_op_t op = script[i % 8];
    if (op.size)
        op.gen += (uint32_t)(i / 8) * 10u;
    return op;
}

/* An update: a new copy written, which replaces the old at its close; or a
 * delete. With too few free pages, a cleanup first. */
static int cfg_do(rig_t *r, cfg_op_t op) {
    static uint8_t d[4096];
    if (!op.size) {
        int rc = fdelete(r, op.name);
        return rc == ALFS_ENOENT ? 0 : rc;
    }
    fill(d, op.size, op.gen);
    int rc = write_file(r, op.name, d, op.size, op.size);
    if (rc == ALFS_ENOSPC && clean_all(r) >= 0)
        rc = write_file(r, op.name, d, op.size, op.size);
    return rc;
}

static void spd_no_erase_outside_erase(void) {
    for (int rate = 0; rate < 2; rate++) {
        rig_t r;
        rig_open(&r, 66);
        CHECK_EQ(setup(&r), 0);
        CHECK_EQ(falloc(&r, "log", 160000, NULL), 0);
        run_t lg;
        run_init(&lg, 160000);
        alfs_file_t f;
        CHECK_EQ(fopenw(&r, &f, "log", 0), 0);
        CHECK_EQ(log_run(&r, &f, &lg, rate ? 50 : 1, 120000, false), 0); /* CALL: no erase */
        CHECK_EQ(remount(&r), 0); /* a reboot, and a reopen: no erase */
        CHECK_EQ(fopenw(&r, &f, "log", 0), 0);
        CHECK_EQ(log_run(&r, &f, &lg, rate ? 50 : 1, 20000, false), 0);
        CHECK_EQ(fclosew(&r, &f), 0);
        for (int i = 0; i < 16; i++)
            CHECK_EQ(cfg_do(&r, cfg_step(i)), 0);
        static uint8_t got[160000];
        CHECK(slurp(&r, "log", got, sizeof(got)) > 0); /* CALL: no erase */
        fstatus(&r);
        free(lg.exp);
        DONE(&r);
        rig_close(&r);
    }
}

static void spd_write_programs_bounded(void) {
    for (int b = 0; b < 2; b++) {
        rig_t r;
        rig_open(&r, 200);
        r.bsize = b ? SECTOR : PAGE;
        CHECK_EQ(setup(&r), 0);
        CHECK_EQ(falloc(&r, "hr", 480u * 1500u, NULL), 0);
        alfs_file_t f;
        CHECK_EQ(fopenw(&r, &f, "hr", 0), 0);
        static uint8_t d[480];
        for (uint32_t t = 0; t < 1500; t++) { /* 30 s of W-HR */
            fill(d, sizeof(d), t);
            uint32_t before = f.used;
            CHECK_EQ(CALL(&r, alfs_file_write(&r.fs, &f, d, sizeof(d))), 480);
            CHECK(whole_page_programs(&r));
            if (before + 480 < r.bsize)
                CHECK_EQ(r.sim.c_programs, 0); /* the block did not fill */
            CHECK(r.sim.c_programs <= (before + 480) / D0);
            if (t % 50 == 49) {
                before = f.used;
                CHECK_EQ(fsyncw(&r, &f), 0);
                CHECK(whole_page_programs(&r));
                CHECK(r.sim.c_programs <= before / D0);
                CHECK(f.used < D); /* the rest of a page waits */
            }
        }
        CHECK_EQ(fclosew(&r, &f), 0);
        DONE(&r);
        rig_close(&r);
    }
}

static void spd_mount_reads_bounded(void) {
    rig_t r;
    rig_open(&r, 128);
    CHECK_EQ(setup(&r), 0);
    static uint8_t d[20000];
    fill(d, sizeof(d), 16);
    for (int i = 0; i < 20; i++) {
        char name[8];
        snprintf(name, sizeof(name), "f%d", i);
        CHECK_EQ(write_file(&r, name, d, 1000u * (uint32_t)(i % 7), 20000), 0);
        if (i % 3 == 2)
            CHECK_EQ(fdelete(&r, name), 0);
    }
    uint32_t firsts = 0; /* pages holding a frame block, deleted files' too */
    for (uint32_t p = 1; p < r.cfg.size / PAGE; p++) {
        const uint8_t *h = pg(&r, p);
        firsts += get32le(h + 6) == crc32_ref(h, 6) && get16le(h + 4) == 0;
    }
    CHECK_EQ(remount(&r), 0);
    CHECK(r.sim.c_bytes_read <= (r.cfg.size / PAGE) * HDR + firsts * FBLOCK);
    CHECK_EQ(r.sim.c_ops, 0);
    DONE(&r);
    rig_close(&r);
}

static void spd_wlog_longest_stall(void) {
    rig_t r;
    rig_open(&r, 260);
    CHECK_EQ(setup(&r), 0);
    CHECK_EQ(falloc(&r, "log", 700000, NULL), 0);
    run_t lg;
    run_init(&lg, 700000);
    alfs_file_t f;
    CHECK_EQ(fopenw(&r, &f, "log", 0), 0);
    CHECK_EQ(log_run(&r, &f, &lg, 50, 600000, false), 0);
    CHECK(longest_stall <= SIM_T_PROG_US);
    CHECK_EQ(fclosew(&r, &f), 0);
    free(lg.exp);
    DONE(&r);
    rig_close(&r);
}

/* The bytes read by the writes and syncs of the same 30 s of W-HR into a
 * reserved file. */
static uint64_t hr_reads(uint32_t sectors) {
    rig_t r;
    rig_open(&r, sectors);
    uint64_t total = 0;
    alfs_file_t f;
    if (!setup(&r) && !falloc(&r, "hr", 96000, NULL) && !fopenw(&r, &f, "hr", 0)) {
        static uint8_t d[480];
        for (uint32_t t = 0; t < 200; t++) {
            fill(d, sizeof(d), t);
            CALL(&r, alfs_file_write(&r.fs, &f, d, sizeof(d)));
            total += r.sim.c_bytes_read;
            if (t % 50 == 49) {
                fsyncw(&r, &f);
                total += r.sim.c_bytes_read;
            }
        }
    }
    rig_close(&r);
    return total;
}

static void spd_cpu_linear(void) {
    uint64_t small = hr_reads(34), large = hr_reads(2048);
    if (small != large)
        printf("    reads: %llu on 34 sectors, %llu on 2048\n", (unsigned long long)small,
               (unsigned long long)large);
    CHECK_EQ(small, large);
}

static void spd_open_reads_bounded(void) {
    static const uint32_t npages[] = {1, 10, 100};
    for (int i = 0; i < 3; i++) {
        rig_t r;
        rig_open(&r, 16);
        CHECK_EQ(setup(&r), 0);
        uint32_t bytes = D0 + (npages[i] - 1) * D;
        static uint8_t d[D0 + 99 * D];
        fill(d, bytes, 17);
        CHECK_EQ(falloc(&r, "o", bytes, NULL), 0);
        alfs_file_t f;
        CHECK_EQ(fopenw(&r, &f, "o", 0), 0);
        CHECK(r.sim.c_bytes_read <= PAGE + npages[i] * (2 * HDR + 2));
        uint32_t walk = npages[i] * (2 * HDR + 2);
        CHECK_EQ(put(&r, &f, d, bytes, NULL), 0);
        CHECK_EQ(fsyncw(&r, &f), 0);
        alfs_reader_t rd;
        CHECK_EQ(CALL(&r, alfs_file_open_read(&r.fs, &rd, "o", r.rblk, PAGE)), 0);
        CHECK(r.sim.c_bytes_read <= PAGE);
        CHECK_EQ(CALL(&r, alfs_file_close_read(&r.fs, &rd)), 0);
        CHECK_EQ(remount(&r), 0);
        CHECK_EQ(fopenw(&r, &f, "o", 0), 0); /* a reopen: a byte count a page, and one page */
        CHECK(r.sim.c_bytes_read <= 2 * PAGE + walk + npages[i] * 2u);
        CHECK_EQ(fclosew(&r, &f), 0);
        DONE(&r);
        rig_close(&r);
    }
}

/*
 * W-RING: W-LOG at 50 rows a second into a file of its own each `every`
 * rows, the oldest file deleted while fewer than two files' worth of pages
 * are free, a cleanup step every 20 ms; until every sector has been erased
 * `laps` times, `max_ms` passes, or worn sectors leave no room for a file.
 * Each file is read back whole after its close. 0, or the first error.
 */
static int ring(rig_t *r, uint32_t every, uint32_t laps, uint64_t max_ms) {
    uint32_t per_file = pages_for(every * ROW), oldest = 0, newest = 0, seq = 0, in_file = 0;
    uint32_t sectors = r->cfg.size / SECTOR;
    uint8_t rows[10 * ROW];
    static uint8_t exp[65536], got[65536];
    uint32_t len = 0;
    alfs_file_t f;
    char name[16];
    snprintf(name, sizeof(name), "f%u", newest);
    if (falloc(r, name, every * ROW, NULL) || fopenw(r, &f, name, 0))
        return -1;
    for (uint64_t t = 20; t < max_ms; t += 20) {
        alfs_fs_status_t st = fstatus(r);
        if (st.free < 2 * per_file && oldest < newest) {
            snprintf(name, sizeof(name), "f%u", oldest++);
            if (fdelete(r, name))
                return -2;
        }
        int rc = (int)ECALL(r, alfs_fs_cleanup(&r->fs, 1));
        if (rc < 0 && rc != ALFS_EIO)
            return rc;
        if (t % 200)
            continue;
        for (int i = 0; i < 10; i++)
            make_row(seq++, rows + i * ROW);
        uint32_t taken = 0;
        rc = put(r, &f, rows, sizeof(rows), &taken);
        if (rc)
            return rc;
        if (len + taken <= sizeof(exp))
            memcpy(exp + len, rows, taken);
        len += taken;
        in_file += 10;
        if (t % 1000 == 0 && fsyncw(r, &f))
            return -3;
        if (in_file >= every) {
            if (fclosew(r, &f))
                return -4;
            snprintf(name, sizeof(name), "f%u", newest);
            if (slurp(r, name, got, sizeof(got)) != (int32_t)len || memcmp(got, exp, len))
                return -5;
            snprintf(name, sizeof(name), "f%u", ++newest);
            rc = falloc(r, name, every * ROW, NULL);
            if (rc == ALFS_ENOSPC && clean_all(r) >= 0)
                rc = falloc(r, name, every * ROW, NULL);
            if (rc == ALFS_ENOSPC)
                return 0; /* worn: the area no longer holds a file */
            if (rc || fopenw(r, &f, name, 0))
                return -6;
            in_file = len = 0;
        }
        uint32_t least = UINT32_MAX;
        for (uint32_t k = 0; k < sectors; k++) {
            uint32_t idx = r->cfg.start / SECTOR + k;
            if (r->sim.bad[idx] == BAD_NONE && r->sim.wear[idx] < least)
                least = r->sim.wear[idx];
        }
        if (least >= laps)
            break;
    }
    fclosew(r, &f);
    return 0;
}

static void life_erase_only_unused(void) {
    rig_t r;
    rig_open(&r, 34);
    CHECK_EQ(setup(&r), 0);
    CHECK_EQ(ring(&r, 1500, 3, 10000000), 0); /* each file read back whole */
    for (int i = 0; i < 12; i++)
        CHECK_EQ(cfg_do(&r, cfg_step(i)), 0);
    DONE(&r); /* no erase of a sector that read erased */
    rig_close(&r);
}

static void life_write_amplification(void) {
    for (int kind = 0; kind < 3; kind++) {
        rig_t r;
        rig_open(&r, 260);
        CHECK_EQ(setup(&r), 0);
        uint32_t per_sync = kind == 0 ? 22 : kind == 1 ? 1100 : 24000, syncs = kind == 2 ? 30u : 300u;
        uint32_t total = per_sync * syncs, pages = pages_for(total);
        uint64_t b0 = r.sim.bytes_programmed;
        CHECK_EQ(falloc(&r, "a", total, NULL), 0);
        CHECK_EQ(r.sim.bytes_programmed - b0, pages * HDR - 4u + 41u); /* the last link left erased */
        alfs_file_t f;
        CHECK_EQ(fopenw(&r, &f, "a", 0), 0);
        static uint8_t d[24000];
        b0 = r.sim.bytes_programmed;
        for (uint32_t s = 0; s < syncs; s++) {
            fill(d, per_sync, s);
            CHECK_EQ(put(&r, &f, d, per_sync, NULL), 0);
            CHECK_EQ(fsyncw(&r, &f), 0);
        }
        uint32_t full = whole_pages(total), npages = full ? 1u + (full - D0) / D : 0u;
        CHECK_EQ(r.sim.bytes_programmed - b0, full + 6u * npages);
        b0 = r.sim.bytes_programmed;
        CHECK_EQ(fclosew(&r, &f), 0);
        uint32_t rest = total - full;
        CHECK_EQ(r.sim.bytes_programmed - b0, (rest ? rest + 6u : 0u) + 10u + 4u);
        DONE(&r);
        rig_close(&r);
    }
}

static void life_wlog_erases(void) {
    rig_t r;
    uint32_t bytes = 50u * 600u * ROW, pages = pages_for(bytes); /* ten minutes at 50 rows a second */
    rig_open(&r, pages / PPS + 6);
    CHECK_EQ(setup(&r), 0);
    uint64_t e0 = r.sim.erases;
    CHECK_EQ(falloc(&r, "log", bytes, NULL), 0);
    run_t lg;
    run_init(&lg, bytes);
    alfs_file_t f;
    CHECK_EQ(fopenw(&r, &f, "log", 0), 0);
    CHECK_EQ(log_run(&r, &f, &lg, 50, 600000, false), 0);
    CHECK_EQ(fclosew(&r, &f), 0);
    CHECK_EQ(fdelete(&r, "log"), 0);
    CHECK(clean_all(&r) >= 0);
    uint64_t erases = r.sim.erases - e0, filled = (pages + PPS - 1) / PPS;
    printf("    W-LOG: %llu sector erases for %llu sectors filled\n", (unsigned long long)erases,
           (unsigned long long)filled);
    CHECK(erases <= filled + 1);
    free(lg.exp);
    DONE(&r);
    rig_close(&r);
}

static void life_wear_level(void) {
    rig_t r;
    rig_open(&r, 34);
    CHECK_EQ(setup(&r), 0);
    CHECK_EQ(ring(&r, 1500, 10, 100000000), 0);
    uint32_t lo = UINT32_MAX, hi = 0;
    for (uint32_t k = 0; k < 34; k++) {
        uint32_t wv = r.sim.wear[r.cfg.start / SECTOR + k];
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

/* Power lost at operation k of a W-LOG run into a reserved file, synced or
 * flushed each second, with a closed file beside it; then remounts and
 * reads. Sets *past when k is past the run's last operation. */
static void powerloss_case(sim_pl_t pl, int64_t k, bool flush, int *past) {
    rig_t r;
    rig_open(&r, 12);
    *past = 1;
    CHECK_EQ(setup(&r), 0);
    static uint8_t cfg[300], got[40000];
    fill(cfg, sizeof(cfg), 18);
    CHECK_EQ(write_file(&r, "cfg", cfg, sizeof(cfg), sizeof(cfg)), 0);
    CHECK_EQ(falloc(&r, "log", 30000, NULL), 0);
    alfs_file_t f;
    CHECK_EQ(fopenw(&r, &f, "log", 0), 0);
    *past = 0;
    run_t lg;
    run_init(&lg, 30000);
    lg.flush = flush;
    sim_power_after(&r.sim, k, pl);
    int rc = log_run(&r, &f, &lg, 50, 20000, false);
    if (!r.sim.off)
        rc = rc ? rc : fclosew(&r, &f);
    if (!r.sim.off) {
        *past = 1;
        CHECK_EQ(rc, 0);
        free(lg.exp);
        rig_close(&r);
        return;
    }
    sim_reboot(&r.sim);
    for (int m = 0; m < 3; m++) {
        CHECK_EQ(remount(&r), 0);
        CHECK(reads_as(&r, "cfg", cfg, sizeof(cfg))); /* SAFE-01 */
        int32_t n = slurp(&r, "log", got, sizeof(got));
        uint32_t kept = flush ? lg.synced : whole_pages(lg.synced); /* SAFE-02 */
        bool ok = n >= (int32_t)kept && n <= (int32_t)lg.len && !memcmp(got, lg.exp, (size_t)n);
        if (!ok)
            printf("    pl %d k %lld: read %d of %u (synced %u)\n", pl, (long long)k, n, lg.len, lg.synced);
        CHECK(ok); /* SAFE-02 */
    }
    free(lg.exp);
    DONE(&r);
    rig_close(&r);
}

static void powerloss_all(bool flush) {
    for (int pl = 0; pl < PL_COUNT; pl++)
        for (int64_t k = 0;; k++) {
            int past = 0;
            powerloss_case((sim_pl_t)pl, k, flush, &past);
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

/* SAFE-04: power lost at operation k of writing; reopened, written on,
 * closed. What reads back is what was written before the loss, as far as
 * some page, then what was written after, whole; and the simulator sees no
 * program into a torn byte. */
static void safe_reopen_after_loss(void) {
    for (int pl = 0; pl < PL_COUNT; pl++)
        for (int64_t k = 0;; k++) {
            rig_t r;
            rig_open(&r, 10);
            CHECK_EQ(setup(&r), 0);
            static uint8_t a[6000], b[2000], got[9000];
            fill(a, sizeof(a), 19);
            fill(b, sizeof(b), 20);
            CHECK_EQ(falloc(&r, "log", 10000, NULL), 0);
            alfs_file_t f;
            CHECK_EQ(fopenw(&r, &f, "log", 0), 0);
            sim_power_after(&r.sim, k, (sim_pl_t)pl);
            uint32_t written = 0, synced = 0;
            for (uint32_t i = 0; i < sizeof(a) && !r.sim.off; i += 300) {
                uint32_t taken = 0;
                put(&r, &f, a + i, 300, &taken);
                written += taken;
                if (!r.sim.off && i % 900 == 600 && fsyncw(&r, &f) == 0 && !r.sim.off)
                    synced = written;
            }
            bool past = !r.sim.off;
            sim_reboot(&r.sim);
            if (past) {
                rig_close(&r);
                break;
            }
            CHECK_EQ(remount(&r), 0);
            CHECK_EQ(fopenw(&r, &f, "log", 0), 0);
            CHECK_EQ(put(&r, &f, b, sizeof(b), NULL), 0);
            CHECK_EQ(fclosew(&r, &f), 0);
            for (int m = 0; m < 3; m++) {
                CHECK_EQ(remount(&r), 0);
                uint32_t n = 0;
                int32_t rc = slurp_n(&r, "log", got, sizeof(got), &n);
                CHECK(rc >= 0 || rc == ALFS_ECHECK);
                /* a's bytes to some page's end, at least the synced, then b's, whole */
                bool ok = false;
                uint32_t pre = 0;
                for (uint32_t p = 0; !ok && p <= n && p <= written; p = p ? p + D : D0) {
                    pre = p;
                    ok = p >= whole_pages(synced) && !memcmp(got, a, p) && n - p == sizeof(b) &&
                         !memcmp(got + p, b, sizeof(b));
                }
                if (!ok)
                    printf("    pl %d k %lld, remount %d: %u read, %u before, synced %u\n", pl, (long long)k, m, n, pre,
                           synced);
                CHECK(ok);
            }
            DONE(&r);
            rig_close(&r);
            if (tf_failed) {
                printf("    behaviour %d, power lost at operation %lld\n", pl, (long long)k);
                return;
            }
        }
}

static void safe_no_false_data(void) {
    for (int trial = 0; trial < 200; trial++) {
        rig_t r;
        rig_open(&r, 8);
        CHECK_EQ(setup(&r), 0);
        static uint8_t d[3000], got[4000];
        fill(d, sizeof(d), (uint32_t)trial);
        CHECK_EQ(falloc(&r, "f", sizeof(d), NULL), 0);
        alfs_file_t f;
        CHECK_EQ(fopenw(&r, &f, "f", 0), 0);
        CHECK_EQ(put(&r, &f, d, sizeof(d), NULL), 0);
        CHECK_EQ(fsyncw(&r, &f), 0); /* not closed: no frame checksum to fail */
        uint32_t pages[16], np = pages_of(&r, "f", pages, 16);
        uint32_t written = whole_pages(sizeof(d)), full = written <= D0 ? 1u : 1u + (written - D0) / D;
        CHECK(np >= full);
        uint32_t hit = sim_rand(&r.sim) % full, h = hit ? HDR : HDR + FBLOCK, n = hit ? D : D0;
        uint32_t at = h + sim_rand(&r.sim) % (2 + n + 4); /* its byte count, data or check */
        sim_flip(&r.sim, r.cfg.start + pages[hit] * PAGE + at, (uint8_t)(1u << (sim_rand(&r.sim) % 8)));
        alfs_fs_status_t st = fstatus(&r);
        uint32_t fails = st.n.check_failures;
        int32_t m = slurp(&r, "f", got, sizeof(got));
        uint32_t off = hit ? D0 + (hit - 1) * D : 0;
        CHECK_EQ(m, (int32_t)(written - n));
        CHECK(memcmp(got, d, off) == 0);
        CHECK(memcmp(got + off, d + off + n, written - off - n) == 0);
        st = fstatus(&r);
        CHECK_EQ(st.n.check_failures, fails + 1);
        DONE(&r);
        rig_close(&r);
    }
}

static void safe_write_only_verified_erased(void) {
    rig_t r;
    rig_open(&r, 10);
    uint32_t bad = r.cfg.start / SECTOR + 5;
    sim_mkbad(&r.sim, bad, BAD_ERASENOOP);
    CHECK_EQ(setup(&r), 0);
    CHECK_EQ(fstatus(&r).bad, 1);
    static uint8_t d[30000];
    fill(d, sizeof(d), 21);
    for (int i = 0; i < 6; i++) {
        char name[8];
        snprintf(name, sizeof(name), "f%d", i);
        if (write_file(&r, name, d, 3000, 3000))
            break;
        uint32_t pages[32], np = pages_of(&r, name, pages, 32);
        for (uint32_t j = 0; j < np; j++)
            CHECK(pages[j] / PPS != bad - r.cfg.start / SECTOR);
    }
    CHECK_EQ(r.sim.sector_progs[bad], 0);
    DONE(&r);
    rig_close(&r);
}

static void safe_prog_failure(void) {
    static const sim_bad_t modes[] = {BAD_PROGERROR, BAD_PROGNOOP, BAD_PROGFLIP};
    for (int m = 0; m < 3; m++) {
        rig_t r;
        rig_open(&r, 16);
        CHECK_EQ(setup(&r), 0);
        CHECK_EQ(falloc(&r, "log", 40000, NULL), 0);
        run_t lg;
        run_init(&lg, 40000);
        alfs_file_t f;
        CHECK_EQ(fopenw(&r, &f, "log", 0), 0);
        CHECK_EQ(log_run(&r, &f, &lg, 50, 5000, false), 0);
        uint32_t pages[512], np = pages_of(&r, "log", pages, 512);
        uint32_t next = pages[whole_pages(lg.len) <= D0 ? 1 : 1 + (whole_pages(lg.len) - D0) / D];
        CHECK(np > 4);
        sim_mkbad(&r.sim, r.cfg.start / SECTOR + next / PPS, modes[m]); /* the sector being written */
        CHECK_EQ(log_run(&r, &f, &lg, 50, 20000, false), 0);
        CHECK_EQ(fsyncw(&r, &f), 0);
        static uint8_t got[40000];
        uint32_t n = 0;
        slurp_n(&r, "log", got, sizeof(got), &n);
        alfs_fs_status_t st = fstatus(&r);
        uint32_t want = whole_pages(lg.len);
        if (n + st.n.bytes_lost != want)
            printf("    mode %d: read %u + lost %u != written %u\n", modes[m], n, st.n.bytes_lost, want);
        CHECK_EQ(n + st.n.bytes_lost, want);
        CHECK(st.n.prog_failures >= 1);
        CHECK(runs_cut(got, n, lg.exp, want));
        free(lg.exp);
        DONE(&r);
        rig_close(&r);
    }
    rig_t r; /* a header that fails during an allocation */
    rig_open(&r, 8);
    CHECK_EQ(setup(&r), 0);
    sim_mkbad(&r.sim, r.cfg.start / SECTOR + 2, BAD_PROGERROR); /* where the search starts */
    static uint8_t d[3000];
    fill(d, sizeof(d), 22);
    CHECK_EQ(write_file(&r, "a", d, sizeof(d), sizeof(d)), 0);
    CHECK(reads_as(&r, "a", d, sizeof(d)));
    DONE(&r);
    rig_close(&r);
}

static void safe_bad_sector(void) {
    static const sim_bad_t modes[] = {BAD_ERASEERROR, BAD_ERASENOOP};
    for (int m = 0; m < 2; m++) {
        rig_t r;
        rig_open(&r, 8);
        uint32_t bad = r.cfg.start / SECTOR + 4;
        sim_mkbad(&r.sim, bad, modes[m]);
        CHECK_EQ(setup(&r), 0);
        alfs_fs_status_t st = fstatus(&r);
        CHECK_EQ(st.bad, 1);
        CHECK_EQ(st.free, 7 * PPS - 1);
        static uint8_t d[30000];
        fill(d, sizeof(d), 23);
        uint32_t all = D0 + (7 * PPS - 2) * D; /* every free page */
        CHECK_EQ(write_file(&r, "a", d, all, all), 0);
        CHECK_EQ(r.sim.sector_progs[bad], 0);
        CHECK_EQ(remount(&r), 0);
        CHECK(reads_as(&r, "a", d, all));
        DONE(&r);
        rig_close(&r);
    }
    rig_t r; /* wear-out under W-RING: the area shrinks, writing goes on */
    rig_open(&r, 34);
    r.sim.wear_limit = 6;
    CHECK_EQ(setup(&r), 0);
    CHECK_EQ(ring(&r, 1500, 20, 3000000), 0);
    CHECK(fstatus(&r).bad > 0);
    DONE(&r);
    rig_close(&r);
}

static void safe_read_retry(void) {
    rig_t r;
    rig_open(&r, 10);
    CHECK_EQ(setup(&r), 0);
    static uint8_t d[9000], got[9000];
    fill(d, sizeof(d), 24);
    CHECK_EQ(write_file(&r, "f", d, sizeof(d), sizeof(d)), 0);
    memset(&r.fs, 0, sizeof(r.fs));
    r.sim.read_fail_once = 1;
    CHECK_EQ(fmount(&r), ALFS_EIO);
    CHECK_EQ(fmount(&r), 0);
    alfs_reader_t rd;
    r.sim.read_fail_once = 1;
    CHECK_EQ(CALL(&r, alfs_file_open_read(&r.fs, &rd, "f", r.rblk, PAGE)), ALFS_EIO);
    CHECK_EQ(CALL(&r, alfs_file_open_read(&r.fs, &rd, "f", r.rblk, PAGE)), 0);
    uint32_t n = 0;
    for (int i = 0; n < sizeof(d) && i < 100000; i++) {
        if (i % 3 == 1)
            r.sim.read_fail_once = 1;
        int32_t k = (int32_t)CALL(&r, alfs_file_read(&r.fs, &rd, got + n, 300));
        if (k == ALFS_EIO)
            continue;
        CHECK(k > 0);
        n += (uint32_t)k;
    }
    CHECK_EQ(CALL(&r, alfs_file_read(&r.fs, &rd, got, 1)), 0);
    CHECK(memcmp(got, d, sizeof(d)) == 0);
    CHECK_EQ(CALL(&r, alfs_file_close_read(&r.fs, &rd)), 0);
    DONE(&r);
    rig_close(&r);
}

static void safe_foreign_area(void) {
    for (int blank = 0; blank < 2; blank++) {
        rig_t r;
        rig_open(&r, 16);
        if (blank)
            memset(r.sim.mem + SECTOR, 0xFF, r.sim.chip_size - 2 * SECTOR);
        CHECK_EQ(remount(&r), 0);
        CHECK_EQ(r.sim.ops, 0);
        alfs_file_info_t in;
        CHECK_EQ(CALL(&r, alfs_file_list(&r.fs, 0, &in)), ALFS_ENOENT);
        alfs_fs_status_t st = fstatus(&r);
        CHECK_EQ(st.live, 0);
        CHECK_EQ(st.dead == 0, blank); /* random bytes are dead pages */
        CHECK_EQ(r.sim.ops, 0);
        DONE(&r);
        rig_close(&r);
    }
    rig_t r;
    rig_open(&r, 16);
    r.cfg.size = 18 * SECTOR; /* past the chip */
    CHECK_EQ(remount(&r), ALFS_EGEOMETRY);
    CHECK_EQ(r.sim.ops, 0);
    DONE(&r);
    rig_close(&r);
}

/* A page header with a valid check, a random frame ID or one given, and a
 * link chosen to mislead. */
static void plant_header(rig_t *r, uint32_t page, uint32_t id, uint32_t count) {
    uint8_t *p = pg(r, page);
    put32le(p, id);
    p[4] = (uint8_t)count;
    p[5] = (uint8_t)(count >> 8);
    put32le(p + 6, crc32_ref(p, 6));
    uint32_t pages = r->cfg.size / PAGE;
    static const uint32_t kinds = 5;
    switch (sim_rand(&r->sim) % kinds) {
    case 0:
        put32le(p + 10, page); /* itself */
        break;
    case 1:
        put32le(p + 10, 0); /* the end */
        break;
    case 2:
        put32le(p + 10, pages + sim_rand(&r->sim) % 1000); /* past the area */
        break;
    case 3:
        put32le(p + 10, 1); /* back to the start */
        break;
    default:
        put32le(p + 10, 1 + sim_rand(&r->sim) % (pages - 1));
        break;
    }
}

static void safe_fuzz(void) {
    for (int trial = 0; trial < 300; trial++) {
        rig_t r;
        rig_open(&r, 12);
        CHECK_EQ(setup(&r), 0);
        static uint8_t d[12000], got[16384];
        fill(d, sizeof(d), (uint32_t)trial);
        for (int i = 0; i < 4; i++) {
            char name[8];
            snprintf(name, sizeof(name), "f%d", i);
            write_file(&r, name, d, 2000u * (uint32_t)(i + 1), 2000u * (uint32_t)(i + 1));
        }
        uint32_t lo = SECTOR, hi = r.sim.chip_size - SECTOR, pages = r.cfg.size / PAGE;
        uint32_t id = get32le(pg(&r, 1));
        switch (trial % 4) {
        case 0: /* bits flipped */
            for (int i = 0; i < 40; i++)
                sim_flip(&r.sim, lo + sim_rand(&r.sim) % (hi - lo), (uint8_t)(1u << (sim_rand(&r.sim) % 8)));
            break;
        case 1: /* headers with valid checks and misleading links */
            for (int i = 0; i < 20; i++)
                plant_header(&r, 1 + sim_rand(&r.sim) % (pages - 1), sim_rand(&r.sim) % 4 ? id : sim_rand(&r.sim),
                             sim_rand(&r.sim) % 8);
            break;
        case 2: /* pages copied from one place to another */
            for (int i = 0; i < 10; i++) {
                uint32_t a = 1 + sim_rand(&r.sim) % (pages - 1);
                uint32_t b = 1 + sim_rand(&r.sim) % (pages - 1);
                memcpy(pg(&r, b), pg(&r, a), PAGE);
            }
            break;
        default: /* every page random */
            sim_fill_random(&r.sim, r.cfg.start, r.cfg.size);
            break;
        }
        r.sim.violations = 0; /* the planted bytes are not alfs's programs */
        memset(&r.fs, 0, sizeof(r.fs));
        if (alfs_fs_mount(&r.fs, &r.cfg, &r.drv, r.scratch, r.cache, CACHE) == 0) {
            alfs_file_info_t in;
            uint32_t after = 0;
            for (int i = 0; i < 64 && alfs_file_list(&r.fs, after, &in) == 0; i++) {
                after = in.id;
                alfs_reader_t rd;
                if (alfs_file_open_read(&r.fs, &rd, in.name, r.rblk, PAGE) == 0) {
                    for (int j = 0; j < 400 && alfs_file_read(&r.fs, &rd, got, 300) > 0; j++)
                        ;
                    alfs_file_close_read(&r.fs, &rd);
                }
            }
            alfs_file_t f;
            if (alfs_file_open(&r.fs, &f, "f1", ALFS_GROW, r.wblk, SECTOR) == 0) {
                alfs_file_write(&r.fs, &f, d, 3000);
                alfs_file_close(&r.fs, &f);
            }
            uint32_t got_id;
            if (alfs_file_alloc(&r.fs, "new", 2000, &got_id) == 0 &&
                alfs_file_open(&r.fs, &f, "new", ALFS_GROW, r.wblk, PAGE) == 0) {
                alfs_file_write(&r.fs, &f, d, 2000);
                alfs_file_close(&r.fs, &f);
            }
            alfs_file_delete(&r.fs, "f2");
            alfs_fs_status_t st;
            alfs_fs_status(&r.fs, &st);
            for (int i = 0; i < 40 && alfs_fs_cleanup(&r.fs, 1) == ALFS_OK; i++)
                ;
        }
        CHECK(gaps_kept(&r));
        rig_close(&r);
    }
}

static void safe_alloc_atomic(void) {
    for (int pl = 0; pl < PL_COUNT; pl++)
        for (int64_t k = 0;; k++) {
            rig_t r;
            rig_open(&r, 8);
            CHECK_EQ(setup(&r), 0);
            static uint8_t d[3000], got[4000];
            fill(d, sizeof(d), 25);
            CHECK_EQ(write_file(&r, "old", d, 1000, 1000), 0);
            CHECK_EQ(fdelete(&r, "old"), 0); /* dead pages: the allocation may erase */
            uint32_t want = pages_for(sizeof(d)), id = 0;
            sim_power_after(&r.sim, k, (sim_pl_t)pl);
            falloc(&r, "a", sizeof(d), &id);
            bool past = !r.sim.off;
            sim_reboot(&r.sim);
            if (past) {
                rig_close(&r);
                break;
            }
            ids_t seen = {{0}, 0};
            for (int m = 0; m < 8; m++) {
                CHECK_EQ(remount(&r), 0);
                alfs_file_info_t in;
                if (finfo(&r, "a", &in) == 0) {
                    uint32_t pages[32];
                    CHECK_EQ(pages_of(&r, "a", pages, 32), want);
                    note(&seen, in.id);
                }
                CHECK(lists_only(&r, "a", "a"));
            }
            CHECK(seen.count <= 1);
            alfs_file_info_t in;
            if (finfo(&r, "a", &in) != 0)
                CHECK_EQ(falloc(&r, "a", sizeof(d), NULL), 0);
            alfs_file_t f;
            CHECK_EQ(fopenw(&r, &f, "a", ALFS_GROW), 0); /* a reopen after a mount skips a page */
            CHECK_EQ(put(&r, &f, d, sizeof(d), NULL), 0);
            CHECK_EQ(fclosew(&r, &f), 0);
            CHECK_EQ(slurp(&r, "a", got, sizeof(got)), (int32_t)sizeof(d));
            CHECK(memcmp(got, d, sizeof(d)) == 0);
            DONE(&r);
            rig_close(&r);
            if (tf_failed) {
                printf("    behaviour %d, power lost at operation %lld\n", pl, (long long)k);
                return;
            }
        }
}

static void safe_close_atomic(void) {
    for (int pl = 0; pl < PL_COUNT; pl++)
        for (int64_t k = 0;; k++) {
            rig_t r;
            rig_open(&r, 10);
            CHECK_EQ(setup(&r), 0);
            static uint8_t d[5000], got[9000];
            fill(d, sizeof(d), 26);
            CHECK_EQ(falloc(&r, "c", 9000, NULL), 0); /* the close erases the unused part */
            alfs_file_t f;
            CHECK_EQ(fopenw(&r, &f, "c", 0), 0);
            CHECK_EQ(put(&r, &f, d, sizeof(d), NULL), 0);
            CHECK_EQ(fsyncw(&r, &f), 0);
            sim_power_after(&r.sim, k, (sim_pl_t)pl);
            fclosew(&r, &f);
            bool past = !r.sim.off;
            sim_reboot(&r.sim);
            if (past) {
                rig_close(&r);
                break;
            }
            for (int m = 0; m < 8; m++) {
                CHECK_EQ(remount(&r), 0);
                alfs_file_info_t in;
                CHECK_EQ(finfo(&r, "c", &in), 0);
                int32_t n = slurp(&r, "c", got, sizeof(got));
                if (in.closed)
                    CHECK(n == (int32_t)sizeof(d) && !memcmp(got, d, sizeof(d)));
                else /* its pages: the partial one the close wrote, or not yet */
                    CHECK((n == (int32_t)whole_pages(sizeof(d)) || n == (int32_t)sizeof(d)) &&
                          !memcmp(got, d, (size_t)n));
                CHECK(lists_only(&r, "c", "c"));
            }
            DONE(&r);
            rig_close(&r);
            if (tf_failed) {
                printf("    behaviour %d, power lost at operation %lld\n", pl, (long long)k);
                return;
            }
        }
}

static void safe_delete_atomic(void) {
    for (int pl = 0; pl < PL_COUNT; pl++)
        for (int64_t k = 0;; k++) {
            rig_t r;
            rig_open(&r, 8);
            CHECK_EQ(setup(&r), 0);
            static uint8_t d[2000], got[3000];
            fill(d, sizeof(d), 27);
            CHECK_EQ(write_file(&r, "x", d, sizeof(d), sizeof(d)), 0);
            CHECK_EQ(write_file(&r, "y", d, 500, 500), 0);
            sim_power_after(&r.sim, k, (sim_pl_t)pl);
            int rc = fdelete(&r, "x");
            bool past = !r.sim.off;
            sim_reboot(&r.sim);
            for (int m = 0; m < 8; m++) {
                CHECK_EQ(remount(&r), 0);
                int32_t n = slurp(&r, "x", got, sizeof(got));
                if (past) {
                    CHECK_EQ(rc, 0);
                    CHECK_EQ(n, ALFS_ENOENT); /* once returned, never back */
                } else {
                    CHECK(n == ALFS_ENOENT || (n == (int32_t)sizeof(d) && !memcmp(got, d, sizeof(d))));
                }
                CHECK(reads_as(&r, "y", d, 500));
                CHECK(lists_only(&r, "x", "y"));
            }
            DONE(&r);
            rig_close(&r);
            if (past)
                break;
            if (tf_failed) {
                printf("    behaviour %d, power lost at operation %lld\n", pl, (long long)k);
                return;
            }
        }
}

static void safe_file_check(void) {
    for (int kind = 0; kind < 2; kind++) {
        rig_t r;
        rig_open(&r, 8);
        CHECK_EQ(setup(&r), 0);
        static uint8_t d[3000], got[3000];
        fill(d, sizeof(d), 28);
        CHECK_EQ(write_file(&r, "f", d, sizeof(d), sizeof(d)), 0);
        uint32_t pages[16];
        CHECK(pages_of(&r, "f", pages, 16) >= 6);
        uint32_t want;
        if (kind == 0) {
            pg(&r, pages[2])[HDR + 2 + 5] ^= 0x10; /* page 2's data damaged */
            want = sizeof(d) - D;
        } else {
            put32le(pg(&r, pages[5]) + 10, 1); /* the list cut short at page 5 */
            want = D0 + 5 * D;
        }
        uint32_t n = 0;
        CHECK_EQ(slurp_n(&r, "f", got, sizeof(got), &n), ALFS_ECHECK);
        CHECK_EQ(n, want);
        CHECK(memcmp(got, d, kind ? want : D0 + D) == 0);
        DONE(&r);
        rig_close(&r);
    }
}

static void safe_links_checked(void) {
    for (int kind = 0; kind < 4; kind++) {
        rig_t r;
        rig_open(&r, 10);
        CHECK_EQ(setup(&r), 0);
        static uint8_t a[4000], b[4000], got[9000];
        fill(a, sizeof(a), 29);
        fill(b, sizeof(b), 30);
        CHECK_EQ(write_file(&r, "b", b, sizeof(b), sizeof(b)), 0);
        CHECK_EQ(falloc(&r, "a", sizeof(a), NULL), 0);
        alfs_file_t f;
        CHECK_EQ(fopenw(&r, &f, "a", 0), 0);
        CHECK_EQ(put(&r, &f, a, sizeof(a), NULL), 0);
        CHECK_EQ(fsyncw(&r, &f), 0); /* not closed: the list's end is the file's */
        uint32_t pa[32], pb[32];
        CHECK(pages_of(&r, "a", pa, 32) > 4);
        CHECK(pages_of(&r, "b", pb, 32) > 4);
        uint32_t bent = kind == 0 ? pb[3] : kind == 1 ? r.cfg.size / PAGE - 1 : kind == 2 ? r.cfg.size / PAGE + 7u : pa[0];
        put32le(pg(&r, pa[2]) + 10, bent); /* page 2's link bent: to b, a free page, past the area, a's start */
        CHECK_EQ(remount(&r), 0);
        uint32_t fails = fstatus(&r).n.check_failures;
        int32_t n = slurp(&r, "a", got, sizeof(got));
        CHECK_EQ(n, (int32_t)(D0 + 2 * D)); /* the pages up to the bent link */
        CHECK(memcmp(got, a, (size_t)n) == 0);
        CHECK(fstatus(&r).n.check_failures > fails);
        CHECK(reads_as(&r, "b", b, sizeof(b)));
        DONE(&r);
        rig_close(&r);
    }
}

static void safe_update_atomic(void) {
    for (int pl = 0; pl < PL_COUNT; pl++)
        for (int64_t k = 0;; k++) {
            rig_t r;
            rig_open(&r, 8);
            CHECK_EQ(setup(&r), 0);
            static uint8_t a[700], b[900], got[1000];
            fill(a, sizeof(a), 42);
            fill(b, sizeof(b), 43);
            CHECK_EQ(write_file(&r, "lua/main.lua", a, sizeof(a), sizeof(a)), 0);
            sim_power_after(&r.sim, k, (sim_pl_t)pl);
            int rc = write_file(&r, "lua/main.lua", b, sizeof(b), sizeof(b));
            bool past = !r.sim.off;
            sim_reboot(&r.sim);
            for (int m = 0; m < 8; m++) {
                CHECK_EQ(remount(&r), 0);
                int32_t n = slurp(&r, "lua/main.lua", got, sizeof(got));
                bool old = n == (int32_t)sizeof(a) && !memcmp(got, a, sizeof(a));
                bool now = n == (int32_t)sizeof(b) && !memcmp(got, b, sizeof(b));
                if (past)
                    CHECK(rc == 0 && now); /* once the close returns, the new copy */
                else if (!old && !now)
                    printf("    pl %d k %lld, remount %d: read %d\n", pl, (long long)k, m, n);
                CHECK(old || now);
                CHECK(lists_only(&r, "lua/main.lua", "lua/main.lua"));
            }
            DONE(&r);
            rig_close(&r);
            if (past)
                break;
            if (tf_failed) {
                printf("    behaviour %d, power lost at operation %lld\n", pl, (long long)k);
                return;
            }
        }
}

/* ---- OBS and SIMP --------------------------------------------------------- */

static void obs_status(void) {
    rig_t r;
    rig_open(&r, 10);
    CHECK_EQ(setup(&r), 0);
    alfs_fs_status_t st = fstatus(&r);
    CHECK_EQ(st.pages, 10 * PPS - 1);
    CHECK_EQ(st.free, 10 * PPS - 1);
    CHECK_EQ(st.data_per_page, D);
    uint64_t p0 = r.sim.programs, e0 = r.sim.erases;
    uint32_t n0 = st.n.programs, ne0 = st.n.erases;
    static uint8_t d[5000];
    fill(d, sizeof(d), 35);
    CHECK_EQ(write_file(&r, "a", d, 5000, 5000), 0);
    CHECK_EQ(write_file(&r, "b", d, 1000, 3000), 0);
    st = fstatus(&r);
    CHECK_EQ(st.live, pages_for(5000) + pages_for(1000));
    CHECK_EQ(st.records, 2);
    CHECK_EQ(st.n.programs - n0, r.sim.programs - p0);
    CHECK_EQ(st.n.erases - ne0, r.sim.erases - e0);
    CHECK_EQ(st.n.prog_failures, 0);
    CHECK_EQ(st.n.bytes_lost, 0);
    CHECK_EQ(fdelete(&r, "a"), 0);
    st = fstatus(&r);
    CHECK_EQ(st.live, pages_for(1000));
    CHECK(st.dead >= pages_for(5000));
    CHECK_EQ(st.live + st.free + st.dead, st.pages);
    CHECK_EQ(remount(&r), 0);
    alfs_fs_status_t again = fstatus(&r);
    CHECK_EQ(again.live, st.live);
    CHECK_EQ(again.dead, st.dead);
    DONE(&r);
    rig_close(&r);
}

static void obs_clean(void) {
    rig_t r;
    rig_open(&r, 8);
    CHECK_EQ(remount(&r), 0);
    CHECK(!fstatus(&r).clean);
    CHECK(clean_all(&r) >= 0);
    CHECK(fstatus(&r).clean);
    uint8_t d[1000];
    fill(d, sizeof(d), 36);
    CHECK_EQ(write_file(&r, "a", d, sizeof(d), sizeof(d)), 0);
    CHECK(fstatus(&r).clean); /* the reservation filled */
    CHECK_EQ(fdelete(&r, "a"), 0);
    CHECK(!fstatus(&r).clean);
    CHECK(clean_all(&r) >= 0);
    CHECK(fstatus(&r).clean);
    CHECK_EQ(remount(&r), 0);
    CHECK(fstatus(&r).clean);
    DONE(&r);
    rig_close(&r);
}

static void simp_ram(void) {
    /* The budgets are for a 32-bit target; a host's pointers are wider. */
    size_t wide = sizeof(void *) - 4;
    CHECK(sizeof(alfs_fs_t) <= 96 + 3 * wide);
    CHECK(sizeof(alfs_file_t) <= 48 + wide);
    CHECK(sizeof(alfs_reader_t) <= 40 + wide);
}

int main(int argc, char **argv) {
    static const tf_test_t tests[] = {
        {"sim_torn_obeys_a9", sim_torn_obeys_a9},
        {"if_geometry", if_geometry},
        {"if_mount", if_mount},
        {"if_endian", if_endian},
        {"if_names", if_names},
        {"if_alloc", if_alloc},
        {"if_write_pages", if_write_pages},
        {"if_close", if_close},
        {"if_grow", if_grow},
        {"if_reopen_unclosed", if_reopen_unclosed},
        {"if_flush", if_flush},
        {"if_read", if_read},
        {"if_delete", if_delete},
        {"if_update", if_update},
        {"if_list", if_list},
        {"if_ids_never_reused", if_ids_never_reused},
        {"if_cleanup", if_cleanup},
        {"if_refusals", if_refusals},
        {"spd_no_erase_outside_erase", spd_no_erase_outside_erase},
        {"spd_write_programs_bounded", spd_write_programs_bounded},
        {"spd_mount_reads_bounded", spd_mount_reads_bounded},
        {"spd_wlog_longest_stall", spd_wlog_longest_stall},
        {"spd_cpu_linear", spd_cpu_linear},
        {"spd_open_reads_bounded", spd_open_reads_bounded},
        {"life_erase_only_unused", life_erase_only_unused},
        {"life_write_amplification", life_write_amplification},
        {"life_wlog_erases", life_wlog_erases},
        {"life_wear_level", life_wear_level},
        {"safe_powerloss_durable", safe_powerloss_durable},
        {"safe_powerloss_bound", safe_powerloss_bound},
        {"safe_reopen_after_loss", safe_reopen_after_loss},
        {"safe_no_false_data", safe_no_false_data},
        {"safe_write_only_verified_erased", safe_write_only_verified_erased},
        {"safe_prog_failure", safe_prog_failure},
        {"safe_bad_sector", safe_bad_sector},
        {"safe_read_retry", safe_read_retry},
        {"safe_foreign_area", safe_foreign_area},
        {"safe_fuzz", safe_fuzz},
        {"safe_alloc_atomic", safe_alloc_atomic},
        {"safe_close_atomic", safe_close_atomic},
        {"safe_delete_atomic", safe_delete_atomic},
        {"safe_file_check", safe_file_check},
        {"safe_links_checked", safe_links_checked},
        {"safe_update_atomic", safe_update_atomic},
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
