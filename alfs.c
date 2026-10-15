/*
 * alfs: the append-log flash store. See alfs.h; the format is docs/SPEC.md.
 *
 * SPDX-License-Identifier: MIT
 */
#include "alfs.h"

#include <string.h>

#define VERSION 2u
#define STAMP_LEN 26u
#define CLAIM_LEN 20u
#define HDR_LEN 6u
#define PIECE 16u /* the most read onto the stack at once */

/* A stream's buffer: a sector's first bytes with prog unit 1, then the
 * payload. A claim program writes it from byte 0 (prog unit 1) or B_CLAIM. */
#define B_CLAIM (STAMP_LEN + 1u)
#define B_HDR (B_CLAIM + CLAIM_LEN)
#define BUF_HEAD (B_HDR + HDR_LEN)

#define AGAIN 100 /* a program settled something and wrote no chunk: call again */

enum { M_CLOSED, M_APPEND, M_READ };
enum { F_NEW = 1, F_FIRST = 2, F_FIRM = 4, F_CLEAR = 8 }; /* stream flags */
#define NONE 0xFFFFu
enum { E_IDLE, E_ERASE, E_LARGE, E_STAMP, E_FIRM };       /* erase step states */
#define E_NO_LARGE 0x80u                                  /* large erases failed this mount */
enum { S_READY, S_LIVE, S_DIRTY };

/* ---- bytes ------------------------------------------------------------ */

static uint32_t crc32(uint32_t c, const uint8_t *p, uint32_t n) {
    static const uint32_t t[16] = {0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
                                   0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
                                   0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
                                   0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C};
    while (n--) {
        c ^= *p++;
        c = (c >> 4) ^ t[c & 15u];
        c = (c >> 4) ^ t[c & 15u];
    }
    return c;
}

static uint32_t crc(const uint8_t *p, uint32_t n) {
    return ~crc32(0xFFFFFFFFu, p, n);
}

static uint32_t get16(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t get32(const uint8_t *p) {
    return get16(p) | get16(p + 2) << 16;
}

static void put16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}

static uint32_t lg2(uint32_t x) {
    uint32_t n = 0;
    while (x > 1u) {
        x >>= 1;
        n++;
    }
    return n;
}

static bool pow2(uint32_t x) {
    return x && !(x & (x - 1u));
}

/* ---- geometry --------------------------------------------------------- */

static uint32_t ru(const alfs_t *fs, uint32_t x) {
    uint32_t u = fs->cfg->prog_unit;
    return (x + u - 1u) & ~(u - 1u);
}

static uint32_t off_dm(const alfs_t *fs) {
    return ru(fs, STAMP_LEN);
}

static uint32_t off_claim(const alfs_t *fs) {
    return off_dm(fs) + ru(fs, 1);
}

static uint32_t off_data(const alfs_t *fs) {
    return off_claim(fs) + CLAIM_LEN;
}

static uint32_t addr_of(const alfs_t *fs, uint32_t k) {
    return fs->cfg->region_start + k * fs->cfg->sector_size;
}

static uint32_t next_page(const alfs_t *fs, uint32_t x) {
    uint32_t p = fs->cfg->page_size;
    return (x / p + 1u) * p;
}

static int check_cfg(const alfs_config_t *c, const alfs_driver_t *d) {
    if (!c || !d || !d->read || !d->prog || !d->erase)
        return ALFS_EINVAL;
    if (c->erase_value != 0xFFu && c->erase_value != 0x00u)
        return ALFS_EINVAL;
    uint32_t s = c->sector_size, p = c->page_size, u = c->prog_unit;
    if (!pow2(s) || !pow2(p) || !pow2(u) || p < 64u || u > p || s < 4u * p || s > 32768u)
        return ALFS_EGEOMETRY;
    if (c->region_start % s || c->region_size % s || !c->region_size ||
        c->region_size / s > 65535u || c->region_start > c->chip_size ||
        c->region_size > c->chip_size - c->region_start)
        return ALFS_EGEOMETRY;
    if (c->large_size && (!pow2(c->large_size) || c->large_size < s || c->large_size / s > 64u))
        return ALFS_EGEOMETRY;
    return ALFS_OK;
}

/* ---- the driver ------------------------------------------------------- */

static int rd(alfs_t *fs, uint32_t addr, void *b, uint32_t n) {
    return fs->drv->read(fs->drv->ctx, addr, b, n) ? ALFS_EIO : ALFS_OK;
}

static bool is_erased(const alfs_t *fs, const uint8_t *p, uint32_t n) {
    while (n--)
        if (*p++ != fs->cfg->erase_value)
            return false;
    return true;
}

/* *blank: every byte of [addr, addr + n) reads erased. */
static int blank(alfs_t *fs, uint32_t addr, uint32_t n, bool *blank_out) {
    uint8_t b[PIECE];
    *blank_out = false;
    while (n) {
        uint32_t k = n < PIECE ? n : PIECE;
        if (rd(fs, addr, b, k))
            return ALFS_EIO;
        if (!is_erased(fs, b, k))
            return ALFS_OK;
        addr += k;
        n -= k;
    }
    *blank_out = true;
    return ALFS_OK;
}

/* Programs, then reads back: a program that silently failed is a failure. */
static int prog(alfs_t *fs, uint32_t addr, const uint8_t *p, uint32_t n) {
    uint8_t b[PIECE];
    fs->n.programs++;
    if (fs->drv->prog(fs->drv->ctx, addr, p, n))
        return ALFS_EPROG;
    while (n) {
        uint32_t k = n < PIECE ? n : PIECE;
        if (rd(fs, addr, b, k))
            return ALFS_OK; /* unknown, not wrong: the chunk's check decides on read */
        if (memcmp(b, p, k))
            return ALFS_EPROG;
        addr += k;
        p += k;
        n -= k;
    }
    return ALFS_OK;
}

