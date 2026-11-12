/*
 * alfs: the append-log flash store, on serial NOR flash: files, each a
 * frame of pages linked in a list, found by filename through a cache a
 * mount builds (alfs_fs.h). A file's pages can be reserved ahead of time, so that
 * writing it programs only flash erased already.
 *
 * docs/REQUIREMENTS.md says what alfs must do; docs/SPEC.md how its bytes
 * lie on the flash. Each call returns when its flash work is done; no
 * program crosses a page. alfs uses no memory but what the caller passes
 * in, and an instance starts zeroed (static storage, or memset). The
 * caller serialises calls on one instance.
 *
 * This header holds the types the store and its driver share.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ALFS_H
#define ALFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Return codes. Each error says the state it leaves and what to do: retry,
 * clean up, or fail.
 */
enum {
    ALFS_OK = 0,
    /* A cleanup: nothing is left to erase. */
    ALFS_CLEAN = 2,
    /* A driver read failed. Nothing changed: retry. */
    ALFS_EIO = -1,
    /* No room: no free page for a reservation, or for a write past one,
     * whose bytes stay in the block (clean up, then retry); or no free
     * record in the cache (delete a file, or fail). */
    ALFS_ENOSPC = -2,
    /* The configuration does not fit the chip. Nothing was written: fail. */
    ALFS_EGEOMETRY = -4,
    /* An argument or a handle's state is wrong. Nothing changed: fail (it
     * is a bug in the caller). */
    ALFS_EINVAL = -5,
    /* No file of that filename. */
    ALFS_ENOENT = -6,
    /* An erase or program failed where alfs could not go around it: the
     * call is abandoned and every file is as it was. Retry later, or fail. */
    ALFS_EPROG = -7,
    /* A file is open that the call would disturb: close it, then retry. */
    ALFS_EOPEN = -8,
    /* A closed file read to its end lacked a page: it is damaged. Write it
     * again, or fail. */
    ALFS_ECHECK = -9,
    /* A file of that filename exists, or is closed and so final: delete it
     * first, or fail. */
    ALFS_EEXIST = -10,
};

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
     * on the BY25Q64ES (IF-04). alfs programs some bytes a second time,
     * with zero: a link a close ends, a filename a delete blocks out
     * (docs/SPEC.md 5 and 7). */
    int (*prog)(void *ctx, uint32_t addr, const void *buf, uint32_t len);
    /* Erase the sector at addr. A driver that slices an erase does so
     * inside this call. */
    int (*erase)(void *ctx, uint32_t addr);
} alfs_driver_t;

/* An area of the chip. The erase value is 0xFF. */
typedef struct alfs_area {
    uint32_t start;       /* sector-aligned, bytes from the chip's start */
    uint32_t size;        /* sector-aligned, at most 65,535 pages        */
    uint32_t chip_size;   /* the area must lie inside it                */
    uint32_t sector_size; /* the erase unit; a power of two, >= 4 pages */
    uint32_t page_size;   /* the most one program writes; a power of two,
                           * >= 128 */
} alfs_area_t;

typedef struct alfs_counters {
    uint32_t programs, erases;
    uint32_t prog_failures, erase_failures;
    uint32_t check_failures; /* pages skipped, links not followed, files
                              * refused: their check failed */
    uint32_t bytes_lost;     /* file bytes dropped with a failed program */
} alfs_counters_t;

#define ALFS_NAME_MAX 31 /* bytes in a filename, none 00h */
#define ALFS_BAD_MAX 8   /* bad sectors remembered in a mount */

/* What the store keeps of its area. Opaque. */
typedef struct alfs_dev {
    const alfs_area_t *cfg;
    const alfs_driver_t *drv;
    uint8_t *scratch; /* one page, the caller's: where each program is built */
    alfs_counters_t n;
    uint16_t sectors;
} alfs_dev_t;

#endif
