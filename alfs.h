/*
 * alfs: the append-log flash store.
 *
 * Append-only streams on serial NOR flash, for a caller that runs code from
 * the same chip and cannot stall. Nothing erases in the append path: an
 * append programs only sectors erased ahead of time, and the caller runs
 * the erases, one step at a time, when a stall does no harm.
 *
 * docs/REQUIREMENTS.md says what alfs must do; docs/SPEC.md how its bytes
 * lie on the flash.
 *
 * Every call performs at most one flash operation (alfs_erase_step: one
 * erase and one program), never waits, and uses no memory but what the
 * caller passes in. The caller serialises calls on one instance.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ALFS_H
#define ALFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Return codes. Each error says the state it leaves and what to do:
 * retry, rebuild (format) or fail.
 */
enum {
    ALFS_OK = 0,
    /* alfs_erase_step, alfs_format: the erase goes on; call
     * alfs_erase_step again. alfs_sync: bytes remain; call it again. */
    ALFS_BUSY = 1,
    /* alfs_erase_step: no dirty sector is left. */
    ALFS_CLEAN = 2,
    /* A driver read failed. Nothing changed: retry. */
    ALFS_EIO = -1,
    /* No sector is ready for the stream's next chunk. Its buffered bytes
     * are kept: run alfs_erase_step (or delete a stream), then retry. */
    ALFS_ENOSPC = -2,
    /* The region holds no alfs: rebuild with alfs_format, or fail. */
    ALFS_ENOFS = -3,
    /* The configuration does not match the region, or the region does not
     * fit the chip. Nothing was written: fail. */
    ALFS_EGEOMETRY = -4,
    /* An argument or the handle's state is wrong. Nothing changed: fail
     * (it is a bug in the caller). */
    ALFS_EINVAL = -5,
    /* No stream has that identity. */
    ALFS_ENOENT = -6,
    /* A program failed. The bytes it carried are kept and go to the next
     * page on the next call: retry. */
    ALFS_EPROG = -7,
    /* The bytes failed max_prog_retries programs in a row and were
     * dropped, and counted in bytes_lost. The stream goes on: continue. */
    ALFS_ELOST = -8,
};

/*
 * The flash. Addresses are bytes from the start of the chip. Each returns
 * 0, or a negative value on failure.
 */
typedef struct alfs_driver {
    void *ctx;
    int (*read)(void *ctx, uint32_t addr, void *buf, uint32_t len);
    /* Program exactly len bytes, never across a page, and no others:
     * padding a page with the erase value costs over three times as long
     * on the BY25Q64ES (IF-05). With a prog unit of 1, alfs programs some bytes a second
     * time, with what they hold or with zero (docs/SPEC.md 4, 5). */
    int (*prog)(void *ctx, uint32_t addr, const void *buf, uint32_t len);
    /* Erase the sector at addr. ALFS_BUSY: the erase continues (a driver
     * that suspends it); alfs calls again with the same addr to resume. */
    int (*erase)(void *ctx, uint32_t addr);
    /* Optional, NULL if none: erase large_size bytes at an address aligned
     * to it, as erase does. */
    int (*erase_large)(void *ctx, uint32_t addr);
} alfs_driver_t;

typedef struct alfs_config {
    uint32_t region_start; /* sector-aligned, bytes from the chip's start */
    uint32_t region_size;  /* sector-aligned, at most 65,535 sectors      */
    uint32_t chip_size;    /* the region must lie inside it              */
    uint32_t sector_size;  /* the erase unit; a power of two, >= 4 pages */
    uint32_t page_size;    /* the most one program writes; a power of two */
    uint32_t prog_unit;    /* programmed at most once between erases     */
    uint32_t large_size;   /* erase_large's size, or 0                   */
    uint8_t erase_value;   /* 0xFF or 0x00                               */
    uint8_t max_page_programs; /* programs into one page; 0 means 16     */
    uint8_t max_prog_retries;  /* failed programs before a drop; 0: 3    */
} alfs_config_t;

typedef struct alfs_counters {
    uint32_t programs, erases;
    uint32_t prog_failures, erase_failures;
    uint32_t check_failures; /* chunks skipped on read: their check failed */
    uint32_t bytes_lost;     /* dropped after max_prog_retries failures    */
} alfs_counters_t;

#define ALFS_BAD_MAX 8

/* An instance. Opaque: its fields are alfs's. */
typedef struct alfs {
    const alfs_config_t *cfg;
    const alfs_driver_t *drv;
    uint32_t gen;      /* the region's format generation */
    uint32_t next_seq; /* the next claim's sequence */
    uint32_t min_wear, max_wear;
    uint32_t ewear;    /* the erase count the erase under way will stamp */
    alfs_counters_t n;
    uint16_t sectors, ready, dirty, nbad;
    uint16_t alloc_cursor, erase_cursor;
    uint16_t esector;  /* the sector, or a large block's first, being erased */
    uint16_t firmed;   /* the dead mark last programmed again */
    uint16_t fresh;    /* a ready sector not to use: where allocation stood */
    uint8_t estate;    /* the erase step's state */
    uint8_t estamped;  /* a large block: sectors stamped so far */
    uint16_t bad[ALFS_BAD_MAX]; /* sectors found bad this mount */
} alfs_t;

/* A stream's buffer: one page, and room for a stamp, a dead mark, a claim
 * and a chunk header ahead of it. */
#define ALFS_BUF_SIZE(page_size) ((page_size) + 53u)

