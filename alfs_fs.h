/*
 * alfs_fs: files on serial NOR flash, each a frame of pages linked in a
 * list, found by filename through a catalog.
 *
 * A filename converts to its file's frame ID, the descriptor every later
 * call on the open file uses. alfs_file_alloc reserves a file's pages and
 * links them, so that writing it later programs only erased flash. A write
 * gathers bytes in a block of RAM the writer brings, and programs each
 * complete page of them along the list; a sync programs the complete pages
 * and keeps the rest; a close programs the rest, the frame's page count,
 * size and checksum, ends the list at its last page, and erases the
 * reservation's sectors it did not use. A file that grows past its
 * reservation takes pages already erased while there are any; nothing is
 * erased for it. A delete blocks out the filename and leaves the pages,
 * which a cleanup erases, sector by sector, once no file holds a page of
 * the sector.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ALFS_FS_H
#define ALFS_FS_H

#include "alfs.h"

/* An instance. Opaque. */
typedef struct alfs_fs {
    alfs_dev_t d;
    uint32_t next_id;     /* the next frame ID */
    uint32_t cat_seq;     /* the catalog's sequence */
    uint32_t search;      /* where the search for a free page goes on */
    uint32_t sweep;       /* where the next cleanup step looks */
    uint16_t cat_slot;    /* the catalog's next free slot */
    uint16_t cat_fresh;   /* a slot not to use (docs/SPEC.md 1.4) */
    uint8_t cat;          /* the catalog sector in use: 0 or 1 */
    uint8_t writers, readers, nbad;
    uint16_t bad[ALFS_BAD_MAX]; /* sectors found bad this mount */
} alfs_fs_t;

/* A file open for writing. The caller's, with its block, until closed.
 * Opaque but for `id`, its descriptor. */
typedef struct alfs_file {
    uint8_t *block;
    uint32_t id;
    uint32_t first;  /* its first page */
    uint32_t page;   /* the page the block's next page of data goes to */
    uint32_t size;   /* bytes so far, programmed or in the block */
    uint32_t crc;    /* the frame checksum so far */
    uint32_t cap;    /* the block's size */
    uint16_t used;   /* bytes in the block */
    uint16_t count;  /* pages holding data so far */
    uint8_t flags;
    uint8_t open;
} alfs_file_t;

/* A file open for reading. Opaque but for `id` and `size`. */
typedef struct alfs_reader {
    uint8_t *block;
    uint32_t id;
    uint32_t size;   /* a closed file's bytes; ALFS_SIZE_OPEN if not closed */
    uint32_t page;   /* the page whose data the block holds, or the next */
    uint32_t left;   /* a closed file's bytes not yet read */
    uint32_t crc;
    uint16_t count;  /* the page's position */
    uint16_t pos, used;
    uint8_t open, closed;
} alfs_reader_t;

#define ALFS_SIZE_OPEN 0xFFFFFFFFu

typedef struct alfs_file_info {
    uint32_t id;
    char name[ALFS_NAME_MAX + 1];
    uint32_t size;  /* bytes, once closed; ALFS_SIZE_OPEN before */
    uint16_t pages; /* pages holding data, once closed */
    bool closed;
} alfs_file_info_t;

/* alfs_file_open's flags. */
#define ALFS_GROW 1u /* writes past the reservation take free pages */

typedef struct alfs_fs_status {
    uint32_t pages;           /* frame pages in the area */
    uint32_t live, free, dead;
    uint16_t entries, entries_max; /* catalog slots used, and slots */
    uint16_t data_per_page;   /* bytes of data in a page but a file's first */
    uint16_t bad;             /* sectors found bad this mount */
    bool clean;               /* no dead page */
    alfs_counters_t n;
} alfs_fs_status_t;

/*
 * Erases the catalog sectors, writes an empty catalog, and mounts the area.
 * scratch: one page, the instance's from now on. ALFS_EGEOMETRY,
 * ALFS_EINVAL: nothing done. ALFS_EPROG: a catalog sector failed (fail).
 * ALFS_EIO: retry.
 */
int alfs_fs_format(alfs_fs_t *fs, const alfs_area_t *cfg, const alfs_driver_t *drv, uint8_t *scratch);

/*
 * Mounts the area: reads the catalog sectors and writes nothing.
 * ALFS_ENOFS: no catalog (format, or fail). ALFS_EOPEN: a file is open.
 * ALFS_EGEOMETRY, ALFS_EINVAL: nothing read. ALFS_EIO: retry.
 */
