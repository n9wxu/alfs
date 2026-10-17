/*
 * alfs_obj: the object area. See alfs_obj.h; the format is docs/SPEC.md 6
 * to 10.
 *
 * SPDX-License-Identifier: MIT
 */
#include "alfs_obj.h"

#include "alfs_flash.h"

#include <string.h>

#define STAMP_LEN 16u
#define OFF_BEGUN STAMP_LEN
#define OFF_COMMIT (OFF_BEGUN + 1u)
#define COMMIT_LEN 12u
#define RECS_LEN (OFF_COMMIT + COMMIT_LEN) /* the bytes a mount reads of a half */
#define NAME_LEN 32u
#define OBJ_HDR (NAME_LEN + 4u) /* name, size */
#define OBJ_OVER (OBJ_HDR + 4u) /* and the CRC after the payload */

static uint32_t page(const alfs_obj_t *o) {
    return o->d.cfg->page_size;
}

static uint32_t half_bytes(const alfs_obj_t *o) {
    return (uint32_t)o->half * o->d.cfg->sector_size;
}

static uint32_t half_addr(const alfs_obj_t *o, uint32_t h) {
    return alfs_addr(&o->d, h * o->half);
}

static uint32_t spare(const alfs_obj_t *o) {
    return o->active ^ 1u;
}

/* A name's 32 bytes (SPEC 6.3), or false when it is not a name. */
static bool name_bytes(const char *name, uint8_t *out) {
    uint32_t n = 0;
    while (name && n <= ALFS_NAME_MAX && name[n])
        n++;
    if (!n || n > ALFS_NAME_MAX)
        return false;
    memset(out, 0, NAME_LEN);
    memcpy(out, name, n);
    return true;
}

/* ---- records ---------------------------------------------------------- */

static void build_stamp(const alfs_obj_t *o, uint8_t *st) {
    memcpy(st, "alfo", 4);
    st[4] = ALFS_VERSION;
    st[5] = (uint8_t)alfs_lg2(o->d.cfg->sector_size);
    st[6] = (uint8_t)alfs_lg2(page(o));
    st[7] = 0;
    alfs_put16(st + 8, o->half);
    alfs_put16(st + 10, 0);
    alfs_put32(st + 12, ~alfs_crc(ALFS_CRC_INIT, st, 12));
}

static void build_commit(uint32_t seq, uint32_t end, uint8_t *c) {
    alfs_put32(c, seq);
    alfs_put32(c + 4, end);
    alfs_put32(c + 8, ~alfs_crc(ALFS_CRC_INIT, c, 8));
}

static bool commit_valid(const alfs_obj_t *o, const uint8_t *c) {
    uint32_t end = alfs_get32(c + 4);
    return !alfs_is_erased(c, COMMIT_LEN) && alfs_get32(c + 8) == ~alfs_crc(ALFS_CRC_INIT, c, 8) &&
           end >= page(o) && end <= half_bytes(o);
}

/* Programs n bytes at offset at of half h, through the scratch page, in
 * programs that do not cross a page. */
static int write_at(alfs_obj_t *o, uint32_t h, uint32_t at, const uint8_t *src, uint32_t n) {
    while (n) {
        uint32_t k = page(o) - at % page(o);
        if (k > n)
            k = n;
        memcpy(o->d.scratch, src, k);
        if (alfs_prog(&o->d, half_addr(o, h) + at, o->d.scratch, k))
            return ALFS_EPROG;
        at += k;
        src += k;
        n -= k;
    }
    return ALFS_OK;
}

/* ---- walking the active half -------------------------------------------- */

/* The object at *x of the active half: its 36 header bytes and its size.
 * ALFS_ENOENT at the end, or where a size runs past it. */
static int object_at(alfs_obj_t *o, uint32_t x, uint8_t *hdr, uint32_t *size) {
    if (x + OBJ_OVER > o->end)
        return ALFS_ENOENT;
    if (alfs_read(&o->d, half_addr(o, o->active) + x, hdr, OBJ_HDR))
        return ALFS_EIO;
    *size = alfs_get32(hdr + NAME_LEN);
    return *size <= o->end - x - OBJ_OVER ? ALFS_OK : ALFS_ENOENT;
}

/* The object called name32 in the active half: where it starts and its
 * size. ALFS_ENOENT when there is none. */
