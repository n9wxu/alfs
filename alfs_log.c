/*
 * alfs_log: the log area. See alfs_log.h; the format is docs/SPEC.md 1 to 5.
 *
 * SPDX-License-Identifier: MIT
 */
#include "alfs_log.h"

#include "alfs_flash.h"

#include <string.h>

#define STAMP_LEN 18u
#define CLAIM_LEN 12u
#define OFF_CLAIM STAMP_LEN
#define OFF_DATA (OFF_CLAIM + CLAIM_LEN)
#define HDR_LEN 7u /* a chunk's: length, tag, CRC */
#define NONE ALFS_NONE

enum { S_READY, S_USED, S_DIRTY };

typedef struct {
    uint8_t stamp[STAMP_LEN];
    uint8_t claim[CLAIM_LEN];
} hdr_t;

/* Headers are read into the scratch page: nothing builds a program there
 * while a scan runs, and the stamp lands where a claim program wants it. */
#define HDR(lg) ((hdr_t *)(void *)(lg)->d.scratch)

static uint32_t page(const alfs_log_t *lg) {
    return lg->d.cfg->page_size;
}

static uint32_t sector_size(const alfs_log_t *lg) {
    return lg->d.cfg->sector_size;
}

static uint32_t next_page(const alfs_log_t *lg, uint32_t x) {
    return (x / page(lg) + 1u) * page(lg);
}

/* ---- sector headers --------------------------------------------------- */

static int read_hdr(alfs_log_t *lg, uint32_t k, hdr_t *h) {
    return alfs_read(&lg->d, alfs_addr(&lg->d, k), h, sizeof(*h));
}

/* A stamp of this geometry with the floor given (SPEC 1.1). */
static void stamp(const alfs_log_t *lg, uint32_t floor, uint8_t *st) {
    memcpy(st, "alfl", 4);
    st[4] = ALFS_VERSION;
    st[5] = (uint8_t)alfs_lg2(sector_size(lg));
    st[6] = (uint8_t)alfs_lg2(page(lg));
    st[7] = 0;
    alfs_put16(st + 8, lg->d.sectors);
    alfs_put32(st + 10, floor);
    alfs_put32(st + 14, ~alfs_crc(ALFS_CRC_INIT, st, 14));
}

/* Valid and matching: it is the stamp this geometry writes, with its floor. */
static bool stamp_matches(const alfs_log_t *lg, const uint8_t *st) {
    uint8_t want[STAMP_LEN];
    stamp(lg, alfs_get32(st + 10), want);
    return !memcmp(st, want, STAMP_LEN);
}

static bool claim_valid(const uint8_t *cl) {
    return !alfs_is_erased(cl, CLAIM_LEN) && alfs_get32(cl + 8) == ~alfs_crc(ALFS_CRC_INIT, cl, 8);
}

/* A claim alfs programmed zero over (SAFE-04 rule 3): no torn claim reads
 * so, since it reads erased wherever its own value is (A-9). */
static bool zeroed(const uint8_t *cl) {
    for (uint32_t i = 0; i < CLAIM_LEN; i++)
        if (cl[i])
            return false;
    return true;
}

/* SPEC section 2. */
static int state_of(const alfs_log_t *lg, const hdr_t *h) {
    if (!stamp_matches(lg, h->stamp))
        return S_DIRTY;
    if (alfs_is_erased(h->claim, CLAIM_LEN))
        return S_READY;
    return claim_valid(h->claim) ? S_USED : S_DIRTY;
}

static bool is_bad(const alfs_log_t *lg, uint32_t k) {
    for (uint32_t i = 0; i < lg->nbad; i++)
        if (lg->bad[i] == k)
            return true;
    return false;
}

static void mark_bad(alfs_log_t *lg, uint32_t k) {
    if (!is_bad(lg, k) && lg->nbad < ALFS_BAD_MAX)
        lg->bad[lg->nbad++] = (uint16_t)k;
}

