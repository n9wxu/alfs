/*
 * alfs_kv: the configuration store. See alfs_kv.h; the format is
 * docs/SPEC.md 11 to 14.
 *
 * SPDX-License-Identifier: MIT
 */
#include "alfs_kv.h"

#include "alfs_flash.h"

#include <string.h>

#define CLAIM_LEN 18u
#define OFF_MARK CLAIM_LEN
#define OFF_PAIRS (OFF_MARK + 1u)
#define PAIR_HDR 4u               /* live mark, key length, value length */
#define PAIR_OVER (PAIR_HDR + 4u) /* and the CRC */
#define LIVE 0xFFu
#define NONE ALFS_NONE

/* A whole pair the walk found: its bytes are in the scratch page, read
 * once, so its check and its value come from the same read. */
typedef struct {
    uint32_t at, len, vlen;
    uint8_t klen, live;
    uint8_t key[ALFS_KEY_MAX];
} pair_t;

static uint32_t page(const alfs_kv_t *kv) {
    return kv->d.cfg->page_size;
}

static uint32_t sector_size(const alfs_kv_t *kv) {
    return kv->d.cfg->sector_size;
}

static uint32_t next_page(const alfs_kv_t *kv, uint32_t x) {
    return (x / page(kv) + 1u) * page(kv);
}

static uint32_t key_len(const char *key) {
    uint32_t n = 0;
    while (key && n <= ALFS_KEY_MAX && key[n])
        n++;
    return n <= ALFS_KEY_MAX ? n : 0;
}

static void build_claim(const alfs_kv_t *kv, uint32_t seq, uint8_t *c) {
    memcpy(c, "alfk", 4);
    c[4] = ALFS_VERSION;
    c[5] = (uint8_t)alfs_lg2(sector_size(kv));
    c[6] = (uint8_t)alfs_lg2(page(kv));
    c[7] = 0;
    alfs_put16(c + 8, kv->d.sectors);
    alfs_put32(c + 10, seq);
    alfs_put32(c + 14, ~alfs_crc(ALFS_CRC_INIT, c, 14));
}

static bool claim_valid(const alfs_kv_t *kv, const uint8_t *c) {
    uint8_t want[CLAIM_LEN];
    build_claim(kv, alfs_get32(c + 10), want);
    return !memcmp(c, want, CLAIM_LEN);
}

/* ---- walking a sector --------------------------------------------------- */

/*
 * SPEC 12: from *x in sector k, the next whole pair, live or replaced,
 * read whole into the scratch page. 1 with *p set and *x past it, 0 at the
 * sector's end, or ALFS_EIO.
 */
static int next_pair(alfs_kv_t *kv, uint32_t k, uint32_t *x, pair_t *p) {
    uint32_t base = alfs_addr(&kv->d, k);
    uint8_t *s = kv->d.scratch;
    while (*x + PAIR_OVER + 1u <= sector_size(kv)) {
        uint8_t h[PAIR_HDR];
        uint32_t np = next_page(kv, *x);
        if (np - *x < PAIR_OVER + 1u) {
            *x = np;
            continue;
        }
        if (alfs_read(&kv->d, base + *x, h, PAIR_HDR))
            return ALFS_EIO;
        if (alfs_is_erased(h, PAIR_HDR)) {
            *x = np;
            continue;
        }
        uint32_t klen = h[1], vlen = alfs_get16(h + 2), body = PAIR_HDR + klen + vlen;
        bool whole = klen && klen <= ALFS_KEY_MAX && *x + body + 4u <= np;
        if (whole && alfs_read(&kv->d, base + *x, s, body + 4u))
            return ALFS_EIO;
        if (!whole || memcmp(s + 1, h + 1, PAIR_HDR - 1u) ||
            alfs_get32(s + body) != ~alfs_crc(ALFS_CRC_INIT, s + 1, body - 1u)) {
            kv->d.n.check_failures++;
            *x = np;
            continue;
        }
        p->at = *x;
        p->len = body + 4u;
        p->klen = (uint8_t)klen;
        p->vlen = vlen;
        p->live = s[0] == LIVE;
        memcpy(p->key, s + PAIR_HDR, klen);
        *x += p->len;
        return 1;
    }
    return 0;
}

