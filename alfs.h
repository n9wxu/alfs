/*
 * alfs: the append-log flash store.
 *
 * Named, versioned files on serial NOR flash, written once from their
 * start, for a caller that runs code from the same chip and cannot stall.
 * Nothing erases in the write path: a write programs only sectors erased
 * ahead of time, and the caller asks for erases when a stall does no harm.
 *
 * docs/REQUIREMENTS.md says what alfs must do; docs/SPEC.md how its bytes
 * lie on the flash. alfs_rtos.h serves an instance from one FreeRTOS task.
 *
 * Each call returns when its flash work is done; no program crosses a page.
 * alfs uses no memory but what the caller passes in, and an instance starts
 * zeroed (static storage, or memset). The caller serialises calls on one
 * instance.
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
    /* alfs_erase: no dirty sector is left. */
    ALFS_CLEAN = 2,
    /* A driver read failed. Nothing changed: retry. */
    ALFS_EIO = -1,
    /* No sector is ready for the file's next bytes. They are kept in its
     * block: erase (alfs_erase) or delete a file, then retry. */
    ALFS_ENOSPC = -2,
    /* The region holds no alfs: rebuild with alfs_format, or fail. */
    ALFS_ENOFS = -3,
    /* The configuration does not match the region, or the region does not
     * fit the chip. Nothing was written: fail. */
    ALFS_EGEOMETRY = -4,
    /* An argument or the handle's state is wrong. Nothing changed: fail
     * (it is a bug in the caller). */
    ALFS_EINVAL = -5,
    /* No file has that name and version. */
    ALFS_ENOENT = -6,
    /* A program failed, and so did its retries in fresh sectors. Nothing
     * the caller passed was lost: retry later, or fail. */
    ALFS_EPROG = -7,
    /* Bytes failed max_prog_retries programs in a row and were dropped,
     * and counted in bytes_lost. The file goes on: continue. */
    ALFS_ELOST = -8,
    /* The file is open (delete), or a file is open (mount, format): close
     * it, then retry. */
    ALFS_EOPEN = -9,
};

#define ALFS_NAME_MAX 31             /* bytes in a name, none 00h */
#define ALFS_SIZE_UNKNOWN 0xFFFFFFFFu /* an unclosed file's size */

/*
 * The flash. Addresses are bytes from the start of the chip. Each returns
 * 0 when its work is done, or a negative value on a fault of the flash. A
 * driver that must wait for the system to allow an operation waits; a read
 * sees every program and erase before it.
 */
