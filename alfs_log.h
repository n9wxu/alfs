/*
 * alfs_log: the log area. Logs are appended at one place that moves
 * forward around a ring of sectors, each sector erased ahead of time:
 * nothing erases unless the caller asks (alfs_log_erase).
 *
 * A log is one flight. It holds a stream for each tag its writers use, so
 * a flight log and a high-rate log written by two tasks are two streams of
 * one log. Logs are numbered as they start; numbers grow and are never
 * used again. After a mount, writing starts a new log.
 *
 * A writer brings a block of RAM: a write fills it and, when it is full,
 * programs it in chunks that fill their pages. A sync programs the rest. A
 * reader's block holds the chunk being read.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ALFS_LOG_H
#define ALFS_LOG_H

#include "alfs.h"

/* An instance. Opaque. */
typedef struct alfs_log {
    alfs_dev_t d;
    uint32_t next_seq; /* the next claim's sequence, or log's number */
    uint32_t cur;      /* the log being written in this mount, or 0 */
    uint16_t head;     /* where the next chunk goes, or none: a new sector */
    uint16_t off;
    uint16_t alloc_cursor, erase_cursor;
    uint16_t fresh;    /* a ready sector not to use: where allocation stood */
    uint16_t ready, dirty, nbad;
    uint16_t bad[ALFS_BAD_MAX]; /* sectors found bad this mount */
    uint8_t writers, readers;   /* handles open */
} alfs_log_t;

/* A writer of one stream. The caller's, with its block, until closed.
 * Opaque but for `log`, which a caller may read. */
typedef struct alfs_writer {
    uint8_t *block;
    uint32_t log;
    uint16_t cap, used;
    uint8_t tag, open;
} alfs_writer_t;

/* A reader of one stream. Opaque. */
typedef struct alfs_reader {
    uint8_t *block;
    uint32_t log, seq; /* seq: the sector being read's */
    uint16_t cap, used, pos;
    uint16_t sector, off;
    uint8_t tag, open;
} alfs_reader_t;

typedef struct alfs_log_info {
    uint32_t log;
    uint16_t sectors;
} alfs_log_info_t;

typedef struct alfs_log_status {
    uint16_t sectors, ready, dirty, bad;
    uint32_t writable; /* bytes that fit in ready sectors, about */
    uint32_t writing;  /* the log being written, or 0 */
    bool clean;        /* no dirty sector left */
    alfs_counters_t n;
} alfs_log_status_t;

/*
 * Mounts the log area. Reads 30 bytes of each sector and writes nothing. A
 * sector holding anything but a log sector of this geometry is dirty.
 * scratch: one page, the instance's from now on. ALFS_EGEOMETRY,
 * ALFS_EINVAL: nothing read. ALFS_EOPEN: a handle is open. ALFS_EIO: retry.
 */
int alfs_log_mount(alfs_log_t *lg, const alfs_area_t *cfg, const alfs_driver_t *drv, uint8_t *scratch);

/*
 * Makes up to `steps` sectors ready: each one dirty, or, when log is not 0,
 * of that log. Erases it, checks it erased, and stamps it. ALFS_OK: there
 * may be more. ALFS_CLEAN: none is left. ALFS_EOPEN: log is being written,
 * or a reader is open. ALFS_EIO: retry. A sector that fails is counted bad
 * and skipped.
 */
int alfs_log_erase(alfs_log_t *lg, uint32_t log, uint32_t steps);

/*
 * Opens a writer of stream `tag`: of a new log when log is 0, or of the
 * log being written (w->log of another writer). A new log starts only when
 * no writer is open, and takes its first sector at once, so its number is
 * on the flash before it is given. block: at least one page and at most
 * 65,535 bytes. Its number is w->log. ALFS_EINVAL, ALFS_EOPEN; for a new
 * log ALFS_ENOSPC, ALFS_EPROG (retry), ALFS_EIO.
 */
int alfs_log_open(alfs_log_t *lg, alfs_writer_t *w, uint32_t log, uint8_t tag, uint8_t *block, uint32_t size);

/*
 * Writes len bytes: copies them into the block and, each time it fills,
 * programs all of it but less than a page's room. Returns when every byte
 * is in flash or in the block: len, or fewer when no sector was ready, after
 * which the next call returns ALFS_ENOSPC. A program that fails drops its
 * bytes, counted in bytes_lost, and the log goes on. ALFS_EINVAL,
 * ALFS_ENOSPC, ALFS_EIO: nothing taken.
 */
int32_t alfs_log_write(alfs_log_t *lg, alfs_writer_t *w, const void *data, uint32_t len);

/* Makes every byte written so far durable. ALFS_OK, or an error as
 * alfs_log_write's. */
int alfs_log_sync(alfs_log_t *lg, alfs_writer_t *w);

/* Syncs and releases the writer. When the sync fails it stays open, its
 * bytes in its block: retry. */
int alfs_log_close(alfs_log_t *lg, alfs_writer_t *w);

/*
 * Opens a reader of stream `tag` of log `log`, from its start. block: as
 * alfs_log_open's; each chunk is read into it and checked whole before any
 * of it is returned. ALFS_ENOENT, ALFS_EINVAL, ALFS_EIO.
 */
int alfs_log_open_read(alfs_log_t *lg, alfs_reader_t *r, uint32_t log, uint8_t tag, uint8_t *block,
                       uint32_t size);

/*
 * Reads up to len bytes and returns how many, 0 at the end of what is
 * written. A chunk whose check fails is skipped and counted. ALFS_EIO:
 * nothing consumed; retry.
 */
int32_t alfs_log_read(alfs_log_t *lg, alfs_reader_t *r, void *out, uint32_t len);

int alfs_log_close_read(alfs_log_t *lg, alfs_reader_t *r);

/*
 * The log with the least number greater than after; 0 starts the list.
 * ALFS_ENOENT past the last. Numbers grow with age.
 */
int alfs_log_list(alfs_log_t *lg, uint32_t after, alfs_log_info_t *info);

void alfs_log_status(const alfs_log_t *lg, alfs_log_status_t *st);

/*
 * RAM only, for a caller that serves alfs from another task (alfs_rtos.h).
 * alfs_log_take copies into a writer's block as much of data as it has room
 * for, and returns how much; alfs_log_write takes the rest. alfs_log_give
 * copies out of a reader's block what it holds, up to len.
 */
uint32_t alfs_log_take(alfs_writer_t *w, const void *data, uint32_t len);
uint32_t alfs_log_give(alfs_reader_t *r, void *out, uint32_t len);

#endif
