/*
 * alfs_flash: what both stores need of the flash and of bytes: checked
 * reads, programs and erases, the CRC, little-endian fields, geometry.
 * Internal to alfs.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ALFS_FLASH_H
#define ALFS_FLASH_H

#include "alfs.h"

#define ALFS_VERSION 4u
#define ALFS_ERASED 0xFFu
#define ALFS_PIECE 16u /* the most read onto the stack at once */
#define ALFS_NONE 0xFFFFu

/* CRC-32 over n bytes, carried in c: start with ALFS_CRC_INIT, end with ~. */
#define ALFS_CRC_INIT 0xFFFFFFFFu
uint32_t alfs_crc(uint32_t c, const void *p, uint32_t n);

uint32_t alfs_get16(const uint8_t *p);
uint32_t alfs_get32(const uint8_t *p);
void alfs_put16(uint8_t *p, uint32_t v);
void alfs_put32(uint8_t *p, uint32_t v);
uint32_t alfs_lg2(uint32_t x);
bool alfs_is_erased(const uint8_t *p, uint32_t n);

/* ALFS_OK, or ALFS_EINVAL / ALFS_EGEOMETRY for an area alfs cannot use. */
int alfs_check_area(const alfs_area_t *c, const alfs_driver_t *d);

/* Binds a store to its area: ALFS_OK, or the error of alfs_check_area. */
int alfs_dev_init(alfs_dev_t *d, const alfs_area_t *cfg, const alfs_driver_t *drv, uint8_t *scratch);

uint32_t alfs_addr(const alfs_dev_t *d, uint32_t sector);

int alfs_read(alfs_dev_t *d, uint32_t addr, void *buf, uint32_t n);

/* *blank: every byte of [addr, addr + n) reads erased. */
int alfs_blank(alfs_dev_t *d, uint32_t addr, uint32_t n, bool *blank);

/* Programs, then reads back: a program that silently failed is a failure.
 * ALFS_OK, or ALFS_EPROG (counted). A read-back that cannot be read is not
 * wrong: the checks decide on read. */
int alfs_prog(alfs_dev_t *d, uint32_t addr, const uint8_t *p, uint32_t n);

/* Erases a sector and checks it erased. ALFS_OK, ALFS_EPROG (counted), or
 * ALFS_EIO. */
int alfs_wipe(alfs_dev_t *d, uint32_t sector);

#endif
