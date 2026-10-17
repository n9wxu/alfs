/*
 * alfs_obj: the object area. Named objects, each replaced whole (put) and
 * read whole (get), for configuration and programs saved on the ground.
 *
 * The area is two halves. A put writes every other object again into the
 * spare half, then the new one, then a commit that makes that half the
 * store: until the commit, gets read what was there before, and after any
 * power loss each object reads whole. A put erases the spare half first
 * when it is not ready; alfs_obj_erase readies it ahead of time.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ALFS_OBJ_H
#define ALFS_OBJ_H

#include "alfs.h"

typedef struct alfs_put alfs_put_t;

/* An instance. Opaque. */
typedef struct alfs_obj {
    alfs_dev_t d;
    uint32_t seq, end;      /* the active half's commit */
    const alfs_put_t *put;  /* the put in progress */
    uint16_t half;          /* sectors in a half */
    uint16_t erased;        /* the spare's sectors erased in this mount */
    uint8_t active;         /* 0 or 1 */
    uint8_t ready;          /* the spare half is ready */
    uint8_t readers[2];     /* gets open in each half */
} alfs_obj_t;

/* A put in progress. The caller's until committed or abandoned. Opaque. */
struct alfs_put {
    uint32_t at, left, crc;
};

/* A get. Opaque but for `size`, the object's. */
typedef struct alfs_get {
    uint32_t at, left, size;
    uint8_t half, open;
} alfs_get_t;

typedef struct alfs_obj_info {
    char name[ALFS_NAME_MAX + 1];
    uint32_t size;
} alfs_obj_info_t;

typedef struct alfs_obj_status {
    uint32_t used;     /* bytes the objects take, their records included */
    uint32_t capacity; /* bytes a half holds for objects */
    uint32_t seq;      /* the active half's commit */
    bool ready;        /* the spare half is ready: a put erases nothing */
    alfs_counters_t n;
} alfs_obj_status_t;

/*
 * Claims the area for an empty store, discarding whatever it held: erases
 * both halves and commits half 0. ALFS_EGEOMETRY, ALFS_EINVAL: nothing
 * written. ALFS_EOPEN: a handle is open. ALFS_EPROG: a sector failed; fail.
 * ALFS_EIO: retry.
 */
int alfs_obj_format(alfs_obj_t *o, const alfs_area_t *cfg, const alfs_driver_t *drv, uint8_t *scratch);

/*
 * Mounts the object area. Reads 29 bytes of each half and writes nothing.
 * ALFS_ENOFS: no store; rebuild or fail. ALFS_EGEOMETRY, ALFS_EINVAL:
 * nothing read. ALFS_EOPEN: a handle is open. ALFS_EIO: retry.
 */
int alfs_obj_mount(alfs_obj_t *o, const alfs_area_t *cfg, const alfs_driver_t *drv, uint8_t *scratch);

/*
 * Erases up to `steps` sectors of the spare half toward making it ready.
 * ALFS_OK: more remain. ALFS_CLEAN: it is ready. ALFS_EOPEN: a get or a put
 * holds it. ALFS_EPROG: a sector failed. ALFS_EIO: retry.
 */
int alfs_obj_erase(alfs_obj_t *o, uint32_t steps);

/*
 * Begins a put of `size` bytes as object `name` (1 to ALFS_NAME_MAX bytes),
 * which replaces any object of that name when committed. Readies the spare
 * half if it is not, and writes the other objects into it. ALFS_EINVAL,
 * ALFS_ENOSPC (it would not fit), ALFS_EOPEN (another put, or a get holds
 * the spare half), ALFS_EPROG, ALFS_EIO: nothing changed.
 */
int alfs_obj_put(alfs_obj_t *o, alfs_put_t *p, const char *name, uint32_t size);

/* Writes the next of the object's bytes; at most what the put has left.
 * Returns len. ALFS_EINVAL. ALFS_EPROG, ALFS_EIO: the put is abandoned. */
int32_t alfs_obj_write(alfs_obj_t *o, alfs_put_t *p, const void *data, uint32_t len);

/* Commits the put once all its bytes are written: the object is replaced.
 * ALFS_EINVAL (bytes left). ALFS_EPROG, ALFS_EIO: the put is abandoned and
 * the store is as it was. */
int alfs_obj_commit(alfs_obj_t *o, alfs_put_t *p);

/* Abandons the put. The store is as it was. */
int alfs_obj_abort(alfs_obj_t *o, alfs_put_t *p);

/* Deletes an object: a put of every other object. ALFS_ENOENT, and the
 * errors of alfs_obj_put. */
int alfs_obj_delete(alfs_obj_t *o, const char *name);

/*
 * Opens object `name` to read it from its start: reads it whole and checks
 * it. Its size is g->size. ALFS_ENOENT, ALFS_ECHECK, ALFS_EINVAL, ALFS_EIO.
 */
int alfs_obj_open(alfs_obj_t *o, alfs_get_t *g, const char *name);

/* Reads up to len bytes and returns how many, 0 at the end. ALFS_EIO:
 * nothing consumed; retry. */
int32_t alfs_obj_read(alfs_obj_t *o, alfs_get_t *g, void *out, uint32_t len);

int alfs_obj_close(alfs_obj_t *o, alfs_get_t *g);

/* The index'th object, from 0, in the order the store holds them.
 * ALFS_ENOENT past the last. */
int alfs_obj_list(alfs_obj_t *o, uint32_t index, alfs_obj_info_t *info);

void alfs_obj_status(const alfs_obj_t *o, alfs_obj_status_t *st);

#endif
