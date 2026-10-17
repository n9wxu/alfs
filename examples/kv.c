/*
 * kv: settings kept as rows of a log area. See kv.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "kv.h"

#include <string.h>

#define TRIES 3u
#define ROW_CAP 255u
#define CHUNK_HDR 7u /* docs/SPEC.md 1.3 */

static const uint8_t mark = 0;

/* 1 to KV_KEY_MAX, or 0 for a bad key. */
static uint32_t key_len(const char *key) {
    uint32_t k = 0;
    while (key && k <= KV_KEY_MAX && key[k])
        k++;
    return k <= KV_KEY_MAX ? k : 0;
}

static kv_slot_t *find(const kv_t *kv, const uint8_t *key, uint32_t k) {
    for (uint32_t i = 0; i < kv->n; i++) {
        kv_slot_t *s = &kv->slots[i];
        if (s->len && s->row[0] == k && !memcmp(s->row + 1, key, k))
            return s;
    }
    return NULL;
}

/* A free slot, or else one a log older than `log` set. The older log's
 * settings fill slots past their number only when it was cut short by an
 * erase, which follows a newer log's mark; that mark frees them anyway. */
static kv_slot_t *slot_for(const kv_t *kv, uint32_t log) {
    kv_slot_t *older = NULL;
    for (uint32_t i = 0; i < kv->n; i++) {
        kv_slot_t *s = &kv->slots[i];
        if (!s->len)
            return s;
        if (s->log < log)
            older = s;
    }
    return older;
}

/* One row of log `log`, read while loading. A row that is neither a
 * setting nor a mark is passed over. */
static int apply(kv_t *kv, const uint8_t *row, uint32_t len, uint32_t log) {
    uint32_t k = row[0];
    if (len == 1 && k == 0) {
        for (uint32_t i = 0; i < kv->n; i++)
            if (kv->slots[i].log != log)
                kv->slots[i].len = 0;
        kv->base = log;
        return ALFS_OK;
    }
    if (!k || k > KV_KEY_MAX || 1 + k > len)
        return ALFS_OK;
    kv_slot_t *s = find(kv, row + 1, k);
    if (len == 1 + k) {
        if (s)
            s->len = 0;
        return ALFS_OK;
    }
    if (!s && !(s = slot_for(kv, log)))
        return ALFS_ENOSPC;
    memcpy(s->row, row, len);
    s->len = (uint8_t)len;
    s->log = log;
    return ALFS_OK;
}

int kv_load(kv_t *kv, alfs_log_t *lg, kv_slot_t *slots, uint32_t n, uint8_t *block, uint32_t size) {
    if (!kv || !lg || !slots || !block)
        return ALFS_EINVAL;
    memset(kv, 0, sizeof(*kv));
    memset(slots, 0, n * sizeof(*slots));
    kv->lg = lg;
    kv->slots = slots;
    kv->n = n;
    kv->block = block;
    kv->size = size;
    alfs_log_info_t in;
    int rc;
    for (uint32_t after = 0; (rc = alfs_log_list(lg, after, &in)) == ALFS_OK; after = in.log) {
        alfs_reader_t r;
        uint8_t row[ROW_CAP];
        int32_t len;
        rc = alfs_log_open_read(lg, &r, in.log, KV_TAG, block, size);
        if (rc == ALFS_ENOENT)
            continue; /* its one claim, torn, read valid to the listing */
        if (rc)
            return rc;
        while ((len = alfs_log_read(lg, &r, row, sizeof(row))) > 0 &&
               (rc = apply(kv, row, (uint32_t)len, in.log)) == ALFS_OK)
            ;
        alfs_log_close_read(lg, &r);
        if (len < 0)
            return (int)len;
        if (rc)
            return rc;
    }
    return rc == ALFS_ENOENT ? ALFS_OK : rc;
}

int kv_get(const kv_t *kv, const char *key, const uint8_t **value, uint32_t *len) {
    uint32_t k = key_len(key);
    if (!kv || !k || !value || !len)
        return ALFS_EINVAL;
    const kv_slot_t *s = find(kv, (const uint8_t *)key, k);
    if (!s)
        return ALFS_ENOENT;
    *value = s->row + 1 + k;
    *len = s->len - 1u - k;
    return ALFS_OK;
}

static uint32_t lost(const kv_t *kv) {
    alfs_log_status_t st;
    alfs_log_status(kv->lg, &st);
    return st.n.bytes_lost;
}

/* Writes a row and syncs it; again when a failed program dropped it. */
static int append(kv_t *kv, const uint8_t *row, uint32_t len) {
    for (uint32_t t = 0; t < TRIES; t++) {
        uint32_t was = lost(kv);
        int rc = alfs_log_write(kv->lg, &kv->w, row, len);
        if (!rc)
            rc = alfs_log_sync(kv->lg, &kv->w);
        if (rc || lost(kv) == was)
            return rc;
    }
    return ALFS_EPROG;
}