/* Zero over sector k's claim, which makes it dirty for good (A-9). */
static void zero_claim(alfs_log_t *lg, uint32_t k) {
    uint8_t z[CLAIM_LEN];
    memset(z, 0, sizeof(z));
    alfs_prog(&lg->d, alfs_addr(&lg->d, k) + OFF_CLAIM, z, CLAIM_LEN);
}

/* ---- mount ------------------------------------------------------------ */

/*
 * One pass over the area (SPEC 1.1, 2 and 4): the counts, the next
 * sequence, where allocation goes on, and the fresh sector.
 */
static int survey(alfs_log_t *lg) {
    bool newest_any = false, torn = false;
    uint32_t seq_max = 0, newest = 0, last_k = 0;
    uint32_t fresh_after = NONE, fresh_any = NONE; /* SAFE-04 rule 3 */
    for (uint32_t k = 0; k < lg->d.sectors; k++) {
        hdr_t *h = HDR(lg);
        if (read_hdr(lg, k, h))
            return ALFS_EIO;
        bool match = stamp_matches(lg, h->stamp), valid = claim_valid(h->claim);
        uint32_t seq = alfs_get32(h->claim + 4);
        if (valid && seq > seq_max)
            seq_max = seq;
        if (match && alfs_get32(h->stamp + 10) > seq_max)
            seq_max = alfs_get32(h->stamp + 10);
        if (match && valid && (!newest_any || seq > newest)) {
            newest = seq;
            last_k = k;
            newest_any = true;
            fresh_after = NONE;
        }
        /* a sector a claim program could have been taking: erased or torn,
         * however its bits read now, and not one alfs zeroed */
        if (match && !valid && !zeroed(h->claim)) {
            if (fresh_any == NONE)
                fresh_any = k;
            if (fresh_after == NONE && newest_any)
                fresh_after = k;
            torn |= !alfs_is_erased(h->claim, CLAIM_LEN);
        }
        int st = state_of(lg, h);
        if (st == S_READY)
            lg->ready++;
        else if (st == S_DIRTY)
            lg->dirty++;
    }
    lg->next_seq = seq_max + (torn ? 3u : 2u); /* past a torn claim's: SPEC 1.1 */
    lg->fresh = (uint16_t)(fresh_after != NONE ? fresh_after : fresh_any);
    lg->alloc_cursor = (uint16_t)(newest_any ? (last_k + 1u) % lg->d.sectors : 0u);
    lg->erase_cursor = lg->alloc_cursor;
    return ALFS_OK;
}

int alfs_log_mount(alfs_log_t *lg, const alfs_area_t *cfg, const alfs_driver_t *drv, uint8_t *scratch) {
    if (!lg)
        return ALFS_EINVAL;
    if (lg->d.cfg && (lg->writers || lg->readers))
        return ALFS_EOPEN;
    memset(lg, 0, sizeof(*lg));
    int rc = alfs_dev_init(&lg->d, cfg, drv, scratch);
    if (!rc) {
        lg->head = NONE;
        rc = survey(lg);
    }
    if (rc)
        lg->d.cfg = NULL; /* not mounted */
    return rc;
}

/* ---- erasing ---------------------------------------------------------- */

/*
 * SPEC 5: the next sector that is dirty, or log's, decided from one read
 * of its header. A log's sector whose erase fails has its claim zeroed, so
 * it cannot bring the log back (SAFE-11). ALFS_CLEAN when none is left.
 */
static int erase_step(alfs_log_t *lg, uint32_t log) {
    for (uint32_t i = 0; i < lg->d.sectors; i++) {
        uint32_t k = (lg->erase_cursor + i) % lg->d.sectors;
        hdr_t *h = HDR(lg);
        if (is_bad(lg, k))
            continue;
        if (read_hdr(lg, k, h))
            return ALFS_EIO;
        int st = state_of(lg, h);
        if (st != S_DIRTY && !(log && st == S_USED && alfs_get32(h->claim) == log))
            continue;
        lg->erase_cursor = (uint16_t)((k + 1u) % lg->d.sectors);
        int rc = alfs_wipe(&lg->d, k);
        if (rc == ALFS_EIO)
            return rc;
        if (!rc) {
            uint8_t s[STAMP_LEN];
            stamp(lg, lg->next_seq, s);
            rc = alfs_prog(&lg->d, alfs_addr(&lg->d, k), s, STAMP_LEN);
        }
        if (st == S_DIRTY && lg->dirty)
            lg->dirty--;
        if (rc) {
            if (st == S_USED)
                zero_claim(lg, k);
            mark_bad(lg, k);
        } else {
            lg->ready++;
        }
        return ALFS_OK;
    }
    if (!log)
        lg->dirty = 0;
    return ALFS_CLEAN;
}