static int find(alfs_obj_t *o, const uint8_t *name32, uint32_t *at, uint32_t *size) {
    uint8_t hdr[OBJ_HDR];
    for (uint32_t x = page(o);; x += OBJ_OVER + *size) {
        int rc = object_at(o, x, hdr, size);
        if (rc)
            return rc;
        if (!memcmp(hdr, name32, NAME_LEN)) {
            *at = x;
            return ALFS_OK;
        }
    }
}

/* ---- the spare half ----------------------------------------------------- */

/*
 * Erases up to steps of the spare's sectors, then stamps it (SPEC 8 step
 * 1). Before the first erase, the active commit is programmed again
 * (SAFE-13).
 */
static int ready_spare(alfs_obj_t *o, uint32_t steps) {
    if (o->ready)
        return ALFS_CLEAN;
    if (o->put || o->readers[spare(o)])
        return ALFS_EOPEN;
    if (!o->erased && o->seq) {
        uint8_t c[COMMIT_LEN];
        build_commit(o->seq, o->end, c);
        alfs_prog(&o->d, half_addr(o, o->active) + OFF_COMMIT, c, COMMIT_LEN);
    }
    for (; steps && o->erased < o->half; steps--) {
        int rc = alfs_wipe(&o->d, spare(o) * o->half + o->erased);
        if (rc)
            return rc;
        o->erased++;
    }
    if (o->erased < o->half)
        return ALFS_OK;
    uint8_t st[STAMP_LEN];
    build_stamp(o, st);
    if (alfs_prog(&o->d, half_addr(o, spare(o)), st, STAMP_LEN)) {
        o->erased = 0;
        return ALFS_EPROG;
    }
    o->ready = 1;
    return ALFS_CLEAN;
}

/*
 * SPEC 8 steps 1 to 3: the spare made ready and begun, and every active
 * object but name32 copied into it. *at: where the next object goes.
 */
static int rewrite(alfs_obj_t *o, const uint8_t *name32, uint32_t *at) {
    int rc = ready_spare(o, o->half);
    if (rc != ALFS_CLEAN)
        return rc;
    uint8_t *s = o->d.scratch;
    build_stamp(o, s);
    s[OFF_BEGUN] = 0;
    o->ready = 0;
    o->erased = 0; /* written from here on */
    if (alfs_prog(&o->d, half_addr(o, spare(o)), s, OFF_BEGUN + 1u))
        return ALFS_EPROG;
    uint32_t y = page(o), size;
    uint8_t hdr[OBJ_HDR];
    for (uint32_t x = page(o); (rc = object_at(o, x, hdr, &size)) == ALFS_OK; x += OBJ_OVER + size) {
        if (!memcmp(hdr, name32, NAME_LEN))
            continue;
        for (uint32_t i = 0, n = OBJ_OVER + size; i < n;) {
            uint32_t k = page(o) - (y + i) % page(o);
            if (k > n - i)
                k = n - i;
            if (alfs_read(&o->d, half_addr(o, o->active) + x + i, s, k))
                return ALFS_EIO;
            if (alfs_prog(&o->d, half_addr(o, spare(o)) + y + i, s, k))
                return ALFS_EPROG;
            i += k;
        }
        y += OBJ_OVER + size;
    }
    *at = y;
    return rc == ALFS_ENOENT ? ALFS_OK : rc;
}

/* SPEC 8 step 5: the commit that makes the spare the store. */
static int finish(alfs_obj_t *o, uint32_t end) {
    uint8_t c[COMMIT_LEN];
    build_commit(o->seq + 1u, end, c);
    if (alfs_prog(&o->d, half_addr(o, spare(o)) + OFF_COMMIT, c, COMMIT_LEN))
        return ALFS_EPROG;
    o->active = (uint8_t)spare(o);
    o->seq++;
    o->end = end;
    o->erased = 0;
    o->ready = 0;
    return ALFS_OK;
}

/* ---- mount and format ------------------------------------------------- */

static int bind(alfs_obj_t *o, const alfs_area_t *cfg, const alfs_driver_t *drv, uint8_t *scratch) {
    if (!o)
        return ALFS_EINVAL;
    if (o->d.cfg && (o->put || o->readers[0] || o->readers[1]))
        return ALFS_EOPEN;
    int rc = alfs_check_area(cfg, drv);
    if (rc)
        return rc;
    if ((cfg->size / cfg->sector_size) % 2u)
        return ALFS_EGEOMETRY;
    memset(o, 0, sizeof(*o));
    rc = alfs_dev_init(&o->d, cfg, drv, scratch);
    o->half = (uint16_t)(o->d.sectors / 2u);
    return rc;
}

