/*
 * alfs's tests, one or more for each requirement of docs/REQUIREMENTS.md.
 * Each is named in the requirement's "Verified by".
 *
 * Every call a test makes goes through CALL, which holds it to SPD-01 (no
 * erase) and records the longest single operation (SPD-06), or ECALL, for
 * the calls SPD-01 lets erase. Every test ends with DONE, which fails it if
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
#define HDR 14u              /* a page header (docs/SPEC.md 2) */
#define FBLOCK 50u           /* a frame block (SPEC 2.1) */
#define NAME_AT (HDR + 4u)   /* the filename, on a first page */
#define CLOSE_AT (HDR + 36u) /* the close fields, on a first page */
#define D (PAGE - HDR - 4u)  /* data a page holds */
#define D0 (D - FBLOCK)      /* data a file's first page holds */
#define CAT_PAGES (2u * PPS) /* the catalog's: no frame page has their addresses */
#define ENTRY 44u            /* a catalog entry (SPEC 1.2) */
#define ENTRIES 80u          /* a catalog sector's slots */
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

/* A page's data check (SPEC 2): the frame ID, the page count, the data. */
static uint32_t page_check(uint32_t id, uint32_t count, const uint8_t *data, uint32_t n) {
    uint8_t h[6] = {(uint8_t)id, (uint8_t)(id >> 8), (uint8_t)(id >> 16), (uint8_t)(id >> 24), (uint8_t)count,
                    (uint8_t)(count >> 8)};
    return ~crc32_more(crc32_more(0xFFFFFFFFu, h, 6), data, n);
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
    return (int)CALL(r, alfs_fs_mount(&r->fs, &r->cfg, &r->drv, r->scratch));
}

/* A reboot: every handle gone, the area mounted again. */
static int remount(rig_t *r) {
    memset(&r->fs, 0, sizeof(r->fs));
    return fmount(r);
}