static bool same_key(const pair_t *p, const uint8_t *key, uint32_t klen) {
    return p->klen == klen && !memcmp(p->key, key, klen);
}

/*
 * SPEC 13: key's last live pair in the head. 1 with *p set, 0 when there
 * is none, or ALFS_EIO. With value given, the last's value is copied there
 * when it fits in cap, from the read its check passed.
 */
static int find(alfs_kv_t *kv, const uint8_t *key, uint32_t klen, pair_t *p, void *value, uint32_t cap) {
    pair_t q;
    int rc = 0, found = 0;
    for (uint32_t x = OFF_PAIRS; kv->head != NONE && (rc = next_pair(kv, kv->head, &x, &q)) == 1;)
        if (q.live && same_key(&q, key, klen)) {
            *p = q;
            found = 1;
            if (value && q.vlen <= cap)
                memcpy(value, kv->d.scratch + PAIR_HDR + klen, q.vlen);
        }
    return rc < 0 ? rc : found;
}

/* ---- mount ------------------------------------------------------------ */

int alfs_kv_mount(alfs_kv_t *kv, const alfs_area_t *cfg, const alfs_driver_t *drv, uint8_t *scratch) {
    if (!kv)
        return ALFS_EINVAL;
    memset(kv, 0, sizeof(*kv));
    int rc = alfs_dev_init(&kv->d, cfg, drv, scratch);
    if (!rc && kv->d.sectors < 2u)
        rc = ALFS_EGEOMETRY;
    uint32_t newest = 0;
    kv->head = NONE;
    for (uint32_t k = 0; !rc && k < kv->d.sectors; k++) {
        uint8_t c[OFF_PAIRS];
        if (alfs_read(&kv->d, alfs_addr(&kv->d, k), c, OFF_PAIRS)) {
            rc = ALFS_EIO;
            break;
        }
        uint32_t seq = alfs_get32(c + 10);
        if (!claim_valid(kv, c))
            continue;
        if (seq > newest)
            newest = seq;
        if (!c[OFF_MARK] && seq > kv->seq) {
            kv->seq = seq;
            kv->head = (uint16_t)k;
        }
    }
    kv->next_seq = newest + 1u;
    if (rc)
        kv->d.cfg = NULL;
    return rc;
}

/* ---- writing ---------------------------------------------------------- */

/* A pair programmed at offset at of sector k, built in the scratch page. */
static int write_pair(alfs_kv_t *kv, uint32_t k, uint32_t at, const uint8_t *key, uint32_t klen, const void *value,
                      uint32_t vlen) {
    uint8_t *s = kv->d.scratch;
    s[0] = LIVE;
    s[1] = (uint8_t)klen;
    alfs_put16(s + 2, vlen);
    memcpy(s + PAIR_HDR, key, klen);
    memcpy(s + PAIR_HDR + klen, value, vlen);
    uint32_t body = PAIR_HDR + klen + vlen;
    alfs_put32(s + body, ~alfs_crc(ALFS_CRC_INIT, s + 1, body - 1u));
    return alfs_prog(&kv->d, alfs_addr(&kv->d, k) + at, s, body + 4u);
}

/* Where a pair of len bytes goes from *y: there, or the next page start.
 * False when it runs past the sector. */
static bool place(const alfs_kv_t *kv, uint32_t *y, uint32_t len) {
    if (next_page(kv, *y) - *y < len)
        *y = next_page(kv, *y);
    return *y + len <= sector_size(kv);
}

/*
 * SPEC 14: every live pair of the head but key's, in order, to sector t
 * from *y on, each programmed from the read its check passed; with t NONE,
 * only placed. ALFS_ENOSPC when they run past the sector.
 */