int alfs_obj_mount(alfs_obj_t *o, const alfs_area_t *cfg, const alfs_driver_t *drv, uint8_t *scratch) {
    int rc = bind(o, cfg, drv, scratch);
    if (rc)
        return rc;
    uint8_t r[2][RECS_LEN], st[STAMP_LEN];
    bool committed[2];
    build_stamp(o, st);
    for (uint32_t h = 0; h < 2; h++) {
        if (alfs_read(&o->d, half_addr(o, h), r[h], RECS_LEN)) {
            o->d.cfg = NULL;
            return ALFS_EIO;
        }
        committed[h] = !memcmp(r[h], st, STAMP_LEN) && commit_valid(o, r[h] + OFF_COMMIT);
    }
    if (!committed[0] && !committed[1]) {
        o->d.cfg = NULL;
        return ALFS_ENOFS;
    }
    o->active = committed[1] && (!committed[0] || alfs_get32(r[1] + OFF_COMMIT) > alfs_get32(r[0] + OFF_COMMIT));
    o->seq = alfs_get32(r[o->active] + OFF_COMMIT);
    o->end = alfs_get32(r[o->active] + OFF_COMMIT + 4);
    const uint8_t *s = r[spare(o)];
    o->ready = !memcmp(s, st, STAMP_LEN) && alfs_is_erased(s + OFF_BEGUN, 1u + COMMIT_LEN);
    return ALFS_OK;
}

int alfs_obj_format(alfs_obj_t *o, const alfs_area_t *cfg, const alfs_driver_t *drv, uint8_t *scratch) {
    int rc = bind(o, cfg, drv, scratch);
    for (uint32_t k = 0; !rc && k < o->d.sectors; k++)
        rc = alfs_wipe(&o->d, k);
    uint8_t st[STAMP_LEN];
    if (!rc) {
        build_stamp(o, st);
        rc = alfs_prog(&o->d, half_addr(o, 1), st, STAMP_LEN);
    }
    if (!rc) { /* an empty store, committed in half 0, from half 1 */
        o->active = 1;
        o->erased = o->half;
        rc = ready_spare(o, 0) == ALFS_CLEAN ? ALFS_OK : ALFS_EPROG;
    }
    uint32_t at = 0;
    if (!rc)
        rc = rewrite(o, st, &at);
    if (!rc)
        rc = finish(o, page(o));
    if (rc) {
        o->d.cfg = NULL;
        return rc;
    }
    o->ready = 1; /* half 1: stamped, and nothing written since */
    return ALFS_OK;
}

/* ---- put and delete --------------------------------------------------- */

int alfs_obj_erase(alfs_obj_t *o, uint32_t steps) {
    if (!o || !o->d.cfg)
        return ALFS_EINVAL;
    return ready_spare(o, steps);
}

int alfs_obj_put(alfs_obj_t *o, alfs_put_t *p, const char *name, uint32_t size) {
    uint8_t hdr[OBJ_HDR]; /* the object's name, then its size */
    if (!o || !o->d.cfg || !p || !name_bytes(name, hdr))
        return ALFS_EINVAL;
    if (o->put)
        return ALFS_EOPEN;
    uint32_t used = o->end, at, old;
    int rc = find(o, hdr, &at, &old);
    if (rc == ALFS_EIO)
        return rc;
    if (!rc)
        used -= OBJ_OVER + old;
    if (size > half_bytes(o) || used + OBJ_OVER + size > half_bytes(o))
        return ALFS_ENOSPC;
    rc = rewrite(o, hdr, &at);
    alfs_put32(hdr + NAME_LEN, size);
    if (!rc)
        rc = write_at(o, spare(o), at, hdr, OBJ_HDR);
    if (rc)
        return rc;
    p->at = at + OBJ_HDR;
    p->left = size;
    p->crc = alfs_crc(ALFS_CRC_INIT, hdr, OBJ_HDR);
    o->put = p;
    return ALFS_OK;
}

int32_t alfs_obj_write(alfs_obj_t *o, alfs_put_t *p, const void *data, uint32_t len) {
    if (!o || !o->d.cfg || !p || o->put != p || (len && !data) || len > p->left)
        return ALFS_EINVAL;
    if (write_at(o, spare(o), p->at, data, len)) {
        o->put = NULL;
        return ALFS_EPROG;
    }
    p->crc = alfs_crc(p->crc, data, len);
    p->at += len;
    p->left -= len;
    return (int32_t)len;
}

