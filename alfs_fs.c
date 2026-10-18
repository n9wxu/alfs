/*
 * alfs_fs: files of linked pages. See alfs_fs.h; the format is docs/SPEC.md.
 *
 * SPDX-License-Identifier: MIT
 */
#include "alfs_fs.h"

#include "alfs_flash.h"

#include <string.h>

#define HDR 14u /* a page header: frame ID, page count, check, next page */
#define O_POS 4u
#define O_HCHK 6u
#define O_NEXT 10u
#define FB 51u  /* the frame block, on a first page */
#define FB_NAME 4u
#define FB_VER 36u
#define FB_OPEN 37u    /* the open check, of the bytes before it */
#define FB_CLOSE 41u   /* page count, size, close check */
#define FB_CHECKED 47u /* the bytes the close check covers */
#define NAME_LEN 32u
#define CHK 4u
#define NONE 0xFFFFFFFFu
#define COUNT_NONE 0xFFFFu
#define TRIES 3u
#define LOOKS 16u /* reads of an allocation's bytes before a page counts free */

enum { F_CLOSED = 1, F_WRITER = 2, F_STALE = 4, F_MOUNTED = 8, F_READER = 16 };

/* A record's readers count in its flags' high bits; a writer or a reader
 * makes a copy busy. */
#define READERS(r) ((uint32_t)(r)->flags >> 4)
#define BUSY(r) (((r)->flags & F_WRITER) || READERS(r))

typedef struct {
    uint32_t id, next;
    uint16_t pos;
    bool valid, erased;
} hdr_t;

/* ---- geometry and fields -------------------------------------------- */

static uint32_t psize(const alfs_fs_t *fs) {
    return fs->d.cfg->page_size;
}

static uint32_t pps(const alfs_fs_t *fs) {
    return fs->d.cfg->sector_size / psize(fs);
}

static uint32_t paddr(const alfs_fs_t *fs, uint32_t p) {
    return fs->d.cfg->start + p * psize(fs);
}

/* Where a page's byte count lies, and how much data it holds at most. */
static uint32_t data_at(uint32_t pos) {
    return pos ? HDR : HDR + FB;
}

static uint32_t dcap(const alfs_fs_t *fs, uint32_t pos) {
    return psize(fs) - data_at(pos) - 2u - CHK;
}

/* Copy A is newer than copy B, across the 8-bit version's wrap. */
static bool newer(uint8_t a, uint8_t b) {
    uint8_t d = (uint8_t)(a - b);
    return d && d < 128u;
}

static uint32_t name_len(const char *name) {
    uint32_t n = 0;
    while (name && n <= ALFS_NAME_MAX && name[n])
        n++;
    return n <= ALFS_NAME_MAX ? n : 0u;
}

/* A frame block's filename: its length, 0 when blocked out or not one. */
static uint32_t fb_name_len(const uint8_t *nm) {
    uint32_t n = 0;
    while (n < NAME_LEN && nm[n])
        n++;
    if (!n || n == NAME_LEN)
        return 0;
    for (uint32_t i = n; i < NAME_LEN; i++)
        if (nm[i])
            return 0;
    return n;
}

static uint16_t name_hash(const uint8_t *name, uint32_t n) {
    return (uint16_t)alfs_crc(ALFS_CRC_INIT, name, n);
}

static uint32_t page_check(uint32_t id, uint32_t pos, const uint8_t *data, uint32_t n) {
    uint8_t h[8];
    alfs_put32(h, id);
    alfs_put16(h + 4, pos);
    alfs_put16(h + 6, n);
    return ~alfs_crc(alfs_crc(ALFS_CRC_INIT, h, 8), data, n);
}

/* ---- pages ---------------------------------------------------------- */

static uint32_t hdr_check(const uint8_t *b) {
    return ~alfs_crc(ALFS_CRC_INIT, b, 6);
}

/* SPEC 2: frame block fb holds a copy of frame id: its open check is right,
 * and its filename is not blocked out. */
static bool is_copy(const uint8_t *fb, uint32_t id) {
    return alfs_get32(fb) == id && alfs_get32(fb + FB_OPEN) == ~alfs_crc(ALFS_CRC_INIT, fb, FB_OPEN) &&
           fb_name_len(fb + FB_NAME);
}

static int read_hdr(alfs_fs_t *fs, uint32_t p, hdr_t *h) {
    uint8_t b[HDR];
    if (alfs_read(&fs->d, paddr(fs, p), b, HDR))
        return ALFS_EIO;
    h->erased = alfs_is_erased(b, HDR);
    h->id = alfs_get32(b);
    h->pos = (uint16_t)alfs_get16(b + O_POS);
    h->next = alfs_get32(b + O_NEXT);
    h->valid = !h->erased && h->id && alfs_get32(b + O_HCHK) == hdr_check(b);
    return ALFS_OK;
}