/* ---- sector headers --------------------------------------------------- */

typedef struct {
    uint8_t stamp[STAMP_LEN];
    uint8_t dead;
    uint8_t claim[CLAIM_LEN];
} hdr_t;

static int read_hdr(alfs_t *fs, uint32_t k, hdr_t *h) {
    uint32_t a = addr_of(fs, k);
    if (rd(fs, a, h->stamp, STAMP_LEN) || rd(fs, a + off_dm(fs), &h->dead, 1) ||
        rd(fs, a + off_claim(fs), h->claim, CLAIM_LEN))
        return ALFS_EIO;
    return ALFS_OK;
}

static bool stamp_valid(const uint8_t *st) {
    return !memcmp(st, "alfs", 4) && st[4] == VERSION && get32(st + 22) == crc(st, 22);
}

static bool stamp_matches(const alfs_t *fs, const uint8_t *st) {
    const alfs_config_t *c = fs->cfg;
    return st[5] == lg2(c->sector_size) && st[6] == lg2(c->page_size) &&
           st[7] == lg2(c->prog_unit) && get16(st + 8) == fs->sectors;
}

/* A claim alfs programmed zero over (SAFE-04): no torn claim reads so, since
 * it reads erased wherever its own value is (A-9). */
static bool zeroed(const alfs_t *fs, const uint8_t *cl) {
    for (uint32_t i = 0; i < CLAIM_LEN; i++)
        if (cl[i] != (uint8_t)~fs->cfg->erase_value)
            return false;
    return true;
}

static bool claim_valid(const alfs_t *fs, const uint8_t *cl) {
    return !is_erased(fs, cl, CLAIM_LEN) && get32(cl + 16) == crc(cl, 16);
}

/* A stream's first claim, whatever its dead mark: its sequence is its
 * identity, and its sector number 0. */
static bool first_of(const alfs_t *fs, const uint8_t *claim, uint32_t k) {
    return claim_valid(fs, claim) && get32(claim) == get32(claim + 4) && get16(claim + 12) == k &&
           get16(claim + 14) == 0;
}

static bool first_claim(const alfs_t *fs, const uint8_t *claim, uint8_t dead, uint32_t k) {
    return first_of(fs, claim, k) && dead == fs->cfg->erase_value;
}

static bool is_first(const alfs_t *fs, const hdr_t *h, uint32_t k) {
    return first_claim(fs, h->claim, h->dead, k);
}

static bool current(const alfs_t *fs, const uint8_t *st) {
    return stamp_valid(st) && stamp_matches(fs, st) && get32(st + 10) == fs->gen;
}

/* SPEC section 2, on a header already read: at most 19 bytes more. */
static int classify_hdr(alfs_t *fs, uint32_t k, const hdr_t *h, int *state) {
    *state = S_DIRTY;
    if (!current(fs, h->stamp))
        return ALFS_OK;
    if (h->dead == fs->cfg->erase_value && is_erased(fs, h->claim, CLAIM_LEN)) {
        *state = S_READY;
        return ALFS_OK;
    }
    if (!claim_valid(fs, h->claim))
        return ALFS_OK;
    uint32_t id = get32(h->claim), first = get16(h->claim + 12);
    if (get32(h->claim + 4) == id) {
        if (is_first(fs, h, k))
            *state = S_LIVE;
        return ALFS_OK;
    }
    if (first >= fs->sectors)
        return ALFS_OK;
    uint8_t dead, claim[CLAIM_LEN];
    uint32_t a = addr_of(fs, first);
    if (rd(fs, a + off_dm(fs), &dead, 1) || rd(fs, a + off_claim(fs), claim, CLAIM_LEN))
        return ALFS_EIO;
    if (first_claim(fs, claim, dead, first) && get32(claim) == id)
        *state = S_LIVE;
    return ALFS_OK;
}

static int classify(alfs_t *fs, uint32_t k, hdr_t *h, int *state) {
    *state = S_DIRTY;
    return read_hdr(fs, k, h) ? ALFS_EIO : classify_hdr(fs, k, h, state);
}

static bool is_bad(const alfs_t *fs, uint32_t k) {
    for (uint32_t i = 0; i < fs->nbad; i++)
        if (fs->bad[i] == k)
            return true;
    return false;
}

/* ---- mount and format ------------------------------------------------- */

static void init(alfs_t *fs, const alfs_config_t *cfg, const alfs_driver_t *drv) {
    memset(fs, 0, sizeof(*fs));
    fs->cfg = cfg;
    fs->drv = drv;
    fs->sectors = (uint16_t)(cfg->region_size / cfg->sector_size);
    fs->min_wear = UINT32_MAX;
    fs->next_seq = 1;
    fs->firmed = NONE;
    fs->fresh = NONE;
}

static void see_wear(alfs_t *fs, uint32_t w) {
    if (w < fs->min_wear)
        fs->min_wear = w;
    if (w > fs->max_wear)
        fs->max_wear = w;
}

/*
 * One pass over the region (SPEC 2): the current generation, the counts,
 * the next sequence, wear, and where allocation goes on. ALFS_ENOFS or
 * ALFS_EGEOMETRY when no stamp matches; the survey is complete either way.
 */