int alfs_obj_commit(alfs_obj_t *o, alfs_put_t *p) {
    if (!o || !o->d.cfg || !p || o->put != p || p->left)
        return ALFS_EINVAL;
    uint8_t c[4];
    alfs_put32(c, ~p->crc);
    o->put = NULL;
    if (write_at(o, spare(o), p->at, c, 4))
        return ALFS_EPROG;
    return finish(o, p->at + 4u);
}

int alfs_obj_abort(alfs_obj_t *o, alfs_put_t *p) {
    if (!o || !o->d.cfg || !p || o->put != p)
        return ALFS_EINVAL;
    o->put = NULL;
    return ALFS_OK;
}

int alfs_obj_delete(alfs_obj_t *o, const char *name) {
    uint8_t name32[NAME_LEN];
    uint32_t at, size;
    if (!o || !o->d.cfg || !name_bytes(name, name32))
        return ALFS_EINVAL;
    if (o->put)
        return ALFS_EOPEN;
    int rc = find(o, name32, &at, &size);
    if (!rc)
        rc = rewrite(o, name32, &at);
    return rc ? rc : finish(o, at);
}

/* ---- get -------------------------------------------------------------- */

int alfs_obj_open(alfs_obj_t *o, alfs_get_t *g, const char *name) {
    uint8_t name32[NAME_LEN], b[ALFS_PIECE];
    uint32_t at, size;
    if (!o || !o->d.cfg || !g || !name_bytes(name, name32))
        return ALFS_EINVAL;
    int rc = find(o, name32, &at, &size);
    if (rc)
        return rc;
    uint32_t a = half_addr(o, o->active) + at, c = ALFS_CRC_INIT;
    for (uint32_t i = 0, n = OBJ_HDR + size; i < n;) {
        uint32_t k = n - i < ALFS_PIECE ? n - i : ALFS_PIECE;
        if (alfs_read(&o->d, a + i, b, k))
            return ALFS_EIO;
        c = alfs_crc(c, b, k);
        i += k;
    }
    if (alfs_read(&o->d, a + OBJ_HDR + size, b, 4))
        return ALFS_EIO;
    if (alfs_get32(b) != ~c) {
        o->d.n.check_failures++;
        return ALFS_ECHECK;
    }
    memset(g, 0, sizeof(*g));
    g->at = at + OBJ_HDR;
    g->left = g->size = size;
    g->half = o->active;
    g->open = 1;
    o->readers[o->active]++;
    return ALFS_OK;
}

int32_t alfs_obj_read(alfs_obj_t *o, alfs_get_t *g, void *out, uint32_t len) {
    if (!o || !o->d.cfg || !g || !g->open || (len && !out))
        return ALFS_EINVAL;
    uint32_t n = len < g->left ? len : g->left;
    if (n && alfs_read(&o->d, half_addr(o, g->half) + g->at, out, n))
        return ALFS_EIO;
    g->at += n;
    g->left -= n;
    return (int32_t)n;
}

int alfs_obj_close(alfs_obj_t *o, alfs_get_t *g) {
    if (!o || !o->d.cfg || !g || !g->open)
        return ALFS_EINVAL;
    g->open = 0;
    o->readers[g->half]--;
    return ALFS_OK;
}

/* ---- listing ---------------------------------------------------------- */

int alfs_obj_list(alfs_obj_t *o, uint32_t index, alfs_obj_info_t *info) {
    uint8_t hdr[OBJ_HDR];
    uint32_t size;
    if (!o || !o->d.cfg || !info)
        return ALFS_EINVAL;
    for (uint32_t x = page(o), i = 0;; x += OBJ_OVER + size, i++) {
        int rc = object_at(o, x, hdr, &size);
        if (rc)
            return rc;
        if (i == index) {
            memcpy(info->name, hdr, ALFS_NAME_MAX);
            info->name[ALFS_NAME_MAX] = 0;
            info->size = size;
            return ALFS_OK;
        }
    }
}

void alfs_obj_status(const alfs_obj_t *o, alfs_obj_status_t *st) {
    memset(st, 0, sizeof(*st));
    if (!o->d.cfg)
        return;
    st->used = o->end - page(o);
    st->capacity = half_bytes(o) - page(o);
    st->seq = o->seq;
    st->ready = o->ready;
    st->n = o->d.n;
}
