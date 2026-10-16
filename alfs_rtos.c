/*
 * alfs_rtos: the flash task. See alfs_rtos.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "alfs_rtos.h"

#include "semphr.h"

#include <string.h>

#ifndef ALFS_RT_IN_ISR
#if defined(portCHECK_IF_IN_ISR)
#define ALFS_RT_IN_ISR() portCHECK_IF_IN_ISR()
#else
#define ALFS_RT_IN_ISR() 0
#endif
#endif

enum { OP_FORMAT, OP_MOUNT, OP_ERASE, OP_CREATE, OP_OPEN, OP_WRITE, OP_READ, OP_SYNC, OP_CLOSE, OP_DELETE, OP_LIST, OP_STATUS };

/* A request, on its caller's stack until the flash task gives done. */
typedef struct req {
    alfs_file_t *f;
    const void *in;           /* bytes to write, a name, a configuration */
    void *out;                /* bytes read, a block, a scratch page, a listing */
    const alfs_driver_t *drv;
    uint32_t n, v;            /* a length or a size; a version or erase steps */
    int32_t rc;
    uint8_t op;
    SemaphoreHandle_t done;
} req_t;

static int32_t run(alfs_t *fs, req_t *q) {
    switch (q->op) {
    case OP_FORMAT:
        return alfs_format(fs, q->in, q->drv, q->out);
    case OP_MOUNT:
        return alfs_mount(fs, q->in, q->drv, q->out);
    case OP_ERASE:
        return alfs_erase(fs, q->v);
    case OP_CREATE:
        return alfs_create(fs, q->f, q->in, q->out, q->n);
    case OP_OPEN:
        return alfs_open_read(fs, q->f, q->in, q->v, q->out, q->n);
    case OP_WRITE:
        return alfs_write(fs, q->f, q->in, q->n);
    case OP_READ:
        return alfs_read(fs, q->f, q->out, q->n);
    case OP_SYNC:
        return alfs_sync(fs, q->f);
    case OP_CLOSE:
        return alfs_close(fs, q->f);
    case OP_DELETE:
        return alfs_delete(fs, q->in, q->v);
    case OP_LIST:
        return alfs_list(fs, q->v, q->out);
    default:
        alfs_status(fs, q->out);
        return ALFS_OK;
    }
}

static void task_fn(void *arg) {
    alfs_rt_t *rt = arg;
    for (;;) {
        req_t *q;
        if (xQueueReceive(rt->queue, &q, portMAX_DELAY) != pdTRUE)
            continue;
        q->rc = run(&rt->fs, q);
        rt->requests++;
        xSemaphoreGive(q->done);
    }
}

/* The calling task, or NULL before the scheduler starts. */
static TaskHandle_t me(void) {
    return xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED ? NULL : xTaskGetCurrentTaskHandle();
}

/* RT-06: a request runs directly before the scheduler starts, and is
 * refused from an interrupt or from the flash task, which would wait on
 * itself. */
static int32_t submit(alfs_rt_t *rt, req_t *q) {
    StaticSemaphore_t done;
    TaskHandle_t t = me();
    if (!rt || ALFS_RT_IN_ISR() || (t && t == rt->task))
        return ALFS_EINVAL;
    if (!t)
        return run(&rt->fs, q);
    q->done = xSemaphoreCreateBinaryStatic(&done);
    xQueueSend(rt->queue, &q, portMAX_DELAY);
    xSemaphoreTake(q->done, portMAX_DELAY);
    return q->rc;
}

static int32_t ask(alfs_rt_t *rt, uint8_t op, alfs_file_t *f, const void *in, void *out, uint32_t n,
                   uint32_t v) {
    req_t q = {f, in, out, NULL, n, v, 0, op, NULL};
    return submit(rt, &q);
}

/* format and mount, which also pass a driver. */
static int region(alfs_rt_t *rt, uint8_t op, const alfs_config_t *cfg, const alfs_driver_t *drv,
                  uint8_t *scratch) {
    req_t q = {NULL, cfg, scratch, drv, 0, 0, 0, op, NULL};
    return (int)submit(rt, &q);
}

/* A file the calling task opened. */
static bool mine(const alfs_rt_file_t *f) {
    return f && f->owner == me();
}