static bool is_bad(const alfs_fs_t *fs, uint32_t k) {
    for (uint32_t i = 0; i < fs->nbad; i++)
        if (fs->bad[i] == k)
            return true;
    return false;
}

static void mark_bad(alfs_fs_t *fs, uint32_t k) {
    if (!is_bad(fs, k) && fs->nbad < ALFS_BAD_MAX)
        fs->bad[fs->nbad++] = (uint16_t)k;
}

/*
 * SPEC 1: every byte reads erased; never page 0, nor a bad sector's. To take
 * a page, the bytes an allocation programs must read erased on LOOKS reads:
 * a header a power loss cut early can read erased but for a bit that reads
 * either way (D-2's rule for a page that looks erased).
 */
static int is_free(alfs_fs_t *fs, uint32_t p, bool *free, uint32_t looks) {
    *free = false;
    if (!p || is_bad(fs, p / pps(fs)))
        return ALFS_OK;
    for (uint32_t i = 0; i < looks; i++) {
        if (alfs_blank(&fs->d, paddr(fs, p), HDR + FB, free))
            return ALFS_EIO;
        if (!*free)
            return ALFS_OK;
    }
    return alfs_blank(&fs->d, paddr(fs, p) + HDR + FB, psize(fs) - HDR - FB, free);
}

/* SPEC 2: from a page's header h, the next page of frame id after position
 * pos, its header in *nh; NONE where the list ends. A link to a page that is
 * not the next is counted. */
static int follow(alfs_fs_t *fs, const hdr_t *h, uint32_t id, uint32_t pos, uint32_t *next, hdr_t *nh) {
    *next = NONE;
    if (h->next == NONE || !h->next)
        return ALFS_OK;
    if (h->next >= fs->pages) {
        fs->d.n.check_failures++;
        return ALFS_OK;
    }
    if (read_hdr(fs, h->next, nh))
        return ALFS_EIO;
    if (nh->valid && nh->id == id && nh->pos == pos + 1u)
        *next = h->next;
    else
        fs->d.n.check_failures++;
    return ALFS_OK;
}

static int next_page(alfs_fs_t *fs, uint32_t p, uint32_t id, uint32_t pos, uint32_t *next) {
    hdr_t h, n;
    *next = NONE;
    return read_hdr(fs, p, &h) ? ALFS_EIO : follow(fs, &h, id, pos, next, &n);
}

/* ---- the cache ------------------------------------------------------ */

static alfs_rec_t *rec_of(alfs_fs_t *fs, uint32_t id) {
    for (uint32_t i = 0; i < fs->ncache; i++)
        if (fs->cache[i].id == id)
            return &fs->cache[i];
    return NULL;
}

static alfs_rec_t *free_rec(alfs_fs_t *fs) {
    return rec_of(fs, 0);
}

/* 1 when record r is a copy of filename name, 0 when not, or ALFS_EIO. */
static int named(alfs_fs_t *fs, const alfs_rec_t *r, const uint8_t *name, uint32_t n, uint16_t hash) {
    uint8_t nm[NAME_LEN];
    if (!r->id || r->hash != hash)
        return 0;
    if (alfs_read(&fs->d, paddr(fs, r->first) + HDR + FB_NAME, nm, NAME_LEN))
        return ALFS_EIO;
    return !memcmp(nm, name, n) && !nm[n];
}

/* The copies of name that are not stale: the newest closed, *c, and a newer
 * one not closed, *o; NULL for none. */
static int pick(alfs_fs_t *fs, const uint8_t *name, uint32_t n, alfs_rec_t **c, alfs_rec_t **o) {
    uint16_t hash = name_hash(name, n);
    *c = *o = NULL;
    for (uint32_t i = 0; i < fs->ncache; i++) {
        alfs_rec_t *r = &fs->cache[i];
        int rc = r->flags & F_STALE ? 0 : named(fs, r, name, n, hash);
        if (rc < 0)
            return rc;
        alfs_rec_t **best = r->flags & F_CLOSED ? c : o;
        if (rc && (!*best || newer(r->version, (*best)->version)))
            *best = r;
    }
    if (*c && *o && !newer((*o)->version, (*c)->version))
        *o = NULL;
    return ALFS_OK;
}

/* SPEC 6: 00h over the copy's filename; its record goes. */
static int block_out(alfs_fs_t *fs, alfs_rec_t *r) {
    uint8_t z[NAME_LEN];
    memset(z, 0, sizeof(z));
    int rc = alfs_prog(&fs->d, paddr(fs, r->first) + HDR + FB_NAME, z, NAME_LEN);
    if (!rc)
        r->id = 0;
    return rc;
}

/* Blocks out every copy of name but keep, stale or not; with only_stale,
 * the stale ones and those not closed. */