static int copy_live(alfs_kv_t *kv, uint32_t t, const uint8_t *key, uint32_t klen, uint32_t *y) {
    pair_t p;
    int rc = 0;
    for (uint32_t x = OFF_PAIRS; kv->head != NONE && (rc = next_pair(kv, kv->head, &x, &p)) == 1;) {
        if (!p.live || same_key(&p, key, klen))
            continue;
        if (!place(kv, y, p.len))
            return ALFS_ENOSPC;
        if (t != NONE && alfs_prog(&kv->d, alfs_addr(&kv->d, t) + *y, kv->d.scratch, p.len))
            return ALFS_EPROG;
        *y += p.len;
    }
    return rc < 0 ? rc : ALFS_OK;
}

/* 1 when a live pair of p's key follows p in sector k, 0 when none, or
 * ALFS_EIO. */
static int replaced(alfs_kv_t *kv, uint32_t k, const pair_t *p) {
    pair_t q;
    int rc;
    for (uint32_t x = p->at + p->len; (rc = next_pair(kv, k, &x, &q)) == 1;)
        if (q.live && same_key(&q, p->key, p->klen))
            return 1;
    return rc;
}

/* SPEC 14: in sector t, every live pair a later one replaces, and every
 * empty one, has its live mark zeroed; in order, so a delete's pair still
 * replaces those before it when they are reached. */
static int tidy(alfs_kv_t *kv, uint32_t t) {
    pair_t p;
    int rc;
    uint8_t zero = 0;
    for (uint32_t x = OFF_PAIRS; (rc = next_pair(kv, t, &x, &p)) == 1;) {
        if (!p.live)
            continue;
        int r = p.vlen ? replaced(kv, t, &p) : 1;
        if (r < 0)
            return r;
        if (r && alfs_prog(&kv->d, alfs_addr(&kv->d, t) + p.at, &zero, 1))
            return ALFS_EPROG;
    }
    return rc;
}

/*
 * SPEC 14: the sector after the head becomes the head, holding every live
 * pair and key's new one (none when vlen is 0), tidied, and made the head
 * by its complete mark. Nothing changes when they would not fit.
 */
static int move_head(alfs_kv_t *kv, const uint8_t *key, uint32_t klen, const void *value, uint32_t vlen) {
    uint32_t t = kv->head == NONE ? 0u : (kv->head + 1u) % kv->d.sectors, y = OFF_PAIRS, len = PAIR_OVER + klen + vlen;
    int rc = copy_live(kv, NONE, key, klen, &y);
    if (rc)
        return rc;
    if (vlen && !place(kv, &y, len))
        return ALFS_ENOSPC;
    rc = alfs_wipe(&kv->d, t);
    if (rc)
        return rc;
    uint8_t c[CLAIM_LEN];
    uint32_t seq = kv->next_seq++;
    build_claim(kv, seq, c);
    if (alfs_prog(&kv->d, alfs_addr(&kv->d, t), c, CLAIM_LEN))
        return ALFS_EPROG;
    y = OFF_PAIRS;
    rc = copy_live(kv, t, key, klen, &y);
    if (!rc && vlen && place(kv, &y, len)) {
        rc = write_pair(kv, t, y, key, klen, value, vlen);
        y += len;
    }
    if (!rc)
        rc = tidy(kv, t);
    uint8_t mark = 0;
    if (!rc)
        rc = alfs_prog(&kv->d, alfs_addr(&kv->d, t) + OFF_MARK, &mark, 1);
    if (rc)
        return rc;
    kv->head = (uint16_t)t;
    kv->seq = seq;
    kv->end = (uint16_t)y;
    kv->moved = 1; /* erased in this mount: nothing torn in it */
    return ALFS_OK;
}

