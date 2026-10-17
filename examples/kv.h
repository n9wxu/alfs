/*
 * kv: settings kept as rows of an alfs log area, the configuration area.
 * An application of alfs, not a part of it: alfs stores rows and returns
 * them, and this file gives them their meaning.
 *
 * A row is one setting: its key's length k (1 to KV_KEY_MAX), the key,
 * then the value. A row whose value is empty deletes its key. A row of one
 * byte, 00h, is a mark: the settings before it in its log are all of them.
 *
 * The settings live in RAM, one to a slot the caller gives. Loading reads
 * every row of every log in order: a setting replaces its key's, and a mark
 * keeps only the settings its own log set. The newest log with a mark, the
 * base, holds every setting: a log before it holds older ones, and a log
 * after it, which has no mark, a part of the base's.
 *
 * A set appends its row and syncs it. The first set after loading, and a
 * set that would leave too little ready room to write every setting again,
 * first starts a new log: it erases every log but the base, writes a row
 * for every setting, then the mark, synced, and only then erases the base.
 * So after a power loss at any moment each setting reads as it was, or as
 * the set cut short would make it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef KV_H
#define KV_H

#include "alfs_log.h"

#define KV_KEY_MAX 31 /* bytes in a key, none 00h */
#define KV_TAG 1      /* the stream the rows are in */

typedef struct kv_slot {
    uint32_t log;     /* the log whose row set it, while loading */
    uint8_t len;      /* the row's length; 0: a free slot */
    uint8_t row[255];
} kv_slot_t;

typedef struct kv {
    alfs_log_t *lg;
    kv_slot_t *slots;
    uint32_t n;
    uint8_t *block; /* the writer's, and the reader's while loading */
    uint32_t size;
    alfs_writer_t w;
    uint32_t base;  /* the newest log with a mark, or 0 */
    bool whole;     /* the log being written is the base */
} kv_t;

/*
 * Loads the settings of lg, a mounted log area, into n slots. block: as
 * alfs_log_open's, kv's from now on. ALFS_ENOSPC: more keys than slots.
 * The errors of alfs_log.h.
 */
int kv_load(kv_t *kv, alfs_log_t *lg, kv_slot_t *slots, uint32_t n, uint8_t *block, uint32_t size);

/* Key's value, where its slot holds it. ALFS_ENOENT; ALFS_EINVAL: a bad
 * key. */
int kv_get(const kv_t *kv, const char *key, const uint8_t **value, uint32_t *len);

/*
 * Sets key's value to len bytes; 0 deletes the key. Returns once its row is
 * synced. ALFS_EINVAL: a bad key, or a row longer than the area's row_max.
 * ALFS_ENOENT: a delete of a key that is not set, which writes nothing.
 * ALFS_ENOSPC: no free slot, or the area cannot hold every setting again.
 * ALFS_EPROG: programs kept failing. The errors of alfs_log.h.
 */
int kv_set(kv_t *kv, const char *key, const void *value, uint32_t len);

#endif