static int block_others(alfs_fs_t *fs, const uint8_t *name, uint32_t n, const alfs_rec_t *keep, bool only_stale) {
    uint16_t hash = name_hash(name, n);
    for (uint32_t i = 0; i < fs->ncache; i++) {
        alfs_rec_t *r = &fs->cache[i];
        if (r == keep || (only_stale && (r->flags & F_CLOSED) && !(r->flags & F_STALE)))
            continue;
        int rc = named(fs, r, name, n, hash);
        if (rc > 0)
            rc = block_out(fs, r);
        if (rc < 0)
            return rc;
    }
    return ALFS_OK;
}

/* A copy the mount found: kept, or recorded stale behind a newer one. */
static int offer(alfs_fs_t *fs, uint32_t p, const uint8_t *fb) {
    uint32_t n = fb_name_len(fb + FB_NAME);
    bool closed = alfs_get32(fb + FB_CHECKED) == ~alfs_crc(ALFS_CRC_INIT, fb, FB_CHECKED);
    uint8_t v = fb[FB_VER];
    alfs_rec_t *c, *o, *r = free_rec(fs);
    int rc = pick(fs, fb + FB_NAME, n, &c, &o);
    if (rc)
        return rc;
    if (!r)
        return ALFS_ENOSPC;
    bool kept = (!c || newer(v, c->version)) && (closed || !o || newer(v, o->version));
    if (kept && closed && c)
        c->flags |= F_STALE;
    if (kept && o && (!closed || !newer(o->version, v)))
        o->flags |= F_STALE;
    r->id = alfs_get32(fb);
    r->first = (uint16_t)p;
    r->pages = closed ? (uint16_t)alfs_get16(fb + FB_CLOSE) : 0u;
    r->hash = name_hash(fb + FB_NAME, n);
    r->version = v;
    r->flags = (uint8_t)((closed ? F_CLOSED : 0) | F_MOUNTED | (kept ? 0 : F_STALE));
    return ALFS_OK;
}

/* SPEC 2: a page in the list of a copy the cache holds. */
static bool is_live(alfs_fs_t *fs, const hdr_t *h) {
    const alfs_rec_t *r = h->valid ? rec_of(fs, h->id) : NULL;
    if (!r || (r->flags & F_STALE))
        return false;
    return !(r->flags & F_CLOSED) || h->pos < (r->pages ? r->pages : 1u);
}

/* ---- mounting ------------------------------------------------------- */

/* SPEC 3: every page's header, and each copy's frame block. */
static int scan(alfs_fs_t *fs) {
    uint32_t max = 0, torn = 0;
    for (uint32_t p = 1; p < fs->pages; p++) {
        hdr_t h;
        uint8_t fb[FB];
        if (read_hdr(fs, p, &h))
            return ALFS_EIO;
        if (!h.valid) {
            torn += !h.erased;
            continue;
        }
        if (h.id > max)
            max = h.id;
        if (h.pos)
            continue;
        if (alfs_read(&fs->d, paddr(fs, p) + HDR, fb, FB))
            return ALFS_EIO;
        if (!is_copy(fb, h.id))
            continue;
        int rc = offer(fs, p, fb);
        if (rc)
            return rc;
    }
    /* past every frame ID a torn header may hold (IF-14) */
    fs->next_id = max + 2u + torn;
    fs->search = 1;
    return ALFS_OK;
}

int alfs_fs_mount(alfs_fs_t *fs, const alfs_area_t *cfg, const alfs_driver_t *drv, uint8_t *scratch,
                  alfs_rec_t *cache, uint16_t n) {
    if (!fs)
        return ALFS_EINVAL;
    if (fs->d.cfg && (fs->writers || fs->readers))
        return ALFS_EOPEN;
    memset(fs, 0, sizeof(*fs));
    int rc = alfs_dev_init(&fs->d, cfg, drv, scratch);
    if (!rc && (!cache || !n))
        rc = ALFS_EINVAL;
    if (!rc && (fs->d.sectors < 2u || cfg->size / cfg->page_size > 65535u))
        rc = ALFS_EGEOMETRY;
    if (!rc) {
        fs->cache = cache;
        fs->ncache = n;
        fs->pages = cfg->size / cfg->page_size;
        memset(cache, 0, n * sizeof(*cache));
        rc = scan(fs);
    }
    if (rc)
        fs->d.cfg = NULL;
    return rc;
}

static bool mounted(const alfs_fs_t *fs) {
    return fs && fs->d.cfg;
}

/* ---- allocating ----------------------------------------------------- */

static uint32_t pages_for(const alfs_fs_t *fs, uint32_t bytes) {
    uint32_t d0 = dcap(fs, 0), d = dcap(fs, 1);
    return bytes <= d0 ? 1u : 1u + (bytes - d0 + d - 1u) / d;
}

static uint32_t step(const alfs_fs_t *fs, uint32_t p, bool back) {
    if (back)
        return p > 1u ? p - 1u : fs->pages - 1u;
    return p + 1u < fs->pages ? p + 1u : 1u;
}