static int survey(alfs_t *fs, const alfs_config_t *cfg, const alfs_driver_t *drv) {
    int rc = check_cfg(cfg, drv);
    if (rc)
        return rc;
    init(fs, cfg, drv);
    bool any = false, mismatch = false, live_any = false, newest_any = false;
    uint32_t ready = 0, live = 0, dirty = 0, last_seq = 0, last_k = 0, seq_max = 0;
    uint32_t newest = 0, fresh_after = NONE, fresh_any = NONE; /* SAFE-04 rule 5 */
    for (uint32_t k = 0; k < fs->sectors; k++) {
        hdr_t h;
        int st = S_DIRTY;
        if (read_hdr(fs, k, &h))
            return ALFS_EIO;
        if (claim_valid(fs, h.claim) && get32(h.claim + 4) > seq_max)
            seq_max = get32(h.claim + 4);
        if (stamp_valid(h.stamp) && !stamp_matches(fs, h.stamp))
            mismatch = true;
        if (stamp_valid(h.stamp) && stamp_matches(fs, h.stamp)) {
            uint32_t g = get32(h.stamp + 10);
            see_wear(fs, get32(h.stamp + 14));
            if (get32(h.stamp + 18) > seq_max)
                seq_max = get32(h.stamp + 18);
            if (!any || g > fs->gen) { /* a newer generation: all before it is dirty */
                dirty += ready + live;
                ready = live = 0;
                live_any = newest_any = false;
                fresh_after = fresh_any = NONE;
                fs->gen = g;
                any = true;
            }
            if (classify_hdr(fs, k, &h, &st))
                return ALFS_EIO;
            if (g == fs->gen && claim_valid(fs, h.claim) && (!newest_any || get32(h.claim + 4) > newest)) {
                newest = get32(h.claim + 4);
                newest_any = true;
                fresh_after = NONE;
            }
            /* a sector a claim program could have been taking: erased or torn,
             * however its bits read now, and not one alfs zeroed */
            if (g == fs->gen && !claim_valid(fs, h.claim) && !zeroed(fs, h.claim)) {
                if (fresh_any == NONE)
                    fresh_any = k;
                if (fresh_after == NONE && newest_any)
                    fresh_after = k;
            }
        }
        if (st == S_READY) {
            ready++;
        } else if (st == S_LIVE) {
            live++;
            if (!live_any || get32(h.claim + 4) > last_seq) {
                last_seq = get32(h.claim + 4);
                last_k = k;
                live_any = true;
            }
        } else {
            dirty++;
        }
    }
    fs->next_seq = seq_max + 2u; /* past a torn claim's: SPEC 1.1 */
    if (cfg->prog_unit == 1u) /* where the allocation cursor stood: SAFE-04 rule 5 */
        fs->fresh = (uint16_t)(fresh_after != NONE ? fresh_after : fresh_any);
    fs->ready = (uint16_t)ready;
    fs->dirty = (uint16_t)dirty;
    fs->alloc_cursor = (uint16_t)(live_any ? (last_k + 1u) % fs->sectors : 0u);
    fs->erase_cursor = fs->alloc_cursor;
    return any ? ALFS_OK : mismatch ? ALFS_EGEOMETRY : ALFS_ENOFS;
}

int alfs_mount(alfs_t *fs, const alfs_config_t *cfg, const alfs_driver_t *drv) {
    int rc = survey(fs, cfg, drv);
    if (rc)
        fs->cfg = NULL; /* not mounted */
    return rc;
}

/* The erase count sector k takes: SPEC 1.1. */
static int wear_for(alfs_t *fs, uint32_t k, uint32_t n, uint32_t *w) {
    uint32_t best = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t st[STAMP_LEN];
        if (rd(fs, addr_of(fs, k + i), st, STAMP_LEN))
            return ALFS_EIO;
        if (stamp_valid(st) && stamp_matches(fs, st) && get32(st + 14) > best)
            best = get32(st + 14);
    }
    *w = best ? best + 1u : (fs->max_wear ? fs->max_wear : 1u);
    return ALFS_OK;
}

static int erase_go(alfs_t *fs);

int alfs_format(alfs_t *fs, const alfs_config_t *cfg, const alfs_driver_t *drv) {
    int rc = survey(fs, cfg, drv);
    if (rc == ALFS_EIO || rc == ALFS_EINVAL || (rc == ALFS_EGEOMETRY && check_cfg(cfg, drv)))
        return rc;
    if (wear_for(fs, 0, 1, &fs->ewear))
        return ALFS_EIO;
    fs->gen++;
    fs->fresh = NONE;
    fs->min_wear = UINT32_MAX;
    fs->ready = 0;
    fs->dirty = fs->sectors;
    fs->alloc_cursor = fs->erase_cursor = 0;
    fs->esector = 0;
    fs->estate = E_ERASE;
    return erase_go(fs);
}

/* ---- erasing ---------------------------------------------------------- */

static uint32_t large_sectors(const alfs_t *fs) {
    if (!fs->cfg->large_size || !fs->drv->erase_large || (fs->estate & E_NO_LARGE))
        return 0;
    return fs->cfg->large_size / fs->cfg->sector_size;
}

static void lose_dirty(alfs_t *fs) {
    if (fs->dirty)
        fs->dirty--;
}

static void mark_bad(alfs_t *fs, uint32_t k) {
    fs->n.erase_failures++;
    lose_dirty(fs);
    if (!is_bad(fs, k) && fs->nbad < ALFS_BAD_MAX)
        fs->bad[fs->nbad++] = (uint16_t)k;
}

static uint32_t stamp_count(const alfs_t *fs) {
    return (fs->estate & 0x7Fu) == E_STAMP && fs->estamped & 0x80u ? large_sectors(fs) : 1u;
}

static void idle(alfs_t *fs, uint32_t next) {
    fs->estate = (uint8_t)((fs->estate & E_NO_LARGE) | E_IDLE);
    fs->estamped = 0;
    fs->erase_cursor = (uint16_t)(next % fs->sectors);
}