/* Erase steps of log, 0 for the dirty sectors alone, until none is left. */
static int erase_all(kv_t *kv, uint32_t log) {
    alfs_log_status_t st;
    alfs_log_status(kv->lg, &st);
    for (uint32_t i = 0; i <= st.sectors; i++) {
        int rc = alfs_log_erase(kv->lg, log, 1);
        if (rc != ALFS_OK)
            return rc == ALFS_CLEAN ? ALFS_OK : rc;
    }
    return ALFS_OK;
}

static bool any_set(const kv_t *kv) {
    for (uint32_t i = 0; i < kv->n; i++)
        if (kv->slots[i].len)
            return true;
    return false;
}

/* Erases the dirty sectors and every log but the base, none of which holds
 * a setting of its own (kv.h). With no base, a setting read comes from logs
 * a mark never followed, which stay. */
static int erase_others(kv_t *kv) {
    int rc = erase_all(kv, 0);
    if (rc || (!kv->base && any_set(kv)))
        return rc;
    alfs_log_status_t st;
    alfs_log_status(kv->lg, &st);
    uint32_t after = 0;
    for (uint32_t i = 0; i <= st.sectors; i++) {
        alfs_log_info_t in;
        rc = alfs_log_list(kv->lg, after, &in);
        if (rc)
            return rc == ALFS_ENOENT ? ALFS_OK : rc;
        if (in.log == kv->base)
            after = in.log;
        else if ((rc = erase_all(kv, in.log)) != ALFS_OK)
            return rc;
    }
    return ALFS_OK;
}

/*
 * Too little ready room, once a row of len bytes has taken up to a sector,
 * to write every setting again. A chunk holds whole rows, so at worst each
 * row takes a page of its own: its header and a row's length unused.
 */
static bool low(const kv_t *kv, uint32_t len) {
    alfs_log_status_t st;
    alfs_log_status(kv->lg, &st);
    uint32_t longest = len, rows = 2, need = 2u + 1u + len; /* the mark, the row */
    for (uint32_t i = 0; i < kv->n; i++)
        if (kv->slots[i].len) {
            rows++;
            need += 1u + kv->slots[i].len;
            if (kv->slots[i].len > longest)
                longest = kv->slots[i].len;
        }
    need += rows * (CHUNK_HDR + longest);
    uint32_t sector = st.ready ? st.writable / st.ready : 0;
    return st.writable < need + sector;
}

/*
 * A new base: the other logs erased, then a row for every setting and the
 * mark, each synced, then the old base erased. Until the mark is on the
 * flash the old base holds the settings. A failed program drops rows, so
 * the next try starts another log.
 */
static int compact(kv_t *kv) {
    int rc = ALFS_EPROG;
    kv->whole = false;
    for (uint32_t t = 0; t < TRIES; t++) {
        if (kv->w.open && (rc = alfs_log_close(kv->lg, &kv->w)) != ALFS_OK)
            return rc;
        if ((rc = erase_others(kv)) != ALFS_OK)
            return rc;
        rc = alfs_log_open(kv->lg, &kv->w, 0, KV_TAG, kv->block, kv->size);
        if (rc == ALFS_ENOSPC || rc == ALFS_EPROG)
            continue; /* the sector refused is dirty now: the next try erases it */
        if (rc)
            return rc;
        uint32_t was = lost(kv);
        for (uint32_t i = 0; !rc && i < kv->n; i++)
            if (kv->slots[i].len)
                rc = alfs_log_write(kv->lg, &kv->w, kv->slots[i].row, kv->slots[i].len);
        if (!rc)
            rc = alfs_log_sync(kv->lg, &kv->w);
        if (!rc)
            rc = lost(kv) == was ? append(kv, &mark, 1) : ALFS_EPROG;
        if (!rc) {
            kv->whole = true;
            kv->base = kv->w.log;
            return erase_others(kv);
        }
        if (rc != ALFS_EPROG)
            return rc;
    }
    return rc;
}

int kv_set(kv_t *kv, const char *key, const void *value, uint32_t len) {
    uint32_t k = key_len(key), n = 1u + k + len;
    if (!kv || !k || (len && !value))
        return ALFS_EINVAL;
    alfs_log_status_t st;
    alfs_log_status(kv->lg, &st);
    if (len > st.row_max || n > st.row_max)
        return ALFS_EINVAL;
    kv_slot_t *s = find(kv, (const uint8_t *)key, k);
    if (!len && !s)
        return ALFS_ENOENT;
    if (!s && !(s = slot_for(kv, 0)))
        return ALFS_ENOSPC;
    uint8_t row[ROW_CAP];
    row[0] = (uint8_t)k;
    memcpy(row + 1, key, k);
    if (len)
        memcpy(row + 1 + k, value, len);
    int rc = ALFS_OK;
    if (!kv->whole || low(kv, n))
        rc = compact(kv);
    if (!rc)
        rc = append(kv, row, n);
    if (rc) {
        kv->whole = false;
        return rc;
    }
    memcpy(s->row, row, n);
    s->len = len ? (uint8_t)n : 0u;
    return ALFS_OK;
}