int alfs_rt_start(alfs_rt_t *rt, const alfs_rt_mem_t *m) {
    if (!rt || !m || !m->stack || !m->tcb || !m->queue_storage || !m->queue || !m->queue_len)
        return ALFS_EINVAL;
    memset(rt, 0, sizeof(*rt));
    rt->queue = xQueueCreateStatic(m->queue_len, sizeof(req_t *), m->queue_storage, m->queue);
#if (configUSE_CORE_AFFINITY == 1) && (configNUMBER_OF_CORES > 1)
    rt->task = xTaskCreateStaticAffinitySet(task_fn, "alfs", m->stack_words, rt, m->priority, m->stack, m->tcb,
                                            m->affinity ? m->affinity : tskNO_AFFINITY);
#else
    rt->task = xTaskCreateStatic(task_fn, "alfs", m->stack_words, rt, m->priority, m->stack, m->tcb);
#endif
    return rt->queue && rt->task ? ALFS_OK : ALFS_EINVAL;
}

int alfs_rt_format(alfs_rt_t *rt, const alfs_config_t *cfg, const alfs_driver_t *drv, uint8_t *scratch) {
    return region(rt, OP_FORMAT, cfg, drv, scratch);
}

int alfs_rt_mount(alfs_rt_t *rt, const alfs_config_t *cfg, const alfs_driver_t *drv, uint8_t *scratch) {
    return region(rt, OP_MOUNT, cfg, drv, scratch);
}

int alfs_rt_erase(alfs_rt_t *rt, uint32_t steps) {
    int rc = (int)ask(rt, OP_ERASE, NULL, NULL, NULL, 0, steps ? 1u : 0u);
    for (uint32_t i = 1; i < steps && rc == ALFS_OK; i++)
        rc = (int)ask(rt, OP_ERASE, NULL, NULL, NULL, 0, 1);
    return rc;
}

int alfs_rt_create(alfs_rt_t *rt, alfs_rt_file_t *f, const char *name, uint8_t *block, uint32_t size) {
    int rc = f ? (int)ask(rt, OP_CREATE, &f->f, name, block, size, 0) : ALFS_EINVAL;
    if (!rc)
        f->owner = me();
    return rc;
}

int alfs_rt_open_read(alfs_rt_t *rt, alfs_rt_file_t *f, const char *name, uint32_t version, uint8_t *block,
                      uint32_t size) {
    int rc = f ? (int)ask(rt, OP_OPEN, &f->f, name, block, size, version) : ALFS_EINVAL;
    if (!rc)
        f->owner = me();
    return rc;
}

/* RT-03: bytes that fit in the block without filling it go there in the
 * caller's task; the flash task takes the rest. */
int32_t alfs_rt_write(alfs_rt_t *rt, alfs_rt_file_t *f, const void *data, uint32_t len) {
    if (!mine(f) || (len && !data))
        return ALFS_EINVAL;
    uint32_t n = alfs_take(&f->f, data, len);
    if (len && n == len && f->f.used < f->f.cap)
        return (int32_t)len;
    int32_t m = ask(rt, OP_WRITE, &f->f, (const uint8_t *)data + n, NULL, len - n, 0);
    return m < 0 ? (n ? (int32_t)n : m) : (int32_t)(n + (uint32_t)m);
}

/* RT-03: what the block holds is read in the caller's task. */
int32_t alfs_rt_read(alfs_rt_t *rt, alfs_rt_file_t *f, void *out, uint32_t len) {
    if (!mine(f) || (len && !out))
        return ALFS_EINVAL;
    uint32_t n = alfs_give(&f->f, out, len);
    return n ? (int32_t)n : ask(rt, OP_READ, &f->f, NULL, out, len, 0);
}

int alfs_rt_sync(alfs_rt_t *rt, alfs_rt_file_t *f) {
    return mine(f) ? (int)ask(rt, OP_SYNC, &f->f, NULL, NULL, 0, 0) : ALFS_EINVAL;
}

int alfs_rt_close(alfs_rt_t *rt, alfs_rt_file_t *f) {
    return mine(f) ? (int)ask(rt, OP_CLOSE, &f->f, NULL, NULL, 0, 0) : ALFS_EINVAL;
}

int alfs_rt_delete(alfs_rt_t *rt, const char *name, uint32_t version) {
    return (int)ask(rt, OP_DELETE, NULL, name, NULL, 0, version);
}

int alfs_rt_list(alfs_rt_t *rt, uint32_t after, alfs_info_t *info) {
    return info ? (int)ask(rt, OP_LIST, NULL, NULL, info, 0, after) : ALFS_EINVAL;
}

int alfs_rt_status(alfs_rt_t *rt, alfs_status_t *st) {
    return st ? (int)ask(rt, OP_STATUS, NULL, NULL, st, 0, 0) : ALFS_EINVAL;
}