/* One sector of an erase that completed: check it erased, stamp it. */
static int stamp_next(alfs_t *fs) {
    uint32_t n = stamp_count(fs);
    uint32_t k = fs->esector + (fs->estamped & 0x7Fu);
    uint32_t a = addr_of(fs, k);
    bool ok;
    if (blank(fs, a, fs->cfg->sector_size, &ok))
        return ALFS_EIO;
    if (ok) {
        uint8_t st[STAMP_LEN];
        memcpy(st, "alfs", 4);
        st[4] = VERSION;
        st[5] = (uint8_t)lg2(fs->cfg->sector_size);
        st[6] = (uint8_t)lg2(fs->cfg->page_size);
        st[7] = (uint8_t)lg2(fs->cfg->prog_unit);
        put16(st + 8, fs->sectors);
        put32(st + 10, fs->gen);
        put32(st + 14, fs->ewear);
        put32(st + 18, fs->next_seq);
        put32(st + 22, crc(st, 22));
        ok = prog(fs, a, st, STAMP_LEN) == ALFS_OK;
    }
    if (ok) {
        fs->ready++;
        lose_dirty(fs);
        see_wear(fs, fs->ewear);
    } else {
        mark_bad(fs, k);
    }
    fs->estamped++;
    if ((fs->estamped & 0x7Fu) >= n)
        idle(fs, fs->esector + n);
    return ALFS_OK;
}

/*
 * What sector k is now, from one read of its stream's dead mark: 2 when it
 * reads live, 1 when it belongs to a deleted stream and is not its first
 * (*first set), 0 when it is dirty otherwise. A bit that reads either way
 * can make a sector read dirty when chosen and live a moment later.
 */
static int reclaim_kind(alfs_t *fs, uint32_t k, uint32_t *first) {
    hdr_t h;
    if (read_hdr(fs, k, &h))
        return ALFS_EIO;
    if (!current(fs, h.stamp) || !claim_valid(fs, h.claim))
        return 0;
    uint32_t id = get32(h.claim), f = get16(h.claim + 12);
    if (get32(h.claim + 4) == id)
        return first_claim(fs, h.claim, h.dead, k) ? 2 : 0;
    if (f >= fs->sectors)
        return 0;
    uint8_t dead, claim[CLAIM_LEN];
    uint32_t a = addr_of(fs, f);
    if (rd(fs, a + off_dm(fs), &dead, 1) || rd(fs, a + off_claim(fs), claim, CLAIM_LEN))
        return ALFS_EIO;
    if (!first_of(fs, claim, f) || get32(claim) != id)
        return 0;
    if (dead == fs->cfg->erase_value)
        return 2;
    *first = f;
    return 1;
}

/*
 * SPEC 5: before an erase, each sector to be erased read again. One that
 * reads live is left for later; a deleted stream's dead mark is programmed
 * again, one a call. ALFS_OK after a program or when it gave up; 1 when the
 * erase may go on.
 */
static int firm_marks(alfs_t *fs) {
    uint32_t n = (fs->estamped & 0x80u) ? large_sectors(fs) : 1u;
    while ((fs->estamped & 0x7Fu) < n) {
        uint32_t first = 0;
        int rc = reclaim_kind(fs, fs->esector + (fs->estamped & 0x7Fu), &first);
        if (rc < 0)
            return rc;
        if (rc == 2) {
            idle(fs, fs->esector + 1u);
            return ALFS_OK;
        }
        fs->estamped++;
        if (rc == 1 && first != fs->firmed) {
            uint8_t mark = (uint8_t)~fs->cfg->erase_value;
            fs->firmed = (uint16_t)first;
            if (prog(fs, addr_of(fs, first) + off_dm(fs), &mark, 1))
                fs->n.prog_failures++;
            return ALFS_OK;
        }
    }
    return 1;
}

static int erase_go(alfs_t *fs) {
    uint8_t st = fs->estate & 0x7Fu;
    if (st == E_STAMP)
        return stamp_next(fs);
    if (st == E_FIRM) {
        int rc = firm_marks(fs);
        if (rc <= 0)
            return rc;
        st = (fs->estamped & 0x80u) ? E_LARGE : E_ERASE;
        fs->estate = (uint8_t)((fs->estate & E_NO_LARGE) | st);
        fs->estamped = 0;
    }
    uint32_t a = addr_of(fs, fs->esector);
    int rc = st == E_LARGE ? fs->drv->erase_large(fs->drv->ctx, a) : fs->drv->erase(fs->drv->ctx, a);
    if (rc == ALFS_BUSY)
        return ALFS_BUSY;
    if (rc) {
        if (st == E_LARGE) { /* go sector by sector; the bad one shows itself */
            fs->estate = E_NO_LARGE | E_IDLE;
            return ALFS_OK;
        }
        mark_bad(fs, fs->esector);
        idle(fs, fs->esector + 1u);
        return ALFS_OK;
    }
    fs->n.erases++;
    fs->estate = (uint8_t)((fs->estate & E_NO_LARGE) | E_STAMP);
    fs->estamped = st == E_LARGE ? 0x80u : 0u;
    return stamp_next(fs);
}

/* A whole aligned block of dirty sectors at k, for one large erase. */
static int large_here(alfs_t *fs, uint32_t k, hdr_t *h, bool *yes) {
    uint32_t n = large_sectors(fs);
    *yes = false;
    if (!n || addr_of(fs, k) % fs->cfg->large_size || k + n > fs->sectors)
        return ALFS_OK;
    for (uint32_t i = 0; i < n; i++) {
        int s;
        if (is_bad(fs, k + i))
            return ALFS_OK;
        if (classify(fs, k + i, h, &s))
            return ALFS_EIO;
        if (s != S_DIRTY)
            return ALFS_OK;
    }
    *yes = true;
    return ALFS_OK;
}