int alfs_kv_set(alfs_kv_t *kv, const char *key, const void *value, uint32_t len) {
    uint32_t klen = key_len(key);
    if (!kv || !kv->d.cfg || !klen || len + PAIR_OVER + klen > page(kv) || (len && !value))
        return ALFS_EINVAL;
    const uint8_t *k = (const uint8_t *)key;
    pair_t old;
    int had = find(kv, k, klen, &old, NULL, 0);
    if (had < 0)
        return had;
    if (!len && (!had || !old.vlen))
        return ALFS_OK; /* deleted already */
    kv->sets++;
    /* SAFE-16: the first set in a mount moves the head, so nothing a power
     * loss tore in the old one is programmed again */
    uint32_t y = kv->end, plen = PAIR_OVER + klen + len;
    if (!kv->moved || !place(kv, &y, plen))
        return move_head(kv, k, klen, value, len);
    int rc = write_pair(kv, kv->head, y, k, klen, value, len);
    if (rc)
        return rc;
    kv->end = (uint16_t)(y + plen);
    /* every earlier live pair of the key: its live mark zeroed */
    pair_t p;
    uint8_t zero = 0;
    for (uint32_t x = OFF_PAIRS; (rc = next_pair(kv, kv->head, &x, &p)) == 1 && p.at < y;)
        if (p.live && same_key(&p, k, klen))
            alfs_prog(&kv->d, alfs_addr(&kv->d, kv->head) + p.at, &zero, 1);
    return rc < 0 ? rc : ALFS_OK;
}

int alfs_kv_delete(alfs_kv_t *kv, const char *key) {
    uint32_t klen = key_len(key);
    pair_t p;
    if (!kv || !kv->d.cfg || !klen)
        return ALFS_EINVAL;
    int had = find(kv, (const uint8_t *)key, klen, &p, NULL, 0);
    if (had < 0)
        return had;
    return had && p.vlen ? alfs_kv_set(kv, key, NULL, 0) : ALFS_ENOENT;
}

/* ---- reading ---------------------------------------------------------- */

int alfs_kv_get(alfs_kv_t *kv, const char *key, void *value, uint32_t cap, uint32_t *len) {
    uint32_t klen = key_len(key);
    pair_t p;
    if (!kv || !kv->d.cfg || !klen || !len || (cap && !value))
        return ALFS_EINVAL;
    int rc = find(kv, (const uint8_t *)key, klen, &p, value, cap);
    if (rc < 0)
        return rc;
    if (!rc || !p.vlen)
        return ALFS_ENOENT;
    *len = p.vlen;
    return cap < p.vlen ? ALFS_EINVAL : ALFS_OK;
}

int alfs_kv_next(alfs_kv_t *kv, alfs_kv_iter_t *it, char *key, void *value, uint32_t cap, uint32_t *len) {
    if (!kv || !kv->d.cfg || !it || !key || !len || (cap && !value))
        return ALFS_EINVAL;
    if (!it->pos) {
        it->pos = OFF_PAIRS;
        it->sets = kv->sets;
    } else if (it->sets != kv->sets) {
        return ALFS_EINVAL;
    }
    pair_t p;
    int rc = 0;
    for (uint32_t x = it->pos; kv->head != NONE && (rc = next_pair(kv, kv->head, &x, &p)) == 1;) {
        if (!p.live)
            continue;
        *len = p.vlen;
        if (cap < p.vlen)
            return ALFS_EINVAL;
        memcpy(value, kv->d.scratch + PAIR_HDR + p.klen, p.vlen);
        memcpy(key, p.key, p.klen);
        key[p.klen] = 0;
        it->pos = (uint16_t)x;
        return ALFS_OK;
    }
    return rc < 0 ? rc : ALFS_ENOENT;
}

void alfs_kv_status(const alfs_kv_t *kv, alfs_kv_status_t *st) {
    memset(st, 0, sizeof(*st));
    st->head = ALFS_KV_NO_HEAD;
    if (!kv->d.cfg)
        return;
    st->sectors = kv->d.sectors;
    st->head = kv->head;
    st->seq = kv->seq;
    st->n = kv->d.n;
}