typedef struct alfs_driver {
    void *ctx;
    int (*read)(void *ctx, uint32_t addr, void *buf, uint32_t len);
    /* Program exactly len bytes, never across a page, and no others:
     * padding a page with the erase value costs over three times as long
     * on the BY25Q64ES (IF-05). alfs programs some bytes a second time,
     * with what they hold or with zero (docs/SPEC.md 4 to 6). */
    int (*prog)(void *ctx, uint32_t addr, const void *buf, uint32_t len);
    /* Erase the sector at addr. A driver that slices an erase does so
     * inside this call. */
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
    uint32_t page_size;    /* the most one program writes; a power of two,
                            * >= 128 */
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

typedef struct alfs_file alfs_file_t;

/* An instance. Opaque: its fields are alfs's. */
typedef struct alfs {
    const alfs_config_t *cfg;
    const alfs_driver_t *drv;
    uint8_t *scratch;  /* one page, the caller's: where each program is built */
    alfs_file_t *open; /* the open files, linked through their handles */
    uint32_t gen;      /* the region's format generation */
    uint32_t next_seq; /* the next claim's sequence */
    uint32_t min_wear, max_wear;
    uint32_t cfirmed;  /* the file whose close record was programmed again */
    alfs_counters_t n;
    uint16_t sectors, ready, dirty, nbad;
    uint16_t alloc_cursor, erase_cursor;
    uint16_t firmed;   /* the dead mark last programmed again */
    uint16_t fresh;    /* a ready sector not to use: where allocation stood */
    uint16_t bad[ALFS_BAD_MAX]; /* sectors found bad this mount */
    uint8_t no_large;  /* large erases failed this mount */
} alfs_t;

/*
 * An open file. The caller supplies it and its block, both the file's until
 * it is closed. Opaque but for `version`, which a caller may read.
 */
struct alfs_file {
    alfs_file_t *next;
    uint8_t *block;
    uint32_t version; /* the file's identity, unique for the life of the region */
    uint32_t size;    /* writing: bytes written so far */
    uint16_t cap;     /* the block's size */
    uint16_t used;    /* writing: bytes in the block; reading: bytes left there */
    uint16_t pos;     /* reading: where those bytes start */
    uint16_t sector, first, index, off;
    uint8_t mode, page_progs, fails, flags;
};

typedef struct alfs_info {
    char name[ALFS_NAME_MAX + 1]; /* empty for a create a power loss tore */
    uint32_t version;
    uint32_t size;                /* ALFS_SIZE_UNKNOWN until closed */
    bool closed;
} alfs_info_t;

typedef struct alfs_status {
    uint16_t sectors, ready, dirty, bad;
    uint32_t writable;   /* bytes that fit in ready sectors, about */
    uint32_t min_wear, max_wear;
    bool clean;          /* no dirty sector left */
    alfs_counters_t n;
} alfs_status_t;

/*
 * Claims a region for alfs, discarding whatever it held, and mounts it.
 * Erases and stamps the first sector; the other sectors are dirty until
 * alfs_erase clears them. scratch: one page, the instance's from now on.
 * ALFS_EGEOMETRY, ALFS_EINVAL: nothing written. ALFS_EOPEN: a file is open.
 * ALFS_EIO: retry.
 */
int alfs_format(alfs_t *fs, const alfs_config_t *cfg, const alfs_driver_t *drv, uint8_t *scratch);

/*
 * Mounts a region alfs formatted. Reads at most 60 bytes of each sector
 * and writes nothing. ALFS_ENOFS: rebuild or fail. ALFS_EGEOMETRY: fail.
 * ALFS_EOPEN: a file is open. ALFS_EIO: retry.
 */
int alfs_mount(alfs_t *fs, const alfs_config_t *cfg, const alfs_driver_t *drv, uint8_t *scratch);

/*
 * Makes up to `steps` dirty sectors (or large blocks) ready: erases each,
 * checks it is erased, and stamps it with its erase count. ALFS_OK: dirty
 * sectors remain. ALFS_CLEAN: none is left. ALFS_EIO: retry. A sector that
 * fails is counted bad and skipped.
 */
int alfs_erase(alfs_t *fs, uint32_t steps);

/*
 * Creates a new version of the file called name (1 to ALFS_NAME_MAX bytes)
 * and opens it for writing: one program, so an empty file exists. Its
 * version is f->version. block: at least one page and at most 65,535
 * bytes. ALFS_EINVAL, ALFS_ENOSPC, ALFS_EPROG, ALFS_EIO.
 */
int alfs_create(alfs_t *fs, alfs_file_t *f, const char *name, uint8_t *block, uint32_t size);

/*
 * Opens a file to read it from the start: the version given, closed or not,
 * or with version 0 the newest closed version of name. name may be NULL
 * when a version is given. block: as alfs_create's; each chunk is read into
 * it and checked whole before any of it is returned. ALFS_ENOENT, ALFS_EIO.
 */
int alfs_open_read(alfs_t *fs, alfs_file_t *f, const char *name, uint32_t version, uint8_t *block,
                   uint32_t size);

/*
 * Writes len bytes: copies them into the block and, each time it fills,
 * programs all of it but less than a page's room. Returns when every byte
 * is in flash or in the block: len, or fewer when a program failed, after
 * which the next call returns the error. Errors, nothing taken:
 * ALFS_EINVAL, ALFS_ENOSPC, ALFS_EPROG, ALFS_ELOST (see above).
 */
int32_t alfs_write(alfs_t *fs, alfs_file_t *f, const void *data, uint32_t len);

/* Makes every byte written so far durable. ALFS_OK, or an error as
 * alfs_write's. */
int alfs_sync(alfs_t *fs, alfs_file_t *f);

/*
 * Releases the file. A file being written is synced and closed: its size
 * is recorded, and it takes no more bytes. When the sync fails it stays
 * open, its bytes in its block: retry. ALFS_EPROG: its close record would
 * not program, and it is released unclosed, every byte synced readable.
 */
int alfs_close(alfs_t *fs, alfs_file_t *f);

/*
 * Reads up to len bytes and returns how many, 0 at the end. A chunk whose
 * check fails is skipped and counted. ALFS_EIO: nothing consumed; retry.
 */
int32_t alfs_read(alfs_t *fs, alfs_file_t *f, void *out, uint32_t len);

/*
 * Deletes one version, atomically: one program, after the close record of
 * the newest closed version of the same name above it is programmed again.
 * Its sectors become dirty. name may be NULL. ALFS_ENOENT, ALFS_EOPEN,
 * ALFS_EIO, ALFS_EPROG (nothing changed: retry).
 */
int alfs_delete(alfs_t *fs, const char *name, uint32_t version);

/*
 * The file with the smallest version greater than after; 0 starts the
 * list. ALFS_ENOENT past the last. Versions grow with age.
 */
int alfs_list(alfs_t *fs, uint32_t after, alfs_info_t *info);

void alfs_status(const alfs_t *fs, alfs_status_t *st);

/*
 * RAM only, for a caller that serves alfs from another task (alfs_rtos.h).
 * alfs_take copies into a file being written as much of data as its block
 * has room for, and returns how much; alfs_write takes the rest. alfs_give
 * copies out of a file being read what its block holds, up to len.
 */
uint32_t alfs_take(alfs_file_t *f, const void *data, uint32_t len);
uint32_t alfs_give(alfs_file_t *f, void *out, uint32_t len);

#endif
