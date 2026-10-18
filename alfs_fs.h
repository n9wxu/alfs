/*
 * alfs_fs: files on serial NOR flash, each a frame of pages linked in a
 * list. A file's first page holds its filename; a mount reads every page's
 * header and builds a cache of the files in a table the caller gives.
 *
 * A filename converts to its file's frame ID, the descriptor every later
 * call on the open file uses. alfs_file_alloc reserves a file's pages and
 * links them, so that writing it later programs only erased flash. A write
 * gathers bytes in a block of RAM the writer brings, and programs each
 * complete page of them along the list, each page's data once, with its
 * byte count and check; a sync programs the complete pages and keeps the
 * rest; a flush programs the rest too, as a partial page, and the file goes
 * on in the next page; a close programs the rest and the file's page count
 * and size, ends the list at its last page, and blocks out the older copy it
 * replaces. A closed file is final. A file that grows past its reservation
 * takes free pages while there are any. A delete blocks out the filename and
 * leaves the pages. alfs_fs_cleanup is the only call that erases: sector by
 * sector, the sectors that hold no live page.
 *
 * Two kinds of file: a log, appended to and reopened after a reset that cut
 * it short; and a configuration file, written whole and read whole, and
 * written again as a new copy that replaces the old when it closes.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ALFS_FS_H
#define ALFS_FS_H

#include "alfs.h"

/* A file in the cache: a record of the table the caller gives the mount.
 * Opaque. */
typedef struct alfs_rec {
    uint32_t id;     /* frame ID; 0: a free record */
    uint16_t first;  /* the first page's address */
    uint16_t pages;  /* a closed copy's page count */
    uint16_t hash;   /* of the filename */
    uint8_t version;
    uint8_t flags;
} alfs_rec_t;

/* An instance. Opaque. */
typedef struct alfs_fs {
    alfs_dev_t d;
    alfs_rec_t *cache;
    uint32_t next_id;  /* the next frame ID */
    uint32_t pages;    /* pages in the area */
    uint32_t search;   /* where the search for a free page goes on */
    uint32_t sweep;    /* where the next cleanup step looks */
    uint16_t ncache;   /* records in the table */
    uint8_t writers, readers, nbad;
    uint16_t bad[ALFS_BAD_MAX]; /* sectors found bad this mount */
} alfs_fs_t;

/* A file open for writing. The caller's, with its block, until closed.
 * Opaque but for `id`, its descriptor. */
typedef struct alfs_file {
    uint8_t *block;
    uint32_t id;
    uint32_t page;  /* the page the block's next page of data goes to */
    uint32_t prev;  /* the page before it in the list */
    uint32_t left;  /* bytes the list's pages from `page` hold, without ALFS_GROW */
    uint32_t size;  /* bytes so far, programmed or in the block */
    uint16_t first; /* its first page */
    uint16_t cap;   /* the block's size */
    uint16_t used;  /* bytes in the block */
    uint16_t count; /* pages so far */
    uint8_t flags;
    uint8_t open;
} alfs_file_t;

/* A file open for reading. Opaque but for `id` and `size`. */
typedef struct alfs_reader {
    uint8_t *block;
    uint32_t id;
    uint32_t size;  /* a closed copy's bytes; ALFS_SIZE_OPEN if not closed */
    uint32_t page;  /* the page whose data the block holds, or the next */
    uint16_t pages; /* a closed copy's page count */
    uint16_t count; /* the page's position */
    uint16_t pos, used;
    uint8_t open, closed, failed;
} alfs_reader_t;

#define ALFS_SIZE_OPEN 0xFFFFFFFFu

typedef struct alfs_file_info {
    uint32_t id;
    char name[ALFS_NAME_MAX + 1];
    uint32_t size;  /* bytes, once closed; ALFS_SIZE_OPEN before */
    uint16_t pages; /* pages, once closed */
    uint8_t version;
    bool closed;
} alfs_file_info_t;

/* alfs_file_open's flags. */
#define ALFS_GROW 1u /* writes past the reservation take free pages */

typedef struct alfs_fs_status {
    uint32_t pages;               /* pages that can hold a file */
    uint32_t live, free, dead;
    uint16_t records, records_max; /* the cache's records used, and its size */
    uint16_t data_per_page;       /* bytes of data in a page but a file's first */
    uint16_t bad;                 /* sectors found bad this mount */
    bool clean;                   /* no dead page */
    alfs_counters_t n;
} alfs_fs_status_t;

/*
 * Mounts the area: reads every page's header, and the frame block of each
 * file's first page, and builds the cache in `cache`, n records, the
 * instance's from now on, with `scratch`, one page. Writes nothing. An area
 * holding anything else mounts with no file. ALFS_ENOSPC: more files than n.
 * ALFS_EOPEN: a file is open. ALFS_EGEOMETRY, ALFS_EINVAL: nothing read.
 * ALFS_EIO: retry.
 */