int alfs_erase_step(alfs_t *fs) {
    if (!fs->cfg)
        return ALFS_EINVAL;
    if ((fs->estate & 0x7Fu) != E_IDLE)
        return erase_go(fs);
    if (!fs->dirty)
        return ALFS_CLEAN;
    for (uint32_t i = 0; i < fs->sectors; i++) {
        uint32_t k = (fs->erase_cursor + i) % fs->sectors;
        hdr_t h;
        int s;
        if (is_bad(fs, k))
            continue;
        if (classify(fs, k, &h, &s))
            return ALFS_EIO;
        if (s != S_DIRTY)
            continue;
        bool large;
        if (large_here(fs, k, &h, &large))
            return ALFS_EIO;
        uint32_t n = large ? large_sectors(fs) : 1u;
        if (wear_for(fs, k, n, &fs->ewear))
            return ALFS_EIO;
        fs->esector = (uint16_t)k;
        if (fs->cfg->prog_unit == 1u) {
            fs->estate = (uint8_t)((fs->estate & E_NO_LARGE) | E_FIRM);
            fs->estamped = large ? 0x80u : 0u;
        } else {
            fs->estate = (uint8_t)((fs->estate & E_NO_LARGE) | (large ? E_LARGE : E_ERASE));
        }
        return erase_go(fs);
    }
    fs->dirty = 0;
    return ALFS_CLEAN;
}

/* ---- finding streams -------------------------------------------------- */

/* 1 when sector k holds a current, valid claim, 0 when not, or ALFS_EIO. */
static int claimed(alfs_t *fs, uint32_t k, hdr_t *h) {
    if (read_hdr(fs, k, h))
        return ALFS_EIO;
    return current(fs, h->stamp) && claim_valid(fs, h->claim);
}

/* The stream's first sector, and with last, its newest. */
static int find_stream(alfs_t *fs, uint32_t id, uint32_t *first, uint32_t *last, uint32_t *count) {
    bool found = false;
    uint32_t best = 0;
    *count = 0;
    for (uint32_t k = 0; k < fs->sectors; k++) {
        hdr_t h;
        int c = claimed(fs, k, &h);
        if (c < 0)
            return c;
        if (!c || get32(h.claim) != id)
            continue;
        (*count)++;
        if (is_first(fs, &h, k)) {
            *first = k;
            found = true;
        }
        if (last && get32(h.claim + 4) >= best) {
            best = get32(h.claim + 4);
            *last = k;
        }
    }
    return found ? ALFS_OK : ALFS_ENOENT;
}

/*
 * SPEC 3: the stream's sector with the least number greater than n, found
 * forward from *k_out; the scan stops at n + 1, usually a few sectors on.
 */
static int next_sector(alfs_t *fs, uint32_t id, uint32_t n, uint32_t *k_out, uint32_t *n_out) {
    bool found = false;
    uint32_t from = *k_out;
    for (uint32_t i = 0; i < fs->sectors; i++) {
        uint32_t k = (from + 1u + i) % fs->sectors;
        hdr_t h;
        int c = claimed(fs, k, &h);
        if (c < 0)
            return c;
        uint32_t m = get16(h.claim + 14);
        if (!c || get32(h.claim) != id || m <= n)
            continue;
        if (!found || m < *n_out) {
            *n_out = m;
            *k_out = k;
            found = true;
            if (m == n + 1u)
                break;
        }
    }
    return found ? ALFS_OK : ALFS_ENOENT;
}

/*
 * SPEC section 3: from *pos in sector k, the next good chunk. 1 with *pos
 * and *len set, 0 at the end, or ALFS_EIO. A good chunk's payload lands in
 * buf when one is given.
 */
static int scan_chunk(alfs_t *fs, uint32_t k, uint32_t *pos, uint32_t *len, uint8_t *buf, bool count) {
    uint32_t base = addr_of(fs, k), x = *pos, sz = fs->cfg->sector_size;
    while (x + HDR_LEN < sz) {
        uint8_t h[HDR_LEN];
        uint32_t np = next_page(fs, x);
        if (np - x < HDR_LEN + 1u) { /* no chunk fits here: the writer moved on too */
            x = np;
            continue;
        }
        if (rd(fs, base + x, h, HDR_LEN))
            return ALFS_EIO;
        if (is_erased(fs, h, HDR_LEN)) { /* a page a resume passed over, or the tail */
            x = np;
            continue;
        }
        uint32_t l = get16(h), c = crc32(0xFFFFFFFFu, h, 2);
        bool good = l && x + HDR_LEN + l <= np;
        for (uint32_t i = 0; good && i < l;) {
            uint8_t piece[PIECE];
            uint8_t *p = buf ? buf + i : piece;
            uint32_t n = l - i < PIECE ? l - i : PIECE;
            if (rd(fs, base + x + HDR_LEN + i, p, n))
                return ALFS_EIO;
            c = crc32(c, p, n);
            i += n;
        }
        if (good && ~c == get32(h + 2)) {
            *pos = x;
            *len = l;
            return 1;
        }
        if (count)
            fs->n.check_failures++;
        x = np;
    }
    return 0;
}

int alfs_next(alfs_t *fs, uint32_t after, alfs_info_t *info) {
    bool found = false;
    for (uint32_t k = 0; k < fs->sectors; k++) {
        hdr_t h;
        int c = claimed(fs, k, &h);
        if (c < 0)
            return c;
        uint32_t id = get32(h.claim);
        if (c && is_first(fs, &h, k) && id > after && (!found || id < info->id)) {
            info->id = id;
            info->tag = get32(h.claim + 8);
            found = true;
        }
    }
    return found ? ALFS_OK : ALFS_ENOENT;
}