/* SPEC 4: the r-th free page forward from the search, in *last. */
static int find_free(alfs_fs_t *fs, uint32_t r, uint32_t *last) {
    uint32_t p = fs->search, got = 0;
    for (uint32_t i = 1; i < fs->pages; i++, p = step(fs, p, false)) {
        bool fr;
        if (is_free(fs, p, &fr, LOOKS))
            return ALFS_EIO;
        if (fr && ++got == r) {
            *last = p;
            return ALFS_OK;
        }
    }
    return ALFS_ENOSPC;
}

/*
 * SPEC 4: the free pages from the search to last, from the last to the
 * first: each one's header, linked to the one after it; the first's frame
 * block and link before its header. Its address in *first. A program that
 * fails makes its sector bad: ALFS_EPROG.
 */
static ALFS_NOINLINE int link_pages(alfs_fs_t *fs, uint32_t last, uint32_t r, uint32_t id, const uint8_t *name,
                                    uint32_t n, uint8_t v, uint32_t *first) {
    uint32_t p = last, next = NONE, pos = r;
    uint8_t *b = fs->d.scratch;
    for (uint32_t i = 1; pos && i < fs->pages; i++, p = step(fs, p, true)) {
        bool fr;
        if (is_free(fs, p, &fr, LOOKS))
            return ALFS_EIO;
        if (!fr)
            continue;
        pos--;
        alfs_put32(b, id);
        alfs_put16(b + O_POS, pos);
        alfs_put32(b + O_NEXT, next);
        alfs_put32(b + HDR, id);
        memset(b + HDR + FB_NAME, 0, NAME_LEN);
        memcpy(b + HDR + FB_NAME, name, n);
        b[HDR + FB_VER] = v;
        alfs_put32(b + HDR + FB_OPEN, ~alfs_crc(ALFS_CRC_INIT, b + HDR, FB_OPEN));
        alfs_put32(b + O_HCHK, hdr_check(b));
        /* the last page's link stays erased, for growth: no program covers it */
        uint32_t at = next == NONE ? HDR : O_NEXT;
        int rc;
        if (pos) {
            rc = alfs_prog(&fs->d, paddr(fs, p), b, next == NONE ? O_NEXT : HDR);
        } else { /* the first page's header last: once valid, the copy is whole */
            rc = alfs_prog(&fs->d, paddr(fs, p) + at, b + at, HDR + FB_CLOSE - at);
            if (!rc)
                rc = alfs_prog(&fs->d, paddr(fs, p), b, O_NEXT);
        }
        if (rc) {
            mark_bad(fs, p / pps(fs));
            return ALFS_EPROG;
        }
        next = p;
    }
    *first = next;
    return pos ? ALFS_ENOSPC : ALFS_OK;
}

int alfs_file_alloc(alfs_fs_t *fs, const char *name, uint32_t bytes, uint32_t *id) {
    uint32_t n = name_len(name);
    const uint8_t *nm = (const uint8_t *)name;
    if (!mounted(fs) || !n || !id)
        return ALFS_EINVAL;
    alfs_rec_t *c, *o;
    int rc = pick(fs, nm, n, &c, &o);
    if (rc)
        return rc;
    if (o && BUSY(o))
        return ALFS_EOPEN;
    uint32_t r = pages_for(fs, bytes), last = 0;
    if ((rc = find_free(fs, r, &last)) != 0)
        return rc;
    uint8_t v = (uint8_t)((c ? c->version : 0u) + 1u);
    if (o && newer(o->version, (uint8_t)(v - 1u)))
        v = (uint8_t)(o->version + 1u);
    /* the copy not closed and the stale ones give way, and their records */
    if ((rc = block_others(fs, nm, n, c, true)) != 0)
        return rc;
    alfs_rec_t *rec = free_rec(fs);
    if (!rec)
        return ALFS_ENOSPC;
    uint32_t first = 0, fid = 0;
    for (uint32_t t = 0; t < TRIES; t++) {
        fid = fs->next_id++;
        rc = link_pages(fs, last, r, fid, nm, n, v, &first);
        if (rc != ALFS_EPROG)
            break;
        if ((rc = find_free(fs, r, &last)) != 0)
            return rc;
        rc = ALFS_EPROG;
    }
    if (rc)
        return rc;
    fs->search = step(fs, last, false);
    rec->id = fid;
    rec->first = (uint16_t)first;
    rec->pages = 0;
    rec->hash = name_hash(nm, n);
    rec->version = v;
    rec->flags = 0;
    *id = fid;
    return ALFS_OK;
}

/* ---- writing -------------------------------------------------------- */

static alfs_rec_t *writing(alfs_fs_t *fs, const alfs_file_t *f) {
    alfs_rec_t *r = mounted(fs) && f && f->open ? rec_of(fs, f->id) : NULL;
    return r && (r->flags & F_WRITER) ? r : NULL;
}