int alfs_log_erase(alfs_log_t *lg, uint32_t log, uint32_t steps) {
    if (!lg || !lg->d.cfg)
        return ALFS_EINVAL;
    if (lg->readers || (log && log == lg->cur && lg->writers))
        return ALFS_EOPEN;
    if (log && log == lg->cur) { /* no writer may join a log being erased */
        lg->cur = 0;
        lg->head = NONE;
    }
    for (uint32_t i = 0; i < steps; i++) {
        if (!log && !lg->dirty)
            return ALFS_CLEAN;
        int rc = erase_step(lg, log);
        if (rc)
            return rc;
    }
    return ALFS_OK;
}

/* ---- writing ---------------------------------------------------------- */

/*
 * A ready sector for a claim, its first page checked erased past the stamp.
 * Its stamp is left in the scratch page, for a claim program from byte 0
 * (SAFE-04 rule 2).
 */
static int take_sector(alfs_log_t *lg, uint32_t *out) {
    for (uint32_t i = 0; lg->ready && i < lg->d.sectors; i++) {
        uint32_t k = (lg->alloc_cursor + i) % lg->d.sectors;
        hdr_t *h = HDR(lg);
        bool ok;
        if (is_bad(lg, k))
            continue;
        if (read_hdr(lg, k, h))
            return ALFS_EIO;
        if (state_of(lg, h) != S_READY)
            continue;
        if (alfs_blank(&lg->d, alfs_addr(&lg->d, k) + OFF_CLAIM, page(lg) - OFF_CLAIM, &ok))
            return ALFS_EIO;
        lg->ready--;
        if (ok && k != lg->fresh) {
            lg->alloc_cursor = (uint16_t)((k + 1u) % lg->d.sectors);
            *out = k;
            return ALFS_OK;
        }
        lg->dirty++; /* touched (SAFE-05), or the fresh sector (SAFE-04 rule 3) */
        if (ok) {
            lg->fresh = NONE;
            zero_claim(lg, k);
        }
    }
    return ALFS_ENOSPC;
}

/* The claim of a sector of log `log`, with the next sequence, at the
 * scratch page's claim. */
static void put_claim(alfs_log_t *lg, uint32_t log) {
    uint8_t *c = lg->d.scratch + OFF_CLAIM;
    alfs_put32(c, log);
    alfs_put32(c + 4, lg->next_seq++);
    alfs_put32(c + 8, ~alfs_crc(ALFS_CRC_INIT, c, 8));
}

/*
 * SPEC 4: a log's first sector, taken at once, so its number, the
 * sequence of its first claim, is on the flash before it is given.
 */
static int start_log(alfs_log_t *lg) {
    uint32_t k;
    int rc = take_sector(lg, &k);
    if (rc)
        return rc;
    uint32_t log = lg->next_seq;
    put_claim(lg, log);
    if (alfs_prog(&lg->d, alfs_addr(&lg->d, k), lg->d.scratch, OFF_DATA)) {
        lg->dirty++;
        return ALFS_EPROG;
    }
    lg->cur = log;
    lg->head = (uint16_t)k;
    lg->off = (uint16_t)OFF_DATA;
    return ALFS_OK;
}

/* Moves the next chunk's place on until a byte of payload fits there. */
static void place(alfs_log_t *lg) {
    while (lg->head != NONE) {
        uint32_t np = next_page(lg, lg->off);
        if (lg->off + HDR_LEN + 1u > sector_size(lg)) {
            lg->head = NONE;
            return;
        }
        if (np - lg->off < HDR_LEN + 1u) {
            lg->off = (uint16_t)np;
            continue;
        }
        return;
    }
}