int alfs_delete(alfs_t *fs, uint32_t id) {
    uint32_t first, count;
    int rc = find_stream(fs, id, &first, NULL, &count);
    if (rc)
        return rc;
    uint8_t mark = (uint8_t)~fs->cfg->erase_value;
    if (prog(fs, addr_of(fs, first) + off_dm(fs), &mark, 1)) {
        fs->n.prog_failures++;
        return ALFS_EPROG;
    }
    fs->dirty = (uint16_t)(fs->dirty + count);
    return ALFS_OK;
}

/* ---- appending -------------------------------------------------------- */

static uint32_t page_limit(const alfs_t *fs, const alfs_stream_t *s, uint32_t page) {
    uint32_t lim = fs->cfg->max_page_programs ? fs->cfg->max_page_programs : 16u;
    if (s->sector == s->first && page == off_dm(fs) / fs->cfg->page_size)
        lim--; /* the dead mark's program */
    return lim;
}

/* Moves the next chunk's place on until a byte of payload fits there. */
static void place(alfs_t *fs, alfs_stream_t *s) {
    uint32_t p = fs->cfg->page_size;
    while (!(s->flags & F_NEW)) {
        if (s->off + HDR_LEN + 1u > fs->cfg->sector_size) {
            s->flags |= F_NEW;
            return;
        }
        uint32_t np = next_page(fs, s->off);
        if (np - s->off < HDR_LEN + 1u || s->page_progs >= page_limit(fs, s, s->off / p)) {
            s->off = (uint16_t)np;
            s->page_progs = 0;
            continue;
        }
        return;
    }
}

static uint32_t capacity(const alfs_t *fs, const alfs_stream_t *s) {
    uint32_t p = fs->cfg->page_size;
    if (s->flags & F_NEW)
        return p - off_data(fs) % p - HDR_LEN;
    return next_page(fs, s->off) - s->off - HDR_LEN;
}