/* 2 bytes at a page's byte count. */
static int read_count(alfs_fs_t *fs, uint32_t p, uint32_t pos, uint32_t *cnt) {
    uint8_t b[2];
    if (alfs_read(&fs->d, paddr(fs, p) + data_at(pos), b, 2))
        return ALFS_EIO;
    *cnt = alfs_get16(b);
    return ALFS_OK;
}

/*
 * SPEC 4: where writing goes on: the page after the last one holding data,
 * past one more skipped after a mount; the size so far; and the bytes the
 * list's pages from there hold. A page whose program failed reads erased
 * before the last one holding data.
 */
static ALFS_NOINLINE int find_end(alfs_fs_t *fs, alfs_file_t *f, alfs_rec_t *r) {
    uint32_t p = r->first, pos = 0, cnt, end = 0, n = 0;
    uint32_t at = NONE, prev = NONE;
    hdr_t h;
    f->size = f->left = 0;
    if (read_hdr(fs, p, &h))
        return ALFS_EIO;
    /* the list's length, the end of its data, and the data's size */
    for (uint32_t i = 0; i < fs->pages; i++) {
        uint32_t next;
        hdr_t nh;
        if (read_count(fs, p, pos, &cnt) || follow(fs, &h, r->id, pos, &next, &nh))
            return ALFS_EIO;
        if (cnt != COUNT_NONE) {
            end = pos + 1u;
            f->size += cnt <= dcap(fs, pos) ? cnt : 0u;
        }
        n = pos + 1u;
        if (next == NONE)
            break;
        h = nh;
        p = next;
        pos++;
    }
    bool skip = end < n && (r->flags & F_MOUNTED);
    end += skip; /* SAFE-04: that page is skipped */
    /* again, to the page `end` */
    p = r->first;
    if (read_hdr(fs, p, &h))
        return ALFS_EIO;
    for (pos = 0; pos < n; pos++) {
        uint32_t next;
        hdr_t nh;
        if (skip && pos + 1u == end) {
            uint8_t z[2] = {0, 0};
            alfs_prog(&fs->d, paddr(fs, p) + data_at(pos), z, 2);
        }
        if (pos == end)
            at = p;
        if (pos >= end)
            f->left += dcap(fs, pos);
        else
            prev = p;
        if (follow(fs, &h, r->id, pos, &next, &nh))
            return ALFS_EIO;
        if (next == NONE)
            break;
        h = nh;
        p = next;
    }
    f->page = at;
    f->count = (uint16_t)end;
    f->first = r->first;
    f->prev = prev;
    r->flags &= (uint8_t)~F_MOUNTED;
    return ALFS_OK;
}

int alfs_file_open(alfs_fs_t *fs, alfs_file_t *f, const char *name, unsigned flags, uint8_t *block,
                   uint32_t size) {
    uint32_t n = name_len(name);
    if (!mounted(fs) || !f || !n || !block || size < psize(fs) || size > 65535u)
        return ALFS_EINVAL;
    alfs_rec_t *c, *o;
    int rc = pick(fs, (const uint8_t *)name, n, &c, &o);
    if (rc)
        return rc;
    if (!o)
        return c ? ALFS_EEXIST : ALFS_ENOENT;
    if (BUSY(o))
        return ALFS_EOPEN;
    memset(f, 0, sizeof(*f));
    if ((rc = find_end(fs, f, o)) != 0)
        return rc;
    f->block = block;
    f->cap = (uint16_t)size;
    f->id = o->id;
    f->flags = (uint8_t)flags;
    f->open = 1;
    o->flags |= F_WRITER;
    fs->writers++;
    return ALFS_OK;
}

/* SPEC 4: past the reservation, a free page linked after the last. */
static int grow(alfs_fs_t *fs, alfs_file_t *f) {
    hdr_t h;
    uint32_t q = 0;
    uint8_t *b = fs->d.scratch;
    if (!(f->flags & ALFS_GROW) || f->prev == NONE)
        return ALFS_ENOSPC;
    if (read_hdr(fs, f->prev, &h))
        return ALFS_EIO;
    if (h.next != NONE)
        return ALFS_ENOSPC; /* the list ended at a link to another page */
    for (uint32_t t = 0; t < TRIES; t++) {
        int rc = find_free(fs, 1, &q);
        if (rc)
            return rc;
        alfs_put32(b, f->id);
        alfs_put16(b + O_POS, f->count);
        alfs_put32(b + O_HCHK, hdr_check(b));
        if (!alfs_prog(&fs->d, paddr(fs, q), b, O_NEXT)) /* its link stays erased */
            break;
        mark_bad(fs, q / pps(fs));
        q = 0;
    }
    if (!q)
        return ALFS_EPROG;
    fs->search = step(fs, q, false);
    alfs_put32(b, q);
    if (alfs_prog(&fs->d, paddr(fs, f->prev) + O_NEXT, b, 4))
        return ALFS_EPROG;
    f->page = q;
    return ALFS_OK;
}