int alfs_fs_mount(alfs_fs_t *fs, const alfs_area_t *cfg, const alfs_driver_t *drv, uint8_t *scratch,
                  alfs_rec_t *cache, uint16_t n);

/*
 * Reserves a copy of `bytes` under filename name, links its pages, and
 * returns its frame ID in *id. A filename that names a closed file gets a
 * new copy, which replaces it when closed; a copy not closed is blocked
 * out. ALFS_ENOSPC: fewer free pages than the reservation needs, or no free
 * record; nothing written: clean up, then retry. ALFS_EINVAL; ALFS_EOPEN: the
 * file is open; ALFS_EPROG, ALFS_EIO: nothing reserved.
 */
int alfs_file_alloc(alfs_fs_t *fs, const char *name, uint32_t bytes, uint32_t *id);

/*
 * Opens file name for writing: its newest copy, not closed, from where its
 * data ends; after a mount, past one page skipped. f->id is its descriptor.
 * flags: ALFS_GROW, or 0 to stop at the reservation's end. block: at least
 * one page, and at most 65,535 bytes. ALFS_ENOENT, ALFS_EOPEN (open
 * already), ALFS_EEXIST (closed: final), ALFS_EINVAL; ALFS_EIO.
 */
int alfs_file_open(alfs_fs_t *fs, alfs_file_t *f, const char *name, unsigned flags, uint8_t *block,
                   uint32_t size);

/*
 * Copies len bytes into the block; each time it fills, programs every
 * complete page of data it holds along the list, and past the reservation
 * into free pages when the file was opened with ALFS_GROW. Returns when
 * every byte is in flash or in the block: len, or fewer when the list
 * ended, after which the next call returns ALFS_ENOSPC. A program that fails
 * drops its page's bytes, counted in bytes_lost, and writing goes on.
 * ALFS_EINVAL, ALFS_ENOSPC, ALFS_EIO: nothing taken.
 */
int32_t alfs_file_write(alfs_fs_t *fs, alfs_file_t *f, const void *data, uint32_t len);

/* Programs every complete page of data the block holds; the rest stays.
 * ALFS_OK, or an error as alfs_file_write's. */
int alfs_file_sync(alfs_fs_t *fs, alfs_file_t *f);

/* Programs everything the block holds, the rest as a partial page; the file
 * goes on in the next page. ALFS_OK, or an error as alfs_file_write's. */
int alfs_file_flush(alfs_fs_t *fs, alfs_file_t *f);

/*
 * Programs the rest as a partial page, the close fields and the zero link,
 * blocks out the older copy, and releases the writer; the file is final.
 * When the program of the rest fails for want of a page it stays open:
 * ALFS_ENOSPC. ALFS_EINVAL, ALFS_EIO.
 */
int alfs_file_close(alfs_fs_t *fs, alfs_file_t *f);

/*
 * Opens file name for reading from its start: its newest closed copy, or
 * its newest copy if none is closed. block: at least one page. ALFS_ENOENT,
 * ALFS_EINVAL, ALFS_EIO.
 */
int alfs_file_open_read(alfs_fs_t *fs, alfs_reader_t *r, const char *name, uint8_t *block, uint32_t size);

/*
 * Reads up to len bytes and returns how many; 0 at the end. A page whose
 * data check fails is skipped and counted. ALFS_ECHECK, at a closed file's
 * end: a page was skipped, or its list ended short of its page count; the
 * file is not valid. ALFS_EIO: nothing consumed; retry.
 */
int32_t alfs_file_read(alfs_fs_t *fs, alfs_reader_t *r, void *out, uint32_t len);

int alfs_file_close_read(alfs_fs_t *fs, alfs_reader_t *r);

/* Blocks out every copy of file name; the pages stay for a cleanup.
 * ALFS_ENOENT, ALFS_EOPEN, ALFS_EINVAL; ALFS_EIO: retry. */
int alfs_file_delete(alfs_fs_t *fs, const char *name);

/* The file with the least frame ID greater than after; 0 starts the list.
 * ALFS_ENOENT past the last. ALFS_EIO: retry. */
int alfs_file_list(alfs_fs_t *fs, uint32_t after, alfs_file_info_t *info);

/*
 * Erases up to `steps` sectors, one a step: each holding no live page and
 * not reading erased; reads each back as all 1s. ALFS_OK: there may be
 * more. ALFS_CLEAN: none is left. ALFS_EIO: retry. A sector that fails is
 * counted bad and skipped.
 */
int alfs_fs_cleanup(alfs_fs_t *fs, uint32_t steps);

/* Reads every page header to count live, free and dead pages; counts no
 * failure. ALFS_EIO: retry. */
int alfs_fs_status(alfs_fs_t *fs, alfs_fs_status_t *st);

#endif
