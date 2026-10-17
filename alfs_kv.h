/*
 * alfs_kv: the configuration store. Key-value pairs on a ring of sectors.
 *
 * One sector, the head, holds the current pairs. A set appends the new
 * pair and zeroes the live mark of the one it replaces. When the head is
 * full, and at the first set in a mount, the next sector round the ring is
 * erased, every current pair is copied in, and it becomes the head. A get
 * or a listing scans the head.
 *
 * A set may erase one sector: settings are saved on the ground.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ALFS_KV_H
#define ALFS_KV_H

#include "alfs.h"

#define ALFS_KEY_MAX 31 /* bytes in a key, none 00h */

/* An instance. Opaque. */
typedef struct alfs_kv {
    alfs_dev_t d;
    uint32_t seq;      /* the head's claim; 0: no head */
    uint32_t next_seq;
    uint32_t sets;     /* sets and deletes this mount: a listing's check */
    uint16_t head;
    uint16_t end;      /* where the next pair goes, once moved */
    uint8_t moved;     /* the head was moved in this mount (SPEC 14) */
} alfs_kv_t;

/* A listing in progress. Zero it to start. */
typedef struct alfs_kv_iter {
    uint32_t sets;
    uint16_t pos;
} alfs_kv_iter_t;

typedef struct alfs_kv_status {
    uint16_t sectors;
    uint16_t head; /* ALFS_KV_NO_HEAD before the first set */
    uint32_t seq;
    alfs_counters_t n;
} alfs_kv_status_t;

#define ALFS_KV_NO_HEAD 0xFFFFu

/*
 * Mounts the configuration store. Reads 19 bytes of each sector and writes
 * nothing. An area holding no store mounts empty. ALFS_EGEOMETRY,
 * ALFS_EINVAL: nothing read. ALFS_EIO: retry.
 */
int alfs_kv_mount(alfs_kv_t *kv, const alfs_area_t *cfg, const alfs_driver_t *drv, uint8_t *scratch);

/*
 * The value of key (1 to ALFS_KEY_MAX bytes): copied into value, its
 * length in *len. ALFS_ENOENT. ALFS_EINVAL: a bad key, or cap is under
 * *len and nothing was copied. ALFS_EIO: retry.
 */
int alfs_kv_get(alfs_kv_t *kv, const char *key, void *value, uint32_t cap, uint32_t *len);

/*
 * Sets key's value: len bytes, at most a page less 8 and the key's length;
 * 0 deletes the key. ALFS_EINVAL. ALFS_ENOSPC: the pairs would not fit in a
 * sector; nothing changed. ALFS_EPROG: a sector failed; nothing changed,
 * fail. ALFS_EIO: retry.
 */
int alfs_kv_set(alfs_kv_t *kv, const char *key, const void *value, uint32_t len);

/* Deletes key. ALFS_ENOENT, and the errors of alfs_kv_set. */
int alfs_kv_delete(alfs_kv_t *kv, const char *key);

/*
 * The next live pair of a listing, in the order written: its key,
 * NUL-terminated, into key[ALFS_KEY_MAX + 1], its value into value, its
 * length in *len. A later pair of a key replaces an earlier one, and an
 * empty value deletes the key: applied in order, a listing gives every
 * current pair. ALFS_ENOENT at the end. ALFS_EINVAL: cap is under *len, or
 * a set since the listing began (start again). ALFS_EIO: retry.
 */
int alfs_kv_next(alfs_kv_t *kv, alfs_kv_iter_t *it, char *key, void *value, uint32_t cap, uint32_t *len);

void alfs_kv_status(const alfs_kv_t *kv, alfs_kv_status_t *st);

#endif