/* One page's data: its byte count, the data, the check, in one program. A
 * program that fails loses its bytes, counted (SAFE-06). */
static void put_page(alfs_fs_t *fs, alfs_file_t *f, uint32_t n) {
    uint8_t *b = fs->d.scratch;
    alfs_put16(b, n);
    memcpy(b + 2, f->block, n);
    alfs_put32(b + 2 + n, page_check(f->id, f->count, f->block, n));
    if (alfs_prog(&fs->d, paddr(fs, f->page) + data_at(f->count), b, 2u + n + CHK))
        fs->d.n.bytes_lost += n;
}

/* Programs the block's complete pages; with all, the rest as a partial
 * page too. */
static int spill(alfs_fs_t *fs, alfs_file_t *f, bool all) {
    for (uint32_t i = 0; i <= fs->pages; i++) {
        uint32_t cap = dcap(fs, f->count), n = f->used < cap ? f->used : cap;
        if (!n || (n < cap && !all))
            return ALFS_OK;
        if (f->page == NONE) {
            int rc = grow(fs, f);
            if (rc)
                return rc;
        }
        uint32_t next;
        if (next_page(fs, f->page, f->id, f->count, &next))
            return ALFS_EIO;
        put_page(fs, f, n);
        memmove(f->block, f->block + n, f->used - n);
        f->used = (uint16_t)(f->used - n);
        f->left = f->left > cap ? f->left - cap : 0u;
        f->prev = f->page;
        f->page = next;
        f->count++;
    }
    return ALFS_OK;
}

int32_t alfs_file_write(alfs_fs_t *fs, alfs_file_t *f, const void *data, uint32_t len) {
    if (!writing(fs, f) || (len && !data))
        return ALFS_EINVAL;
    uint32_t taken = 0;
    while (taken < len) {
        if (f->used == f->cap) {
            int rc = spill(fs, f, false);
            if (rc)
                return taken ? (int32_t)taken : rc;
        }
        uint32_t k = len - taken, room = (uint32_t)(f->cap - f->used);
        if (k > room)
            k = room;
        if (!(f->flags & ALFS_GROW)) {
            room = f->left > f->used ? f->left - f->used : 0u;
            if (k > room)
                k = room;
        }
        if (!k)
            return taken ? (int32_t)taken : ALFS_ENOSPC;
        memcpy(f->block + f->used, (const uint8_t *)data + taken, k);
        f->used = (uint16_t)(f->used + k);
        f->size += k;
        taken += k;
    }
    if (f->used == f->cap)
        spill(fs, f, false); /* the bytes are taken: a page that cannot be had waits */
    return (int32_t)taken;
}

int alfs_file_sync(alfs_fs_t *fs, alfs_file_t *f) {
    return writing(fs, f) ? spill(fs, f, false) : ALFS_EINVAL;
}

int alfs_file_flush(alfs_fs_t *fs, alfs_file_t *f) {
    return writing(fs, f) ? spill(fs, f, true) : ALFS_EINVAL;
}

int alfs_file_close(alfs_fs_t *fs, alfs_file_t *f) {
    alfs_rec_t *r = writing(fs, f);
    if (!r)
        return ALFS_EINVAL;
    int rc = spill(fs, f, true);
    if (rc)
        return rc;
    uint8_t *b = fs->d.scratch, z[4] = {0, 0, 0, 0};
    uint32_t pages = f->count ? f->count : 1u;
    if (alfs_read(&fs->d, paddr(fs, f->first) + HDR, b, FB_CLOSE))
        return ALFS_EIO;
    alfs_put16(b + FB_CLOSE, pages);
    alfs_put32(b + FB_CLOSE + 2, f->size);
    alfs_put32(b + FB_CHECKED, ~alfs_crc(ALFS_CRC_INIT, b, FB_CHECKED));
    alfs_prog(&fs->d, paddr(fs, f->first) + HDR + FB_CLOSE, b + FB_CLOSE, FB - FB_CLOSE);
    alfs_prog(&fs->d, paddr(fs, f->prev != NONE ? f->prev : f->first) + O_NEXT, z, 4);
    /* the older copy gives way (SAFE-16) */
    uint8_t nm[NAME_LEN];
    if (!alfs_read(&fs->d, paddr(fs, f->first) + HDR + FB_NAME, nm, NAME_LEN))
        block_others(fs, nm, fb_name_len(nm), r, false);
    r->flags = (uint8_t)((r->flags & ~(F_WRITER | F_MOUNTED)) | F_CLOSED);
    r->pages = (uint16_t)pages;
    f->open = 0;
    fs->writers--;
    return ALFS_OK;
}

/* ---- reading -------------------------------------------------------- */