static int ffmt(rig_t *r) {
    return (int)ECALL(r, alfs_fs_format(&r->fs, &r->cfg, &r->drv, r->scratch));
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

/* The area formatted and every frame page made free. */
static int setup(rig_t *r) {
    return ffmt(r) || clean_all(r) < 0 ? -1 : 0;
}

static int falloc(rig_t *r, const char *name, uint32_t bytes, uint32_t *id) {
    uint32_t got = 0;
    int rc = (int)ECALL(r, alfs_file_alloc(&r->fs, name, bytes, &got));
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

static int fclosew(rig_t *r, alfs_file_t *f) {
    return (int)ECALL(r, alfs_file_close(&r->fs, f));
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

/* Allocates `reserve` bytes, writes d, closes. 0, or the first error. */
static int write_file(rig_t *r, const char *name, const uint8_t *d, uint32_t n, uint32_t reserve) {
    alfs_file_t f;
    int rc = falloc(r, name, reserve, NULL);
    if (!rc)
        rc = fopenw(r, &f, name, 0);
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

/* The catalog sector in use (SPEC 1.1), or -1. */
static int cat_sector(rig_t *r) {
    int best = -1;
    uint32_t seq = 0;
    for (int c = 0; c < 2; c++) {
        const uint8_t *s = r->sim.mem + r->cfg.start + (uint32_t)c * SECTOR;
        if (memcmp(s, "alfc", 4) || get32le(s + 16) != crc32_ref(s, 16) || s[20] != 0)
            continue;
        if (best < 0 || get32le(s + 12) > seq) {
            best = c;
            seq = get32le(s + 12);
        }
    }
    return best;
}

/* Slot i of catalog sector c (SPEC 1.2). */
static uint8_t *slot(rig_t *r, int c, uint32_t i) {
    uint32_t page = i / 5, k = i % 5;
    return r->sim.mem + r->cfg.start + (uint32_t)c * SECTOR + page * PAGE + (page ? 0u : 24u) + k * ENTRY;
}

/* File name's catalog entry, or NULL. */
static uint8_t *entry_of(rig_t *r, const char *name) {
    int c = cat_sector(r);
    for (uint32_t i = 0; c >= 0 && i < ENTRIES; i++) {
        uint8_t *e = slot(r, c, i);
        if (get32le(e + 40) == crc32_ref(e, 40) && !strncmp((const char *)e + 8, name, 32))
            return e;
    }
    return NULL;
}

/* A list by its links (SPEC 3), from page first: its pages into out. */
static uint32_t list_of(rig_t *r, uint32_t first, uint32_t *out, uint32_t max) {
    uint32_t n = 0, p = first, id = get32le(pg(r, first));
    while (n < max && p >= CAT_PAGES && p < r->cfg.size / PAGE) {
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

/* File name's pages, by the catalog and the links. */
static uint32_t pages_of(rig_t *r, const char *name, uint32_t *out, uint32_t max) {
    const uint8_t *e = entry_of(r, name);
    return e ? list_of(r, get32le(e + 4), out, max) : 0u;
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
        {"area under four sectors", &r.cfg.size, 3 * SECTOR},
        {"empty area", &r.cfg.size, 0},
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        r.cfg = good;
        *bad[i].field = bad[i].value;
        int rc = ffmt(&r);
        if (rc != ALFS_EGEOMETRY && rc != ALFS_EINVAL)
            printf("    format accepted: %s (%d)\n", bad[i].what, rc);
        CHECK(rc == ALFS_EGEOMETRY || rc == ALFS_EINVAL);
        rc = remount(&r);
        CHECK(rc == ALFS_EGEOMETRY || rc == ALFS_EINVAL);
        CHECK_EQ(r.sim.ops, 0);
    }
    r.cfg = good;
    CHECK_EQ(setup(&r), 0);
    r.cfg.size = 6 * SECTOR; /* the stamp records 8 sectors */
    CHECK_EQ(remount(&r), ALFS_ENOFS);
    r.cfg = good;
    CHECK_EQ(remount(&r), 0);
    DONE(&r);
    rig_close(&r);
}

static void if_format_mount(void) {
    rig_t r;
    rig_open(&r, 8);
    CHECK_EQ(ffmt(&r), 0);
    CHECK_EQ(cat_sector(&r), 0);
    alfs_file_info_t in;
    CHECK_EQ(CALL(&r, alfs_file_list(&r.fs, 0, &in)), ALFS_ENOENT);
    alfs_fs_status_t st = fstatus(&r);
    CHECK_EQ(st.pages, 6 * PPS);
    CHECK_EQ(st.live, 0);
    CHECK_EQ(st.entries, 0);
    CHECK_EQ(st.entries_max, ENTRIES);
    CHECK(!st.clean); /* the frame sectors hold what they held */
    CHECK(clean_all(&r) >= 0);
    st = fstatus(&r);
    CHECK_EQ(st.free, 6 * PPS);
    CHECK_EQ(st.dead, 0);
    CHECK(st.clean);
    CHECK_EQ(remount(&r), 0);
    CHECK_EQ(r.sim.c_ops, 0);
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

    const uint8_t *s = r.sim.mem + r.cfg.start;
    CHECK(memcmp(s, "alfc", 4) == 0);
    CHECK_EQ(s[4], 6);  /* version           */
    CHECK_EQ(s[5], 12); /* log2(sector size) */
    CHECK_EQ(s[6], 8);  /* log2(page size)   */
    CHECK_EQ(s[7], 0);
    CHECK_EQ(get16le(s + 8), 8);
    CHECK_EQ(get16le(s + 10), 0);
    CHECK_EQ(get32le(s + 12), 1); /* sequence */
    CHECK_EQ(get32le(s + 16), crc32_ref(s, 16));
    CHECK_EQ(s[20], 0); /* complete */

    const uint8_t *e = entry_of(&r, "ab");
    CHECK(e != NULL);
    CHECK_EQ(get32le(e), id);
    uint32_t first = get32le(e + 4);
    CHECK(first >= CAT_PAGES);
    CHECK(memcmp(e + 8, "ab", 3) == 0);
    for (int i = 11; i < 40; i++)
        CHECK_EQ(e[i], 0);
    CHECK_EQ(get32le(e + 40), crc32_ref(e, 40));

    uint32_t pages[4];
    CHECK_EQ(list_of(&r, first, pages, 4), 2); /* 300 bytes: 188 and 112 */
    const uint8_t *p0 = pg(&r, pages[0]), *p1 = pg(&r, pages[1]);
    CHECK_EQ(get32le(p0), id);
    CHECK_EQ(get16le(p0 + 4), 0);
    CHECK_EQ(get32le(p0 + 6), crc32_ref(p0, 6));
    CHECK_EQ(get32le(p0 + 10), pages[1]);
    CHECK_EQ(get32le(p0 + HDR), id);
    CHECK(memcmp(p0 + NAME_AT, "ab", 3) == 0);
    const uint8_t *c = p0 + CLOSE_AT;
    CHECK_EQ(get16le(c), 2);                     /* page count */
    CHECK_EQ(get32le(c + 2), 300);               /* size       */
    CHECK_EQ(get32le(c + 6), crc32_ref(d, 300)); /* checksum   */
    CHECK_EQ(get32le(c + 10), crc32_ref(p0 + HDR, 46));
    CHECK(memcmp(p0 + HDR + FBLOCK, d, D0) == 0);
    CHECK_EQ(get32le(p0 + HDR + FBLOCK + D0), page_check(id, 0, d, D0));
    CHECK_EQ(get16le(p1 + 4), 1);
    CHECK_EQ(get32le(p1 + 10), 0); /* the last page */
    CHECK(memcmp(p1 + HDR, d + D0, 300 - D0) == 0);
    CHECK_EQ(get32le(p1 + HDR + 300 - D0), page_check(id, 1, d + D0, 300 - D0));
    CHECK(erased(p1 + HDR + 300 - D0 + 4, PAGE - HDR - (300 - D0) - 4));
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
    CHECK_EQ(falloc(&r, "a", 10, &id), ALFS_EEXIST); /* one file a filename */
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
    CHECK_EQ(r.sim.c_programs, want + 1); /* the entry and each page */
    CHECK_EQ(r.sim.c_erases, 0);          /* every page was free */
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
    CHECK(erased(pg(&r, pages[0]) + CLOSE_AT, 14));            /* not closed */
    alfs_file_info_t in;
    CHECK_EQ(finfo(&r, "log", &in), 0);
    CHECK_EQ(in.id, id);
    CHECK(!in.closed);
    CHECK_EQ(in.size, ALFS_SIZE_OPEN);

    /* An area of dead pages: an allocation erases what it needs, and only
     * sectors holding no live page. */
    static uint8_t d[20000];
    fill(d, sizeof(d), 2);
    CHECK_EQ(write_file(&r, "keep", d, 3000, 3000), 0);
    for (int i = 0; i < 6; i++) {
        char name[8];
        snprintf(name, sizeof(name), "f%d", i);
        CHECK_EQ(write_file(&r, name, d, 2000, 2000), 0);
        CHECK_EQ(fdelete(&r, name), 0);
    }
    CHECK_EQ(fdelete(&r, "log"), 0);
    alfs_fs_status_t st = fstatus(&r);
    uint32_t need = pages_for(12000);
    if (st.free < need) {
        CHECK_EQ(falloc(&r, "big", 12000, NULL), 0);
        CHECK(r.sim.c_erases > 0);
        CHECK(r.sim.c_erases <= (need - st.free + PPS - 1) / PPS + 2); /* sectors may hold free pages too */
        CHECK(reads_as(&r, "keep", d, 3000));
    }
    DONE(&r);
    rig_close(&r);
}

/* Each program a write or sync makes is one page's data and its check, at
 * the page's data. */
static bool whole_page_programs(const rig_t *r) {
    for (int i = 0; i < r->nprog && i < MAX_PROGS; i++) {
        uint32_t at = (r->paddr[i] - r->cfg.start) % PAGE, len = r->plen[i];
        if (!((at == HDR && len == D + 4) || (at == HDR + FBLOCK && len == D0 + 4)))
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
    CHECK_EQ(fclosew(&r, &f), 0);
    const uint8_t *c = pg(&r, pages[0]) + CLOSE_AT;
    CHECK_EQ(get16le(c), used);
    CHECK_EQ(get32le(c + 2), n);
    CHECK_EQ(get32le(c + 10), crc32_ref(pg(&r, pages[0]) + HDR, 46));
    CHECK_EQ(get32le(pg(&r, pages[used - 1]) + 10), 0); /* the list's end */
    for (uint32_t i = used; i < 40; i++) {
        uint32_t k = pages[i] / PPS; /* its sector */
        bool live = false;
        for (uint32_t j = 0; j < used; j++)
            live |= pages[j] / PPS == k;
        if (live)
            CHECK_EQ(get32le(pg(&r, pages[i])), get32le(pg(&r, pages[0]))); /* dead, kept */
        else
            CHECK(erased(pg(&r, k * PPS), SECTOR)); /* erased by the close */
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
    alfs_file_t f;
    CHECK_EQ(fopenw(&r, &f, "u", 0), 0);
    CHECK_EQ(put(&r, &f, a, n, NULL), 0);
    CHECK_EQ(fsyncw(&r, &f), 0);
    CHECK_EQ(remount(&r), 0); /* never closed: the block's 100 bytes are lost */
    CHECK_EQ(fopenw(&r, &f, "u", 0), 0);
    CHECK_EQ(put(&r, &f, b, sizeof(b), NULL), 0);
    CHECK_EQ(fclosew(&r, &f), 0);
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

static void if_reopen_closed(void) {
    rig_t r;
    rig_open(&r, 8);
    CHECK_EQ(setup(&r), 0);
    static uint8_t a[1600], b[1000], got[4000];
    fill(a, sizeof(a), 8);
    fill(b, sizeof(b), 9);
    CHECK_EQ(write_file(&r, "a", a, 1000, 1000), 0);
    CHECK_EQ(write_file(&r, "b", b, 1000, 1000), 0); /* beside a, in its last sector */
    alfs_file_t f;
    CHECK_EQ(fopenw(&r, &f, "a", 0), ALFS_EINVAL); /* a page's block cannot hold a sector */
    CHECK_EQ(r.sim.c_ops, 0);
    r.bsize = SECTOR;
    CHECK_EQ(ECALL(&r, alfs_file_open(&r.fs, &f, "a", ALFS_GROW, r.wblk, r.bsize)), 0);
    CHECK_EQ(put(&r, &f, a + 1000, 600, NULL), 0);
    CHECK_EQ(fclosew(&r, &f), 0);
    for (int pass = 0; pass < 2; pass++) {
        CHECK_EQ(slurp(&r, "a", got, sizeof(got)), 1600);
        CHECK(memcmp(got, a, 1600) == 0);
        CHECK(reads_as(&r, "b", b, 1000));
        CHECK_EQ(remount(&r), 0);
    }
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
    uint8_t *e = entry_of(&r, "a");
    CHECK(e != NULL);
    uint32_t first = get32le(e + 4);
    CHECK_EQ(fdelete(&r, "a"), 0);
    CHECK_EQ(r.sim.c_erases, 0);
    for (int i = 0; i < 32; i++) {
        CHECK_EQ(e[8 + i], 0);                     /* in the entry */
        CHECK_EQ(pg(&r, first)[NAME_AT + i], 0);   /* and the frame block */
    }
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
    /* A Lua update: the filename blocked out, a new file written. */
    CHECK_EQ(write_file(&r, "lua/main.lua", a, 300, 300), 0);
    CHECK_EQ(fdelete(&r, "lua/main.lua"), 0);
    CHECK_EQ(write_file(&r, "lua/main.lua", b, 400, 400), 0);
    CHECK_EQ(remount(&r), 0);
    CHECK(reads_as(&r, "lua/main.lua", b, 400));
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
    static uint8_t d[3000], got[3000];
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
 * written every 200 ms, synced every second, `ms` long, with a cleanup
 * step every 20 ms if asked. Rows go to exp[]; synced is the length the
 * last successful sync covered. Stops early when the power is lost. 0, or
 * the first error that is not a power loss.
 */
typedef struct {
    uint8_t *exp;
    uint32_t len, cap;
    uint32_t synced;
    uint32_t seq;
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
            int rc = fsyncw(r, f);
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

/* An update: the filename blocked out, if it names a file, and a new file
 * written. */
static int cfg_do(rig_t *r, cfg_op_t op) {
    static uint8_t d[4096];
    int rc = fdelete(r, op.name);
    if (rc && rc != ALFS_ENOENT)
        return rc;
    if (!op.size)
        return 0;
    fill(d, op.size, op.gen);
    return write_file(r, op.name, d, op.size, op.size);
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
    CHECK_EQ(remount(&r), 0);
    CHECK(r.sim.c_bytes_read <= 2u * SECTOR);
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
        CHECK(r.sim.c_bytes_read <= SECTOR + PAGE + npages[i] * HDR);
        CHECK_EQ(put(&r, &f, d, bytes, NULL), 0);
        CHECK_EQ(fsyncw(&r, &f), 0);
        alfs_reader_t rd;
        CHECK_EQ(CALL(&r, alfs_file_open_read(&r.fs, &rd, "o", r.rblk, PAGE)), 0);
        CHECK(r.sim.c_bytes_read <= SECTOR + PAGE);
        CHECK_EQ(CALL(&r, alfs_file_close_read(&r.fs, &rd)), 0);
        CHECK_EQ(remount(&r), 0);
        CHECK_EQ(fopenw(&r, &f, "o", 0), 0); /* a reopen: every page's data once more */
        CHECK(r.sim.c_bytes_read <= SECTOR + PAGE + npages[i] * (HDR + PAGE));
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
            if (rc == ALFS_ENOSPC)
                return 0; /* worn: the area no longer holds a file */
            if (rc || fopenw(r, &f, name, 0))
                return -6;
            in_file = len = 0;
        }
        uint32_t least = UINT32_MAX;
        for (uint32_t k = 2; k < sectors; k++) {
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
        CHECK_EQ(r.sim.bytes_programmed - b0, ENTRY + pages * HDR + (FBLOCK - 14));
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
        CHECK_EQ(r.sim.bytes_programmed - b0, full + 4u * npages);
        b0 = r.sim.bytes_programmed;
        CHECK_EQ(fclosew(&r, &f), 0);
        uint32_t rest = total - full;
        CHECK_EQ(r.sim.bytes_programmed - b0, (rest ? rest + 4u : 0u) + 14u + 4u);
        DONE(&r);
        rig_close(&r);
    }
}

static void life_wlog_erases(void) {
    rig_t r;
    uint32_t bytes = 50u * 600u * ROW, pages = pages_for(bytes); /* ten minutes at 50 rows a second */
    rig_open(&r, 2 + pages / PPS + 6);
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
    for (uint32_t k = 2; k < 34; k++) {
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

/* Power lost at operation k of a W-LOG run into a reserved file, with a
 * closed file beside it; then remounts and reads. Sets *past when k is past
 * the run's last operation. */
static void powerloss_case(sim_pl_t pl, int64_t k, int *past) {
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
        bool ok = n >= (int32_t)whole_pages(lg.synced) && n <= (int32_t)lg.len && !memcmp(got, lg.exp, (size_t)n);
        if (!ok)
            printf("    pl %d k %lld: read %d of %u (synced %u)\n", pl, (long long)k, n, lg.len, lg.synced);
        CHECK(ok); /* SAFE-02 */
    }
    free(lg.exp);
    DONE(&r);
    rig_close(&r);
}

static void powerloss_all(void) {
    for (int pl = 0; pl < PL_COUNT; pl++)
        for (int64_t k = 0;; k++) {
            int past = 0;
            powerloss_case((sim_pl_t)pl, k, &past);
            if (past && !tf_failed)
                break;
            if (tf_failed) {
                printf("    behaviour %d, power lost at operation %lld\n", pl, (long long)k);
                return;
            }
        }
}

static void safe_powerloss_durable(void) {
    powerloss_all();
}

static void safe_powerloss_bound(void) {
    powerloss_all();
}

/* SAFE-04: power lost at operation k of writing; reopened, written on,
 * closed. What reads back is what was written before the loss, as far as
 * some page, then what was written after, perhaps but its first page. */
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
            r.sim.torn_ok = true; /* a page that reads erased is written as erased (D-3) */
            CHECK_EQ(remount(&r), 0);
            CHECK_EQ(fopenw(&r, &f, "log", 0), 0);
            CHECK_EQ(put(&r, &f, b, sizeof(b), NULL), 0);
            CHECK_EQ(fclosew(&r, &f), 0);
            for (int m = 0; m < 3; m++) {
                CHECK_EQ(remount(&r), 0);
                uint32_t n = 0;
                int32_t rc = slurp_n(&r, "log", got, sizeof(got), &n);
                CHECK(rc >= 0 || rc == ALFS_ECHECK);
                /* a's bytes to some page's end, then b's, perhaps but its first page */
                bool ok = false;
                uint32_t pre = 0;
                for (uint32_t p = 0; !ok && p <= n && p <= written; p = p ? p + D : D0) {
                    uint32_t rest = n - p;
                    pre = p;
                    ok = p >= whole_pages(synced) && !memcmp(got, a, p) && rest <= sizeof(b) &&
                         sizeof(b) - rest <= D && !memcmp(got + p, b + sizeof(b) - rest, rest);
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
        uint32_t at = h + sim_rand(&r.sim) % (n + 4); /* its data or its check */
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
        CHECK_EQ(st.free, 5 * PPS);
        static uint8_t d[20000];
        fill(d, sizeof(d), 23);
        CHECK_EQ(write_file(&r, "a", d, 5 * PPS * D - D, 5 * PPS * D - D), 0);
        CHECK_EQ(r.sim.sector_progs[bad], 0);
        CHECK_EQ(remount(&r), 0);
        CHECK(reads_as(&r, "a", d, 5 * PPS * D - D));
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
        CHECK_EQ(fmount(&r), ALFS_ENOFS);
        CHECK_EQ(r.sim.ops, 0);
        DONE(&r);
        rig_close(&r);
    }
    rig_t r;
    rig_open(&r, 16);
    r.cfg.size = 18 * SECTOR; /* past the chip */
    CHECK_EQ(fmount(&r), ALFS_EGEOMETRY);
    CHECK_EQ(ffmt(&r), ALFS_EGEOMETRY);
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
        put32le(p + 10, sim_rand(&r->sim) % CAT_PAGES); /* the catalog */
        break;
    case 2:
        put32le(p + 10, pages + sim_rand(&r->sim) % 1000); /* past the area */
        break;
    case 3:
        put32le(p + 10, CAT_PAGES); /* back to the start */
        break;
    default:
        put32le(p + 10, CAT_PAGES + sim_rand(&r->sim) % (pages - CAT_PAGES));
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
        uint32_t id = get32le(pg(&r, CAT_PAGES));
        switch (trial % 4) {
        case 0: /* bits flipped */
            for (int i = 0; i < 40; i++)
                sim_flip(&r.sim, lo + sim_rand(&r.sim) % (hi - lo), (uint8_t)(1u << (sim_rand(&r.sim) % 8)));
            break;
        case 1: /* headers with valid checks and misleading links */
            for (int i = 0; i < 20; i++)
                plant_header(&r, CAT_PAGES + sim_rand(&r.sim) % (pages - CAT_PAGES), sim_rand(&r.sim) % 4 ? id : sim_rand(&r.sim),
                             sim_rand(&r.sim) % 8);
            break;
        case 2: /* pages copied from one place to another */
            for (int i = 0; i < 10; i++) {
                uint32_t a = CAT_PAGES + sim_rand(&r.sim) % (pages - CAT_PAGES);
                uint32_t b = CAT_PAGES + sim_rand(&r.sim) % (pages - CAT_PAGES);
                memcpy(pg(&r, b), pg(&r, a), PAGE);
            }
            break;
        default: /* every frame page random */
            sim_fill_random(&r.sim, r.cfg.start + CAT_PAGES * PAGE, r.cfg.size - CAT_PAGES * PAGE);
            break;
        }
        r.sim.violations = 0; /* the planted bytes are not alfs's programs */
        memset(&r.fs, 0, sizeof(r.fs));
        if (alfs_fs_mount(&r.fs, &r.cfg, &r.drv, r.scratch) == 0) {
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
            static uint8_t d[3000], got[3000];
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
            }
            CHECK(seen.count <= 1);
            alfs_file_info_t in;
            if (finfo(&r, "a", &in) != 0)
                CHECK_EQ(falloc(&r, "a", sizeof(d), NULL), 0);
            alfs_file_t f;
            CHECK_EQ(fopenw(&r, &f, "a", 0), 0);
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
                else
                    CHECK(n == (int32_t)whole_pages(sizeof(d)) && !memcmp(got, d, (size_t)n));
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

static void safe_frame_check(void) {
    rig_t r;
    rig_open(&r, 8);
    CHECK_EQ(setup(&r), 0);
    static uint8_t d[3000], got[3000];
    fill(d, sizeof(d), 28);
    CHECK_EQ(write_file(&r, "f", d, sizeof(d), sizeof(d)), 0);
    uint32_t pages[16];
    CHECK(pages_of(&r, "f", pages, 16) >= 3);
    uint32_t id = get32le(pg(&r, pages[0]));
    uint8_t *p = pg(&r, pages[2]);
    p[HDR + 5] ^= 0x10; /* the data changed, and its page's check made to match */
    put32le(p + HDR + D, page_check(id, 2, p + HDR, D));
    uint32_t n = 0;
    CHECK_EQ(slurp_n(&r, "f", got, sizeof(got), &n), ALFS_ECHECK);
    CHECK_EQ(n, sizeof(d));
    DONE(&r);
    rig_close(&r);
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
        uint32_t bent = kind == 0 ? pb[3] : kind == 1 ? 1u : kind == 2 ? r.cfg.size / PAGE + 7u : pa[0];
        put32le(pg(&r, pa[2]) + 10, bent); /* page 2's link bent: to b, the catalog, past the area, a's start */
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

static void safe_catalog_rewrite_atomic(void) {
    for (int pl = 0; pl < PL_COUNT; pl++)
        for (int64_t k = 0;; k++) {
            rig_t r;
            rig_open(&r, 12);
            CHECK_EQ(setup(&r), 0);
            static uint8_t d[600];
            fill(d, sizeof(d), 31);
            static const char *const keep[] = {"k0", "k1", "k2"};
            for (int i = 0; i < 3; i++)
                CHECK_EQ(write_file(&r, keep[i], d, 200u * (uint32_t)(i + 1), 600), 0);
            for (uint32_t i = 3; i < ENTRIES; i++) { /* every slot used */
                CHECK_EQ(falloc(&r, "t", 10, NULL), 0);
                CHECK_EQ(fdelete(&r, "t"), 0);
            }
            CHECK(clean_all(&r) >= 0);
            int before = cat_sector(&r);
            sim_power_after(&r.sim, k, (sim_pl_t)pl);
            int rc = falloc(&r, "last", 10, NULL); /* the rewrite */
            bool past = !r.sim.off;
            sim_reboot(&r.sim);
            if (past) {
                CHECK_EQ(rc, 0);
                CHECK(cat_sector(&r) != before);
                rig_close(&r);
                break;
            }
            for (int m = 0; m < 4; m++) {
                CHECK_EQ(remount(&r), 0);
                for (int i = 0; i < 3; i++)
                    CHECK(reads_as(&r, keep[i], d, 200u * (uint32_t)(i + 1)));
            }
            DONE(&r);
            rig_close(&r);
            if (tf_failed) {
                printf("    behaviour %d, power lost at operation %lld\n", pl, (long long)k);
                return;
            }
        }
}

static void safe_reopen_closed_cut(void) {
    for (int pl = 0; pl < PL_COUNT; pl++)
        for (int64_t k = 0;; k++) {
            rig_t r;
            rig_open(&r, 10);
            CHECK_EQ(setup(&r), 0);
            static uint8_t a[1600], b[1000], c[3000], got[4000];
            fill(a, sizeof(a), 32);
            fill(b, sizeof(b), 33);
            fill(c, sizeof(c), 34);
            CHECK_EQ(write_file(&r, "a", a, 1000, 1000), 0);
            CHECK_EQ(write_file(&r, "b", b, 1000, 1000), 0); /* beside a */
            CHECK_EQ(write_file(&r, "pad", c, 3000, 3000), 0);
            CHECK_EQ(write_file(&r, "c", c, 3000, 3000), 0); /* sectors of its own */
            r.bsize = SECTOR;
            alfs_file_t f;
            sim_power_after(&r.sim, k, (sim_pl_t)pl);
            if (ECALL(&r, alfs_file_open(&r.fs, &f, "a", ALFS_GROW, r.wblk, r.bsize)) == 0 && !r.sim.off) {
                put(&r, &f, a + 1000, 600, NULL);
                if (!r.sim.off)
                    fclosew(&r, &f);
            }
            bool past = !r.sim.off;
            sim_reboot(&r.sim);
            if (past) {
                rig_close(&r);
                break;
            }
            for (int m = 0; m < 3; m++) {
                CHECK_EQ(remount(&r), 0);
                CHECK(reads_as(&r, "c", c, sizeof(c))); /* no page in the rewritten sectors */
                uint32_t n = 0;
                slurp_n(&r, "a", got, sizeof(got), &n);
                CHECK(runs_cut(got, n, a, sizeof(a))); /* no byte it never held */
                slurp_n(&r, "b", got, sizeof(got), &n);
                CHECK(runs_cut(got, n, b, sizeof(b)));
            }
            DONE(&r);
            rig_close(&r);
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
    CHECK_EQ(st.pages, 8 * PPS);
    CHECK_EQ(st.free, 8 * PPS);
    CHECK_EQ(st.data_per_page, D);
    uint64_t p0 = r.sim.programs, e0 = r.sim.erases;
    uint32_t n0 = st.n.programs, ne0 = st.n.erases;
    static uint8_t d[5000];
    fill(d, sizeof(d), 35);
    CHECK_EQ(write_file(&r, "a", d, 5000, 5000), 0);
    CHECK_EQ(write_file(&r, "b", d, 1000, 3000), 0);
    st = fstatus(&r);
    CHECK_EQ(st.live, pages_for(5000) + pages_for(1000));
    CHECK_EQ(st.entries, 2);
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
    CHECK_EQ(ffmt(&r), 0);
    CHECK(!fstatus(&r).clean);
    CHECK(clean_all(&r) >= 0);
    CHECK(fstatus(&r).clean);
    uint8_t d[1000];
    fill(d, sizeof(d), 36);
    CHECK_EQ(write_file(&r, "a", d, sizeof(d), sizeof(d)), 0);
    CHECK(fstatus(&r).clean);
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
        {"if_format_mount", if_format_mount},
        {"if_endian", if_endian},
        {"if_names", if_names},
        {"if_alloc", if_alloc},
        {"if_write_pages", if_write_pages},
        {"if_close", if_close},
        {"if_grow", if_grow},
        {"if_reopen_unclosed", if_reopen_unclosed},
        {"if_reopen_closed", if_reopen_closed},
        {"if_read", if_read},
        {"if_delete", if_delete},
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
        {"safe_frame_check", safe_frame_check},
        {"safe_links_checked", safe_links_checked},
        {"safe_catalog_rewrite_atomic", safe_catalog_rewrite_atomic},
        {"safe_reopen_closed_cut", safe_reopen_closed_cut},
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