/* The payload that fits where the next chunk goes. */
static uint32_t room(const alfs_log_t *lg) {
    if (lg->head == NONE)
        return page(lg) - OFF_DATA - HDR_LEN;
    return next_page(lg, lg->off) - lg->off - HDR_LEN;
}

/*
 * One program of up to n bytes of stream tag: in the current page, or with
 * the claim of a new sector, from its byte 0. The bytes it took, or an
 * error with nothing done. A program that fails drops its bytes (SAFE-06).
 */
static int32_t prog_chunk(alfs_log_t *lg, uint8_t tag, const uint8_t *p, uint32_t n) {
    bool fresh = lg->head == NONE;
    uint32_t k = lg->head, at = lg->off;
    if (n > room(lg))
        n = room(lg);
    if (fresh) {
        int rc = take_sector(lg, &k);
        if (rc)
            return rc;
        put_claim(lg, lg->cur);
        at = 0;
    }
    uint8_t *h = lg->d.scratch + (fresh ? OFF_DATA : 0u);
    alfs_put16(h, n);
    h[2] = tag;
    alfs_put32(h + 3, ~alfs_crc(alfs_crc(ALFS_CRC_INIT, h, 3), p, n));
    memcpy(h + HDR_LEN, p, n);
    uint32_t len = (uint32_t)(h - lg->d.scratch) + HDR_LEN + n;
    if (alfs_prog(&lg->d, alfs_addr(&lg->d, k) + at, lg->d.scratch, len)) {
        lg->d.n.bytes_lost += n;
        if (fresh)
            lg->dirty++;
        lg->head = NONE; /* a page that fails is a sector that is failing */
        return (int32_t)n;
    }
    lg->head = (uint16_t)k;
    lg->off = (uint16_t)(at + len);
    return (int32_t)n;
}

/*
 * Programs the block's bytes as chunks, each filling its page: all of them,
 * or all but what is left after the last whole page's room (IF-12).
 */
static int spill(alfs_log_t *lg, alfs_writer_t *w, bool all) {
    uint32_t done = 0;
    int rc = ALFS_OK;
    while (done < w->used) {
        place(lg);
        uint32_t left = w->used - done, n = room(lg);
        if (!all && left < n)
            break;
        int32_t m = prog_chunk(lg, w->tag, w->block + done, left < n ? left : n);
        if (m < 0) {
            rc = m;
            break;
        }
        done += (uint32_t)m;
    }
    memmove(w->block, w->block + done, w->used - done);
    w->used = (uint16_t)(w->used - done);
    return rc;
}

static bool block_ok(const alfs_log_t *lg, const uint8_t *block, uint32_t size) {
    return block && size >= page(lg) && size <= 65535u;
}

int alfs_log_open(alfs_log_t *lg, alfs_writer_t *w, uint32_t log, uint8_t tag, uint8_t *block, uint32_t size) {
    if (!lg || !lg->d.cfg || !w || !block_ok(lg, block, size))
        return ALFS_EINVAL;
    if (!log) {
        if (lg->writers)
            return ALFS_EOPEN;
        int rc = start_log(lg);
        if (rc)
            return rc;
    } else if (log != lg->cur) {
        return ALFS_EINVAL;
    }
    memset(w, 0, sizeof(*w));
    w->block = block;
    w->cap = (uint16_t)size;
    w->log = lg->cur;
    w->tag = tag;
    w->open = 1;
    lg->writers++;
    return ALFS_OK;
}

static bool writing(const alfs_log_t *lg, const alfs_writer_t *w) {
    return lg && lg->d.cfg && w && w->open && w->log == lg->cur;
}

uint32_t alfs_log_take(alfs_writer_t *w, const void *data, uint32_t len) {
    uint32_t n = w->open ? (uint32_t)(w->cap - w->used) : 0u;
    if (n > len)
        n = len;
    if (n)
        memcpy(w->block + w->used, data, n);
    w->used = (uint16_t)(w->used + n);
    return n;
}