int alfs_file_open_read(alfs_fs_t *fs, alfs_reader_t *r, const char *name, uint8_t *block, uint32_t size) {
    uint32_t n = name_len(name);
    if (!mounted(fs) || !r || !n || !block || size < psize(fs))
        return ALFS_EINVAL;
    alfs_rec_t *c, *o;
    int rc = pick(fs, (const uint8_t *)name, n, &c, &o);
    if (rc)
        return rc;
    alfs_rec_t *rec = c ? c : o;
    if (!rec)
        return ALFS_ENOENT;
    if (READERS(rec) == 15u)
        return ALFS_EOPEN;
    uint8_t b[4];
    memset(r, 0, sizeof(r[0]));
    r->size = ALFS_SIZE_OPEN;
    if (rec->flags & F_CLOSED) {
        if (alfs_read(&fs->d, paddr(fs, rec->first) + HDR + FB_CLOSE + 2, b, 4))
            return ALFS_EIO;
        r->size = alfs_get32(b);
        r->closed = 1;
    }
    r->block = block;
    r->id = rec->id;
    r->page = rec->first;
    r->pages = rec->pages;
    r->open = 1;
    rec->flags = (uint8_t)(rec->flags + F_READER);
    fs->readers++;
    return ALFS_OK;
}

/* SPEC 5: an erased byte count in a copy not closed ends it, unless a page
 * after it, in its sector or the first past it, holds data: a page whose
 * program failed reads erased. */
static int data_ahead(alfs_fs_t *fs, const alfs_reader_t *r, uint32_t p, bool *more) {
    uint32_t pos = r->count + 1u, k = r->page / pps(fs), cnt;
    *more = false;
    for (uint32_t i = 0; p != NONE && i <= pps(fs); i++) {
        uint32_t next;
        if (read_count(fs, p, pos, &cnt))
            return ALFS_EIO;
        if (cnt != COUNT_NONE) {
            *more = true;
            return ALFS_OK;
        }
        if (p / pps(fs) != k)
            return ALFS_OK;
        if (next_page(fs, p, r->id, pos, &next))
            return ALFS_EIO;
        p = next;
        pos++;
    }
    return ALFS_OK;
}

/* SPEC 5: the next page's data into the block, and the page after it. A
 * failed read changes nothing (SAFE-08). */
static int load(alfs_fs_t *fs, alfs_reader_t *r) {
    uint32_t off = data_at(r->count), next, cnt;
    if (alfs_read(&fs->d, paddr(fs, r->page) + off, r->block, psize(fs) - off))
        return ALFS_EIO;
    if (next_page(fs, r->page, r->id, r->count, &next))
        return ALFS_EIO;
    cnt = alfs_get16(r->block);
    if (cnt == COUNT_NONE && !r->closed) {
        bool more;
        if (data_ahead(fs, r, next, &more))
            return ALFS_EIO;
        if (!more) {
            r->page = NONE; /* the end of what is written */
            return ALFS_OK;
        }
    }
    r->pos = 2;
    r->used = 0;
    if (cnt == COUNT_NONE && r->closed && r->size) { /* a page the writer lost */
        fs->d.n.check_failures++;
        r->failed = 1;
    }
    if (cnt && cnt != COUNT_NONE) {
        if (cnt <= dcap(fs, r->count) && alfs_get32(r->block + 2 + cnt) == page_check(r->id, r->count, r->block + 2, cnt)) {
            r->used = (uint16_t)cnt;
        } else {
            fs->d.n.check_failures++;
            r->failed = 1;
        }
    }
    r->page = next;
    r->count++;
    return ALFS_OK;
}

int32_t alfs_file_read(alfs_fs_t *fs, alfs_reader_t *r, void *out, uint32_t len) {
    if (!mounted(fs) || !r || !r->open || (len && !out))
        return ALFS_EINVAL;
    for (uint32_t i = 0; i <= fs->pages + 1u; i++) {
        if (r->used || !len) {
            uint32_t k = r->used < len ? r->used : len;
            if (k)
                memcpy(out, r->block + r->pos, k);
            r->pos = (uint16_t)(r->pos + k);
            r->used = (uint16_t)(r->used - k);
            return (int32_t)k;
        }
        if (r->page == NONE || (r->closed && r->count >= r->pages))
            return r->closed && (r->failed || r->count < r->pages) ? ALFS_ECHECK : 0;
        int rc = load(fs, r);
        if (rc)
            return rc;
    }
    return 0;
}

int alfs_file_close_read(alfs_fs_t *fs, alfs_reader_t *r) {
    if (!mounted(fs) || !r || !r->open)
        return ALFS_EINVAL;
    r->open = 0;
    fs->readers--;
    alfs_rec_t *rec = rec_of(fs, r->id);
    if (rec && READERS(rec))
        rec->flags = (uint8_t)(rec->flags - F_READER);
    return ALFS_OK;
}

/* ---- deleting and listing ------------------------------------------- */

