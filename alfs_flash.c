/*
 * alfs_flash: see alfs_flash.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "alfs_flash.h"

#include <string.h>

uint32_t alfs_crc(uint32_t c, const void *p, uint32_t n) {
    static const uint32_t t[16] = {0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4,
                                   0x4DB26158, 0x5005713C, 0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
                                   0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C};
    const uint8_t *b = p;
    while (n--) {
        c ^= *b++;
        c = (c >> 4) ^ t[c & 15u];
        c = (c >> 4) ^ t[c & 15u];
    }
    return c;
}

uint32_t alfs_get16(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

uint32_t alfs_get32(const uint8_t *p) {
    return alfs_get16(p) | alfs_get16(p + 2) << 16;
}

void alfs_put16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

void alfs_put32(uint8_t *p, uint32_t v) {
    alfs_put16(p, v);
    alfs_put16(p + 2, v >> 16);
}

uint32_t alfs_lg2(uint32_t x) {
    uint32_t n = 0;
    while (x > 1u) {
        x >>= 1;
        n++;
    }
    return n;
}

bool alfs_is_erased(const uint8_t *p, uint32_t n) {
    while (n--)
        if (*p++ != ALFS_ERASED)
            return false;
    return true;
}

static bool pow2(uint32_t x) {
    return x && !(x & (x - 1u));
}

int alfs_check_area(const alfs_area_t *c, const alfs_driver_t *d) {
    if (!c || !d || !d->read || !d->prog || !d->erase)
        return ALFS_EINVAL;
    uint32_t s = c->sector_size, p = c->page_size;
    if (!pow2(s) || !pow2(p) || p < 128u || p > 256u * 16u || s < 4u * p || s > 32768u)
        return ALFS_EGEOMETRY;
    if (c->start % s || c->size % s || !c->size || c->size / s > 65535u || c->start > c->chip_size ||
        c->size > c->chip_size - c->start)
        return ALFS_EGEOMETRY;
    return ALFS_OK;
}

int alfs_dev_init(alfs_dev_t *d, const alfs_area_t *cfg, const alfs_driver_t *drv, uint8_t *scratch) {
    int rc = alfs_check_area(cfg, drv);
    if (rc || !scratch)
        return rc ? rc : ALFS_EINVAL;
    memset(d, 0, sizeof(*d));
    d->cfg = cfg;
    d->drv = drv;
    d->scratch = scratch;
    d->sectors = (uint16_t)(cfg->size / cfg->sector_size);
    return ALFS_OK;
}

uint32_t alfs_addr(const alfs_dev_t *d, uint32_t sector) {
    return d->cfg->start + sector * d->cfg->sector_size;
}

int alfs_read(alfs_dev_t *d, uint32_t addr, void *buf, uint32_t n) {
    return d->drv->read(d->drv->ctx, addr, buf, n) ? ALFS_EIO : ALFS_OK;
}

int alfs_blank(alfs_dev_t *d, uint32_t addr, uint32_t n, bool *blank) {
    uint8_t b[ALFS_PIECE];
    *blank = false;
    while (n) {
        uint32_t k = n < ALFS_PIECE ? n : ALFS_PIECE;
        if (alfs_read(d, addr, b, k))
            return ALFS_EIO;
        if (!alfs_is_erased(b, k))
            return ALFS_OK;
        addr += k;
        n -= k;
    }
    *blank = true;
    return ALFS_OK;
}

int alfs_prog(alfs_dev_t *d, uint32_t addr, const uint8_t *p, uint32_t n) {
    uint8_t b[ALFS_PIECE];
    d->n.programs++;
    if (d->drv->prog(d->drv->ctx, addr, p, n)) {
        d->n.prog_failures++;
        return ALFS_EPROG;
    }
    while (n) {
        uint32_t k = n < ALFS_PIECE ? n : ALFS_PIECE;
        if (alfs_read(d, addr, b, k))
            return ALFS_OK;
        if (memcmp(b, p, k)) {
            d->n.prog_failures++;
            return ALFS_EPROG;
        }
        addr += k;
        p += k;
        n -= k;
    }
    return ALFS_OK;
}

int alfs_wipe(alfs_dev_t *d, uint32_t sector) {
    uint32_t a = alfs_addr(d, sector);
    bool ok;
    d->n.erases++;
    if (d->drv->erase(d->drv->ctx, a)) {
        d->n.erase_failures++;
        return ALFS_EPROG;
    }
    if (alfs_blank(d, a, d->cfg->sector_size, &ok))
        return ALFS_EIO;
    if (!ok) {
        d->n.erase_failures++;
        return ALFS_EPROG;
    }
    return ALFS_OK;
}