int32_t alfs_log_write(alfs_log_t *lg, alfs_writer_t *w, const void *data, uint32_t len) {
    if (!writing(lg, w) || (len && !data))
        return ALFS_EINVAL;
    uint32_t taken = 0;
    for (;;) {
        if (w->used == w->cap) {
            int rc = spill(lg, w, false);
            if (rc)
                return taken ? (int32_t)taken : rc;
        }
        if (taken == len)
            return (int32_t)taken;
        taken += alfs_log_take(w, (const uint8_t *)data + taken, len - taken);
    }
}

int alfs_log_sync(alfs_log_t *lg, alfs_writer_t *w) {
    return writing(lg, w) ? spill(lg, w, true) : ALFS_EINVAL;
}

int alfs_log_close(alfs_log_t *lg, alfs_writer_t *w) {
    if (!writing(lg, w))
        return ALFS_EINVAL;
    int rc = spill(lg, w, true);
    if (rc)
        return rc;
    w->open = 0;
    lg->writers--;
    return ALFS_OK;
}

/* ---- reading ---------------------------------------------------------- */

/*
 * SPEC 3: log's sector with the least sequence greater than *seq, found
 * forward from *k: the first of the log's there.
 */
static int next_sector(alfs_log_t *lg, uint32_t log, uint32_t *k, uint32_t *seq) {
    for (uint32_t i = 1; i <= lg->d.sectors; i++) {
        uint32_t j = (*k + i) % lg->d.sectors;
        hdr_t *h = HDR(lg);
        if (read_hdr(lg, j, h))
            return ALFS_EIO;
        if (state_of(lg, h) == S_USED && alfs_get32(h->claim) == log && alfs_get32(h->claim + 4) > *seq) {
            *k = j;
            *seq = alfs_get32(h->claim + 4);
            return ALFS_OK;
        }
    }
    return ALFS_ENOENT;
}

/*
 * SPEC 3: from *pos in sector k, the next good chunk of stream tag. 1 with
 * *pos and *len set and its payload in buf, 0 at the end, or ALFS_EIO.
 */
static int scan_chunk(alfs_log_t *lg, uint32_t k, uint8_t tag, uint32_t *pos, uint32_t *len, uint8_t *buf) {
    uint32_t base = alfs_addr(&lg->d, k), x = *pos;
    while (x + HDR_LEN < sector_size(lg)) {
        uint8_t h[HDR_LEN];
        uint32_t np = next_page(lg, x);
        if (np - x < HDR_LEN + 1u) {
            x = np;
            continue;
        }
        if (alfs_read(&lg->d, base + x, h, HDR_LEN))
            return ALFS_EIO;
        if (alfs_is_erased(h, HDR_LEN)) {
            x = np;
            continue;
        }
        uint32_t l = alfs_get16(h);
        if (!l || x + HDR_LEN + l > np) {
            lg->d.n.check_failures++;
            x = np;
            continue;
        }
        if (h[2] != tag) {
            x += HDR_LEN + l;
            continue;
        }
        uint32_t c = alfs_crc(ALFS_CRC_INIT, h, 3);
        for (uint32_t i = 0; i < l;) {
            uint32_t n = l - i < ALFS_PIECE ? l - i : ALFS_PIECE;
            if (alfs_read(&lg->d, base + x + HDR_LEN + i, buf + i, n))
                return ALFS_EIO;
            c = alfs_crc(c, buf + i, n);
            i += n;
        }
        if (~c == alfs_get32(h + 3)) {
            *pos = x;
            *len = l;
            return 1;
        }
        lg->d.n.check_failures++;
        x = np;
    }
    return 0;
}