int alfs_file_delete(alfs_fs_t *fs, const char *name) {
    uint32_t n = name_len(name);
    if (!mounted(fs) || !n)
        return ALFS_EINVAL;
    alfs_rec_t *c, *o;
    int rc = pick(fs, (const uint8_t *)name, n, &c, &o);
    if (rc)
        return rc;
    if (!c && !o)
        return ALFS_ENOENT;
    if ((c && BUSY(c)) || (o && BUSY(o)))
        return ALFS_EOPEN;
    return block_others(fs, (const uint8_t *)name, n, NULL, false);
}

int alfs_file_list(alfs_fs_t *fs, uint32_t after, alfs_file_info_t *info) {
    if (!mounted(fs) || !info)
        return ALFS_EINVAL;
    for (uint32_t guard = 0; guard < fs->ncache; guard++) {
        alfs_rec_t *best = NULL;
        for (uint32_t i = 0; i < fs->ncache; i++) {
            alfs_rec_t *r = &fs->cache[i];
            if (r->id > after && !(r->flags & F_STALE) && (!best || r->id < best->id))
                best = r;
        }
        if (!best)
            return ALFS_ENOENT;
        uint8_t fb[FB];
        if (alfs_read(&fs->d, paddr(fs, best->first) + HDR, fb, FB))
            return ALFS_EIO;
        alfs_rec_t *c, *o;
        uint32_t n = fb_name_len(fb + FB_NAME);
        int rc = pick(fs, fb + FB_NAME, n, &c, &o);
        if (rc)
            return rc;
        if (best != (c ? c : o)) { /* a copy the filename does not name */
            after = best->id;
            continue;
        }
        memset(info, 0, sizeof(*info));
        info->id = best->id;
        memcpy(info->name, fb + FB_NAME, n);
        info->version = best->version;
        info->closed = best->flags & F_CLOSED;
        info->size = info->closed ? alfs_get32(fb + FB_CLOSE + 2) : ALFS_SIZE_OPEN;
        info->pages = info->closed ? best->pages : 0u;
        return ALFS_OK;
    }
    return ALFS_ENOENT;
}

/* ---- cleaning up ---------------------------------------------------- */

/* SPEC 7: the next sector holding no live page that does not read erased,
 * erased and read back. */
static ALFS_NOINLINE int clean_step(alfs_fs_t *fs) {
    uint32_t sectors = fs->d.sectors, per = pps(fs);
    for (uint32_t i = 0; i < sectors; i++) {
        uint32_t k = (fs->sweep + i) % sectors;
        bool live = false, erased = true;
        if (is_bad(fs, k))
            continue;
        for (uint32_t j = 0; j < per && !live; j++) {
            hdr_t h;
            if (read_hdr(fs, k * per + j, &h))
                return ALFS_EIO;
            erased &= h.erased;
            live = k * per + j && is_live(fs, &h);
        }
        if (live)
            continue;
        if (erased) {
            bool blank;
            if (alfs_blank(&fs->d, alfs_addr(&fs->d, k), fs->d.cfg->sector_size, &blank))
                return ALFS_EIO;
            if (blank)
                continue;
        }
        fs->sweep = (k + 1u) % sectors;
        int rc = alfs_wipe(&fs->d, k);
        if (rc == ALFS_EIO)
            return rc;
        if (rc)
            mark_bad(fs, k);
        return ALFS_OK;
    }
    return ALFS_CLEAN;
}

int alfs_fs_cleanup(alfs_fs_t *fs, uint32_t steps) {
    if (!mounted(fs))
        return ALFS_EINVAL;
    if (fs->readers)
        return ALFS_EOPEN;
    for (uint32_t i = 0; i < fs->ncache; i++) /* stale copies give way first */
        if (fs->cache[i].id && (fs->cache[i].flags & F_STALE) && block_out(fs, &fs->cache[i]) == ALFS_EIO)
            return ALFS_EIO;
    for (uint32_t i = 0; i < steps; i++) {
        int rc = clean_step(fs);
        if (rc)
            return rc;
    }
    return ALFS_OK;
}

int alfs_fs_status(alfs_fs_t *fs, alfs_fs_status_t *st) {
    if (!st)
        return ALFS_EINVAL;
    memset(st, 0, sizeof(*st));
    if (!mounted(fs))
        return ALFS_EINVAL;
    st->pages = fs->pages - 1u;
    st->records_max = fs->ncache;
    for (uint32_t i = 0; i < fs->ncache; i++)
        st->records = (uint16_t)(st->records + (fs->cache[i].id != 0));
    st->data_per_page = (uint16_t)dcap(fs, 1);
    st->bad = fs->nbad;
    for (uint32_t p = 1; p < fs->pages; p++) {
        hdr_t h;
        bool blank = false;
        if (read_hdr(fs, p, &h) || (h.erased && is_free(fs, p, &blank, 1)))
            return ALFS_EIO;
        if (blank)
            st->free++;
        else if (is_live(fs, &h))
            st->live++;
        else
            st->dead++;
    }
    st->clean = !st->dead;
    st->n = fs->d.n;
    return ALFS_OK;
}