/* Zero over sector k's claim, which makes it dirty for good (A-9). */
static void zero_claim(alfs_t *fs, uint32_t k) {
    static const uint8_t z[2][CLAIM_LEN] = {{0}, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
    if (prog(fs, addr_of(fs, k) + off_claim(fs), z[fs->cfg->erase_value == 0x00u], CLAIM_LEN))
        fs->n.prog_failures++;
}

/* A ready sector for a new claim, its claim page checked erased. */
static int find_ready(alfs_t *fs, uint32_t *out) {
    uint32_t p = fs->cfg->page_size, c = off_claim(fs);
    uint32_t from = off_dm(fs) / p == c / p ? off_dm(fs) : c;
    for (uint32_t i = 0; fs->ready && i < fs->sectors; i++) {
        uint32_t k = (fs->alloc_cursor + i) % fs->sectors;
        hdr_t h;
        int s;
        bool ok;
        if (is_bad(fs, k) || ((fs->estate & 0x7Fu) != E_IDLE && k >= fs->esector &&
                              k < fs->esector + stamp_count(fs)))
            continue;
        if (classify(fs, k, &h, &s))
            return ALFS_EIO;
        if (s != S_READY)
            continue;
        if (blank(fs, addr_of(fs, k) + from, next_page(fs, c) - from, &ok))
            return ALFS_EIO;
        if (ok && k == fs->fresh) { /* SAFE-04 rule 5 */
            fs->fresh = NONE;
            fs->ready--;
            fs->dirty++;
            zero_claim(fs, k);
            return AGAIN;
        }
        if (ok) {
            *out = k;
            return ALFS_OK;
        }
        fs->ready--; /* touched: SAFE-05 */
        fs->dirty++;
    }
    return ALFS_ENOSPC;
}

static int failed(alfs_t *fs, alfs_stream_t *s) {
    uint32_t max = fs->cfg->max_prog_retries ? fs->cfg->max_prog_retries : 3u;
    fs->n.prog_failures++;
    s->flags |= F_NEW;
    if (++s->fails < max)
        return ALFS_EPROG;
    fs->n.bytes_lost += s->used;
    s->used = 0;
    s->fails = 0;
    return ALFS_ELOST;
}

/* The claim of the stream's sector numbered n, built at buf + B_CLAIM. */
static void build_claim(alfs_stream_t *s, uint32_t seq, uint32_t first, uint32_t n) {
    uint8_t *c = s->buf + B_CLAIM;
    put32(c, s->id);
    put32(c + 4, seq);
    put32(c + 8, s->tag);
    put16(c + 12, first);
    put16(c + 14, n);
    put32(c + 16, crc(c, 16));
}

/* Sector k's stamp, which must be valid, and an erased dead mark, at buf. */
static int head_of(alfs_t *fs, uint32_t k, uint8_t *buf) {
    if (rd(fs, addr_of(fs, k), buf, STAMP_LEN))
        return ALFS_EIO;
    if (!stamp_valid(buf) || !stamp_matches(fs, buf) || get32(buf + 10) != fs->gen)
        return ALFS_EIO; /* a bit that reads either way: read again */
    buf[STAMP_LEN] = fs->cfg->erase_value;
    return ALFS_OK;
}

/* SAFE-04 rule 1: the last sector's stamp and claim, programmed again. */
static int firm(alfs_t *fs, alfs_stream_t *s) {
    if (head_of(fs, s->sector, s->buf))
        return ALFS_EIO;
    build_claim(s, s->seq, s->first, s->index);
    s->flags &= (uint8_t)~F_FIRM;
    if (prog(fs, addr_of(fs, s->sector), s->buf, B_HDR))
        return failed(fs, s);
    return AGAIN;
}

/* A torn claim's fields read erased wherever the stream's own are (A-9). */
static bool could_be(const alfs_t *fs, const uint8_t *cl, const alfs_stream_t *s) {
    uint8_t want[16];
    put32(want, s->id);
    put32(want + 8, s->tag);
    put16(want + 12, s->first);
    put16(want + 14, s->index + 1u);
    for (uint32_t i = 0; i < 16; i++) {
        if (i >= 4 && i < 8)
            continue; /* the sequence is unknown */
        uint8_t ew = (uint8_t)~(want[i] ^ fs->cfg->erase_value); /* erased in want */
        uint8_t ec = (uint8_t)~(cl[i] ^ fs->cfg->erase_value);
        if (ew & (uint8_t)~ec)
            return false;
    }
    return true;
}

/* SAFE-04 rule 3: zero over one claim that could stand after the stream's
 * last sector. AGAIN after a program; 0 when none is left. */
static int clear_ahead(alfs_t *fs, alfs_stream_t *s) {
    uint8_t *st = s->buf, *cl = s->buf + B_CLAIM; /* scratch until the claim is built */
    for (uint32_t k = 0; k < fs->sectors; k++) {
        uint32_t a = addr_of(fs, k);
        if (k == s->sector || is_bad(fs, k))
            continue;
        if (rd(fs, a, st, STAMP_LEN) || rd(fs, a + off_claim(fs), cl, CLAIM_LEN))
            return ALFS_EIO;
        if (!current(fs, st) || is_erased(fs, cl, CLAIM_LEN))
            continue;
        bool valid = claim_valid(fs, cl);
        if (valid ? get32(cl) != s->id || get16(cl + 14) <= s->index : !could_be(fs, cl, s))
            continue;
        fs->dirty = (uint16_t)(fs->dirty + valid);
        zero_claim(fs, k);
        return AGAIN;
    }
    s->flags &= (uint8_t)~F_CLEAR;
    return ALFS_OK;
}

/*
 * Programs the chunk being built: one program. After a failure moved the
 * stream to a new sector, less may fit than was held; the rest stays for
 * the next call. AGAIN: the program settled something instead.
 */
static int put_chunk(alfs_t *fs, alfs_stream_t *s) {
    if (s->flags & F_FIRM)
        return firm(fs, s);
    if (s->flags & F_CLEAR) {
        int rc = clear_ahead(fs, s);
        if (rc)
            return rc;
    }
    uint8_t *h = s->buf + B_HDR;
    uint32_t n = capacity(fs, s) < s->used ? capacity(fs, s) : s->used;
    put16(h, n);
    put32(h + 2, ~crc32(crc32(0xFFFFFFFFu, h, 2), s->buf + BUF_HEAD, n));
    uint32_t p = fs->cfg->page_size;

    if (!(s->flags & F_NEW)) {
        uint32_t page = s->off / p;
        if (prog(fs, addr_of(fs, s->sector) + s->off, h, HDR_LEN + n))
            return failed(fs, s);
        s->off = (uint16_t)ru(fs, s->off + HDR_LEN + n);
        s->page_progs = s->off / p == page ? (uint8_t)(s->page_progs + 1u) : 0u;
    } else {
        uint32_t k, c = off_claim(fs);
        int rc = find_ready(fs, &k);
        if (rc)
            return rc;
        bool whole = fs->cfg->prog_unit == 1u; /* SAFE-04 rule 4: from byte 0 */
        if (whole && head_of(fs, k, s->buf))
            return ALFS_EIO;
        uint32_t seq = (s->flags & F_FIRST) ? s->id : fs->next_seq++;
        uint32_t first = (s->flags & F_FIRST) ? k : s->first;
        uint32_t num = (s->flags & F_FIRST) ? 0u : s->index + 1u;
        build_claim(s, seq, first, num);
        fs->ready--;
        fs->alloc_cursor = (uint16_t)((k + 1u) % fs->sectors);
        int bad = whole ? prog(fs, addr_of(fs, k), s->buf, BUF_HEAD + n)
                        : prog(fs, addr_of(fs, k) + c, s->buf + B_CLAIM, CLAIM_LEN + HDR_LEN + n);
        if (bad) {
            fs->dirty++;
            if (s->flags & F_FIRST)
                s->id = fs->next_seq++; /* the failed sector may hold a first claim */
            return failed(fs, s);
        }
        s->sector = (uint16_t)k;
        s->seq = seq;
        s->first = (uint16_t)first;
        s->index = (uint16_t)num;
        s->flags = 0;
        s->off = (uint16_t)ru(fs, c + CLAIM_LEN + HDR_LEN + n);
        s->page_progs = s->off / p == c / p ? (uint8_t)(c / p == 0 ? 2u : 1u) : 0u;
    }
    memmove(s->buf + BUF_HEAD, s->buf + BUF_HEAD + n, s->used - n);
    s->used = (uint16_t)(s->used - n);
    s->fails = 0;
    place(fs, s);
    return ALFS_OK;
}

int alfs_create(alfs_t *fs, alfs_stream_t *s, uint8_t *buf, uint32_t tag) {
    if (!fs->cfg || !s || !buf)
        return ALFS_EINVAL;
    memset(s, 0, sizeof(*s));
    s->id = fs->next_seq++;
    s->tag = tag;
    s->buf = buf;
    s->mode = M_APPEND;
    s->flags = F_NEW | F_FIRST;
    return ALFS_OK;
}

/*
 * SAFE-04 (SPEC 4): the power loss before the mount interrupted at most the
 * next chunk in the last good chunk's page, the first of the next page, or
 * a new sector's claim. Neither page takes new data, and the first chunk is
 * preceded by the claim programmed again and the claims cleared ahead. A
 * unit over 1 takes no second program: a new sector.
 */
int alfs_open_append(alfs_t *fs, alfs_stream_t *s, uint8_t *buf, uint32_t id) {
    uint32_t first, last, count;
    if (!fs->cfg || !s || !buf)
        return ALFS_EINVAL;
    int rc = find_stream(fs, id, &first, &last, &count);
    if (rc)
        return rc;
    hdr_t h;
    if (read_hdr(fs, last, &h))
        return ALFS_EIO;
    memset(s, 0, sizeof(*s));
    s->id = id;
    s->tag = get32(h.claim + 8);
    s->seq = get32(h.claim + 4);
    s->index = (uint16_t)get16(h.claim + 14);
    s->buf = buf;
    s->sector = (uint16_t)last;
    s->first = (uint16_t)first;
    s->mode = M_APPEND;
    s->flags = F_NEW;
    if (fs->cfg->prog_unit != 1u)
        return ALFS_OK;
    uint32_t p = fs->cfg->page_size, x = off_data(fs), l = 0, page = off_claim(fs) / p;
    for (;;) {
        rc = scan_chunk(fs, last, &x, &l, NULL, false);
        if (rc < 0)
            return rc;
        if (!rc)
            break;
        page = x / p;
        x = ru(fs, x + HDR_LEN + l);
    }
    uint32_t at = (page + 2u) * p;
    s->flags = F_NEW | F_CLEAR;
    if (at + HDR_LEN + 1u <= fs->cfg->sector_size) {
        s->off = (uint16_t)at;
        s->flags = F_FIRM | F_CLEAR;
    }
    return ALFS_OK;
}

int32_t alfs_append(alfs_t *fs, alfs_stream_t *s, const void *data, uint32_t len) {
    if (!s || s->mode != M_APPEND || (len && !data))
        return ALFS_EINVAL;
    if (!len)
        return 0;
    uint32_t cap = capacity(fs, s);
    if (s->used >= cap) {
        int rc = put_chunk(fs, s);
        if (rc == AGAIN)
            return 0;
        if (rc)
            return rc;
        cap = capacity(fs, s);
    }
    uint32_t n = cap - s->used < len ? cap - s->used : len;
    memcpy(s->buf + BUF_HEAD + s->used, data, n);
    s->used = (uint16_t)(s->used + n);
    return (int32_t)n;
}

int alfs_sync(alfs_t *fs, alfs_stream_t *s) {
    if (!s || s->mode != M_APPEND)
        return ALFS_EINVAL;
    if (!s->used)
        return ALFS_OK;
    int rc = put_chunk(fs, s);
    if (rc == AGAIN)
        return ALFS_BUSY;
    return rc ? rc : s->used ? ALFS_BUSY : ALFS_OK;
}

int alfs_close(alfs_t *fs, alfs_stream_t *s) {
    int rc = alfs_sync(fs, s);
    if (!rc)
        s->mode = M_CLOSED;
    return rc;
}

/* ---- reading ---------------------------------------------------------- */

int alfs_open_read(alfs_t *fs, alfs_stream_t *s, uint8_t *buf, uint32_t id) {
    uint32_t first, count;
    if (!fs->cfg || !s || !buf)
        return ALFS_EINVAL;
    int rc = find_stream(fs, id, &first, NULL, &count);
    if (rc)
        return rc;
    memset(s, 0, sizeof(*s));
    s->id = id;
    s->seq = id;
    s->index = 0;
    s->buf = buf;
    s->sector = (uint16_t)first;
    s->off = (uint16_t)off_data(fs);
    s->mode = M_READ;
    return ALFS_OK;
}

int32_t alfs_read(alfs_t *fs, alfs_stream_t *s, void *out, uint32_t len) {
    if (!s || s->mode != M_READ || (len && !out))
        return ALFS_EINVAL;
    for (uint32_t guard = 0; guard <= fs->sectors; guard++) {
        if (s->used) {
            uint32_t n = s->used < len ? s->used : len;
            memcpy(out, s->buf + (s->first - s->used), n);
            s->used = (uint16_t)(s->used - n);
            return (int32_t)n;
        }
        uint32_t x = s->off, l = 0;
        int rc = scan_chunk(fs, s->sector, &x, &l, s->buf, true);
        if (rc < 0)
            return rc;
        if (rc == 1) {
            s->off = (uint16_t)ru(fs, x + HDR_LEN + l);
            s->first = (uint16_t)l;
            s->used = (uint16_t)l;
            guard--;
            continue;
        }
        uint32_t k = s->sector, n = 0;
        rc = next_sector(fs, s->id, s->index, &k, &n);
        if (rc == ALFS_ENOENT)
            return 0;
        if (rc)
            return rc;
        if (n != s->index + 1u)
            fs->n.check_failures++; /* SAFE-12: a sector missing */
        s->sector = (uint16_t)k;
        s->index = (uint16_t)n;
        s->off = (uint16_t)off_data(fs);
    }
    return 0;
}

void alfs_status(const alfs_t *fs, alfs_status_t *st) {
    uint32_t per = fs->cfg ? fs->cfg->sector_size - off_data(fs) - HDR_LEN : 0;
    memset(st, 0, sizeof(*st));
    st->sectors = fs->sectors;
    st->ready = fs->ready;
    st->dirty = fs->dirty;
    st->bad = fs->nbad;
    st->appendable = fs->ready * per;
    st->min_wear = fs->min_wear == UINT32_MAX ? 0 : fs->min_wear;
    st->max_wear = fs->max_wear;
    st->clean = !fs->dirty && (fs->estate & 0x7Fu) == E_IDLE;
    st->n = fs->n;
}