int alfs_fs_mount(alfs_fs_t *fs, const alfs_area_t *cfg, const alfs_driver_t *drv, uint8_t *scratch);

/*
 * Reserves a file of `bytes` under filename name, links its pages, and
 * returns its frame ID in *id. May erase sectors holding no live page when
 * too few pages are free, and rewrite the catalog when it has no slot.
 * ALFS_EEXIST, ALFS_EINVAL; ALFS_ENOSPC: not enough pages, or no catalog
 * slot left after a rewrite; ALFS_EPROG, ALFS_EIO: nothing reserved.
 */
int alfs_file_alloc(alfs_fs_t *fs, const char *name, uint32_t bytes, uint32_t *id);

/*
 * Opens file name for writing; f->id is its descriptor. flags: ALFS_GROW,
 * or 0 to stop at the reservation's end. block: at least one page, and at
 * most 65,535 bytes; at least one sector to reopen a closed file, which
 * rewrites the sectors of its first and last pages (IF-07). ALFS_ENOENT,
 * ALFS_EOPEN (open already), ALFS_EINVAL (a closed file and a block under a
 * sector); ALFS_EPROG, ALFS_EIO.
 */
int alfs_file_open(alfs_fs_t *fs, alfs_file_t *f, const char *name, unsigned flags, uint8_t *block,
                   uint32_t size);

/*
 * Copies len bytes into the block; each time it fills, programs every
 * complete page of data it holds along the list, and past the reservation
 * into free pages when the file was opened with ALFS_GROW. Returns when
 * every byte is in flash or in the block: len, or fewer when the list
 * ended, after which the next call returns ALFS_ENOSPC. A program that fails drops its page's bytes,
 * counted in bytes_lost, and writing goes on. ALFS_EINVAL, ALFS_ENOSPC,
 * ALFS_EIO: nothing taken.
 */
int32_t alfs_file_write(alfs_fs_t *fs, alfs_file_t *f, const void *data, uint32_t len);

/* Programs every complete page of data the block holds; the rest stays.
 * ALFS_OK, or an error as alfs_file_write's. */
int alfs_file_sync(alfs_fs_t *fs, alfs_file_t *f);

/*
 * Programs the rest, the close fields and the zero link, then erases the
 * reservation's sectors holding no live page, and releases the writer.
 * When the program of the rest fails for want of a page it stays open:
 * ALFS_ENOSPC. ALFS_EINVAL, ALFS_EIO.
 */
int alfs_file_close(alfs_fs_t *fs, alfs_file_t *f);

/*
 * Opens file name for reading from its start, closed or not. block: at
 * least one page. ALFS_ENOENT, ALFS_EINVAL, ALFS_EIO.
 */
int alfs_file_open_read(alfs_fs_t *fs, alfs_reader_t *r, const char *name, uint8_t *block, uint32_t size);

/*
 * Reads up to len bytes and returns how many; 0 at the end. A page whose
 * data check fails is skipped and counted. ALFS_ECHECK: a closed file's
 * frame checksum failed, at its end. ALFS_EIO: nothing consumed; retry.
 */
int32_t alfs_file_read(alfs_fs_t *fs, alfs_reader_t *r, void *out, uint32_t len);

int alfs_file_close_read(alfs_fs_t *fs, alfs_reader_t *r);

/*
 * Blocks out file name's filename, in the frame block and the catalog;
 * the pages stay for a cleanup. ALFS_ENOENT, ALFS_EOPEN, ALFS_EINVAL;
 * ALFS_EIO: retry.
 */
int alfs_file_delete(alfs_fs_t *fs, const char *name);

/* The file with the least frame ID greater than after; 0 starts the list.
 * ALFS_ENOENT past the last. ALFS_EIO: retry. */
int alfs_file_list(alfs_fs_t *fs, uint32_t after, alfs_file_info_t *info);

/*
 * Erases up to `steps` sectors, one a step: each holding no live page and
 * not reading erased; checks each erased. ALFS_OK: there may be more.
 * ALFS_CLEAN: none is left. ALFS_EIO: retry. A sector that fails is
 * counted bad and skipped.
 */
int alfs_fs_cleanup(alfs_fs_t *fs, uint32_t steps);

/* Reads every page header to count live, free and dead pages.
 * ALFS_EIO: retry. */
int alfs_fs_status(alfs_fs_t *fs, alfs_fs_status_t *st);

#endif