int alfs_log_open_read(alfs_log_t *lg, alfs_reader_t *r, uint32_t log, uint8_t tag, uint8_t *block,
                       uint32_t size) {
    if (!lg || !lg->d.cfg || !r || !log || !block_ok(lg, block, size))
        return ALFS_EINVAL;
    bool found = false;
    uint32_t first = 0, seq = 0;
    for (uint32_t k = 0; k < lg->d.sectors; k++) {
        hdr_t *h = HDR(lg);
        if (read_hdr(lg, k, h))
            return ALFS_EIO;
        uint32_t s = alfs_get32(h->claim + 4);
        if (state_of(lg, h) == S_USED && alfs_get32(h->claim) == log && (!found || s < seq)) {
            first = k;
            seq = s;
            found = true;
        }
    }
    if (!found)
        return ALFS_ENOENT;
    memset(r, 0, sizeof(*r));
    r->block = block;
    r->cap = (uint16_t)size;
    r->log = log;
    r->seq = seq;
    r->sector = (uint16_t)first;
    r->off = (uint16_t)OFF_DATA;
    r->tag = tag;
    r->open = 1;
    lg->readers++;
    return ALFS_OK;
}

uint32_t alfs_log_give(alfs_reader_t *r, void *out, uint32_t len) {
    uint32_t n = r->open ? r->used : 0u;
    if (n > len)
        n = len;
    if (n)
        memcpy(out, r->block + r->pos, n);
    r->pos = (uint16_t)(r->pos + n);
    r->used = (uint16_t)(r->used - n);
    return n;
}

int32_t alfs_log_read(alfs_log_t *lg, alfs_reader_t *r, void *out, uint32_t len) {
    if (!lg || !lg->d.cfg || !r || !r->open || (len && !out))
        return ALFS_EINVAL;
    for (uint32_t guard = 0; guard <= lg->d.sectors; guard++) {
        if (r->used || !len)
            return (int32_t)alfs_log_give(r, out, len);
        uint32_t x = r->off, l = 0;
        int rc = scan_chunk(lg, r->sector, r->tag, &x, &l, r->block);
        if (rc < 0)
            return rc;
        if (rc == 1) {
            r->off = (uint16_t)(x + HDR_LEN + l);
            r->pos = 0;
            r->used = (uint16_t)l;
            guard--;
            continue;
        }
        uint32_t k = r->sector, seq = r->seq;
        rc = next_sector(lg, r->log, &k, &seq);
        if (rc == ALFS_ENOENT)
            return 0; /* the end of what is written: a later read goes on from here */
        if (rc)
            return rc;
        r->sector = (uint16_t)k;
        r->seq = seq;
        r->off = (uint16_t)OFF_DATA;
    }
    return 0;
}

int alfs_log_close_read(alfs_log_t *lg, alfs_reader_t *r) {
    if (!lg || !lg->d.cfg || !r || !r->open)
        return ALFS_EINVAL;
    r->open = 0;
    lg->readers--;
    return ALFS_OK;
}

/* ---- listing ---------------------------------------------------------- */

int alfs_log_list(alfs_log_t *lg, uint32_t after, alfs_log_info_t *info) {
    if (!lg || !lg->d.cfg || !info)
        return ALFS_EINVAL;
    bool found = false;
    for (uint32_t k = 0; k < lg->d.sectors; k++) {
        hdr_t *h = HDR(lg);
        if (read_hdr(lg, k, h))
            return ALFS_EIO;
        uint32_t log = alfs_get32(h->claim);
        if (state_of(lg, h) != S_USED || log <= after)
            continue;
        if (!found || log < info->log) {
            info->log = log;
            info->sectors = 0;
            found = true;
        }
        if (log == info->log)
            info->sectors++;
    }
    return found ? ALFS_OK : ALFS_ENOENT;
}

void alfs_log_status(const alfs_log_t *lg, alfs_log_status_t *st) {
    memset(st, 0, sizeof(*st));
    if (!lg->d.cfg)
        return;
    st->sectors = lg->d.sectors;
    st->ready = lg->ready;
    st->dirty = lg->dirty;
    st->bad = lg->nbad;
    st->writable = (uint32_t)lg->ready * (sector_size(lg) - OFF_DATA - HDR_LEN);
    st->writing = lg->cur;
    st->clean = !lg->dirty;
    st->n = lg->d.n;
}