/* An open stream. The caller supplies it and its buffer. Opaque. */
typedef struct alfs_stream {
    uint8_t *buf; /* append: the chunk being built; read: the chunk read */
    uint32_t id;  /* the stream's identity, unique in the region */
    uint32_t tag; /* the caller's */
    uint32_t seq; /* the current sector's claim sequence */
    uint16_t sector;
    uint16_t off;   /* where the next chunk is, in the sector */
    uint16_t used;  /* append: bytes buffered; read: bytes left in buf */
    uint16_t first; /* append: the first sector; read: the chunk's length */
    uint16_t index; /* the current sector's number in the stream */
    uint8_t mode;
    uint8_t page_progs;
    uint8_t fails;
    uint8_t flags;
} alfs_stream_t;

typedef struct alfs_info {
    uint32_t id;
    uint32_t tag;
} alfs_info_t;

typedef struct alfs_status {
    uint16_t sectors, ready, dirty, bad;
    uint32_t appendable; /* bytes that fit in ready sectors, about */
    uint32_t min_wear, max_wear;
    bool clean;          /* no dirty sector left, no erase under way */
    alfs_counters_t n;
} alfs_status_t;

/*
 * Claims a region for alfs, discarding whatever it held, and mounts it.
 * Erases and stamps the first sector: ALFS_OK when done, or ALFS_BUSY when
 * the driver's erase goes on, in which case alfs_erase_step finishes it.
 * The other sectors are dirty until erase steps clear them.
 * ALFS_EGEOMETRY, ALFS_EINVAL: nothing written. ALFS_EIO: retry.
 */
int alfs_format(alfs_t *fs, const alfs_config_t *cfg, const alfs_driver_t *drv);

/*
 * Mounts a region alfs formatted. Reads at most 60 bytes of each sector
 * and writes nothing. ALFS_ENOFS: rebuild or fail. ALFS_EGEOMETRY: fail.
 * ALFS_EIO: retry.
 */
int alfs_mount(alfs_t *fs, const alfs_config_t *cfg, const alfs_driver_t *drv);

/*
 * Makes one dirty sector ready: erases it, checks it is erased, and stamps
 * it with its erase count. One erase and one program at most.
 * ALFS_OK: a step was taken; call again while work remains. ALFS_BUSY: the
 * erase goes on; call again. ALFS_CLEAN: nothing is dirty. ALFS_EIO: retry.
 * A sector that fails is counted bad and skipped: ALFS_OK.
 */
int alfs_erase_step(alfs_t *fs);

/*
 * Starts a new stream with the caller's tag. Writes nothing until its
 * first chunk. buf: ALFS_BUF_SIZE(page_size) bytes, the stream's until it
 * is closed.
 */
int alfs_create(alfs_t *fs, alfs_stream_t *s, uint8_t *buf, uint32_t tag);

/*
 * Reopens a stream to append to it, after a reset. Its next chunk starts on
 * a page no interrupted program can have touched: two pages past its last
 * good chunk, after its claim is programmed again (prog unit 1), or in a new
 * sector. Reads the last sector's chunks; no flash operation. ALFS_ENOENT,
 * ALFS_EIO.
 */
int alfs_open_append(alfs_t *fs, alfs_stream_t *s, uint8_t *buf, uint32_t id);

/*
 * Opens a stream to read it from the start. buf: ALFS_BUF_SIZE(page_size)
 * bytes; each chunk is read into it and checked whole before any of it is
 * returned. ALFS_ENOENT, ALFS_EIO.
 */
int alfs_open_read(alfs_t *fs, alfs_stream_t *s, uint8_t *buf, uint32_t id);

/*
 * Appends up to len bytes and returns how many it took, >= 0. When the
 * chunk being built is full it is programmed first: one program at most.
 * It may take 0 when that program settled something instead (a resumed
 * stream's claim, a claim cleared ahead, the first ready sector after a
 * mount): call again. Errors, nothing taken: ALFS_ENOSPC, ALFS_EPROG,
 * ALFS_ELOST (see above).
 */
int32_t alfs_append(alfs_t *fs, alfs_stream_t *s, const void *data, uint32_t len);

/*
 * Makes every byte appended so far durable: programs the chunk being built.
 * One program at most. ALFS_OK; ALFS_BUSY when bytes remain: after a failed
 * program moved the stream to a new sector with less room in its first page,
 * or when the program settled something instead (see alfs_append). Call
 * again. Errors as alfs_append.
 */
int alfs_sync(alfs_t *fs, alfs_stream_t *s);

/* Syncs and releases the stream. On ALFS_BUSY or an error it stays open. */
int alfs_close(alfs_t *fs, alfs_stream_t *s);

/*
 * Reads up to len bytes and returns how many, 0 at the end. A chunk whose
 * check fails is skipped and counted. ALFS_EIO: nothing consumed; retry.
 */
int32_t alfs_read(alfs_t *fs, alfs_stream_t *s, void *buf, uint32_t len);

/*
 * Deletes a stream, atomically: one program. Its sectors become dirty.
 * ALFS_ENOENT, ALFS_EIO, ALFS_EPROG (nothing changed: retry).
 */
int alfs_delete(alfs_t *fs, uint32_t id);

/*
 * The stream with the smallest identity greater than after; 0 starts the
 * list. ALFS_ENOENT past the last. Identities grow with age.
 */
int alfs_next(alfs_t *fs, uint32_t after, alfs_info_t *info);

void alfs_status(const alfs_t *fs, alfs_status_t *st);

#endif
