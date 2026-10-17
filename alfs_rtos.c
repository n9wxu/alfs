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

enum {
    LOG_MOUNT, LOG_ERASE, LOG_OPEN, LOG_WRITE, LOG_SYNC, LOG_CLOSE, LOG_OPEN_READ, LOG_READ, LOG_CLOSE_READ,
    LOG_LIST, LOG_STATUS, OBJ_FORMAT, OBJ_MOUNT, OBJ_ERASE, OBJ_PUT, OBJ_WRITE, OBJ_COMMIT, OBJ_ABORT,
    OBJ_DELETE, OBJ_OPEN, OBJ_READ, OBJ_CLOSE, OBJ_LIST, OBJ_STATUS
};

/* A request, on its caller's stack until the flash task gives done. */
typedef struct req {
    void *a;                  /* the area's instance */
    void *h;                  /* a handle */
    const void *in;           /* a row or bytes to write, a name, an area */
    void *out;                /* bytes read, a block, a scratch page, a listing */
    const alfs_driver_t *drv;
    uint32_t n, v;            /* a length or a size; a log, or an index */
    int32_t rc;
    uint8_t op, tag;
    SemaphoreHandle_t done;
} req_t;

/* Direct calls only, so the stack check (SIMP-04) follows every chain. */
static int32_t run(req_t *q) {
    alfs_log_t *lg = q->a;
    alfs_obj_t *o = q->a;
    switch (q->op) {
    case LOG_MOUNT:
        return alfs_log_mount(lg, q->in, q->drv, q->out);
    case LOG_ERASE:
        return alfs_log_erase(lg, q->v, 1);
    case LOG_OPEN:
        return alfs_log_open(lg, q->h, q->v, q->tag, q->out, q->n);
    case LOG_WRITE:
        return alfs_log_write(lg, q->h, q->in, q->n);
    case LOG_SYNC:
        return alfs_log_sync(lg, q->h);
    case LOG_CLOSE:
        return alfs_log_close(lg, q->h);
    case LOG_OPEN_READ:
        return alfs_log_open_read(lg, q->h, q->v, q->tag, q->out, q->n);
    case LOG_READ:
        return alfs_log_read(lg, q->h, q->out, q->n);
    case LOG_CLOSE_READ:
        return alfs_log_close_read(lg, q->h);
    case LOG_LIST:
        return alfs_log_list(lg, q->v, q->out);
    case LOG_STATUS:
        alfs_log_status(lg, q->out);
        return ALFS_OK;
    case OBJ_FORMAT:
        return alfs_obj_format(o, q->in, q->drv, q->out);
    case OBJ_MOUNT:
        return alfs_obj_mount(o, q->in, q->drv, q->out);
    case OBJ_ERASE:
        return alfs_obj_erase(o, 1);
    case OBJ_PUT:
        return alfs_obj_put(o, q->h, q->in, q->n);
    case OBJ_WRITE:
        return alfs_obj_write(o, q->h, q->in, q->n);
    case OBJ_COMMIT:
        return alfs_obj_commit(o, q->h);
    case OBJ_ABORT:
        return alfs_obj_abort(o, q->h);
    case OBJ_DELETE:
        return alfs_obj_delete(o, q->in);
    case OBJ_OPEN:
        return alfs_obj_open(o, q->h, q->in);
    case OBJ_READ:
        return alfs_obj_read(o, q->h, q->out, q->n);
    case OBJ_CLOSE:
        return alfs_obj_close(o, q->h);
    case OBJ_LIST:
        return alfs_obj_list(o, q->v, q->out);
    default:
        alfs_obj_status(o, q->out);
        return ALFS_OK;
    }
}

static void task_fn(void *arg) {
    alfs_rt_t *rt = arg;
    for (;;) {
        req_t *q;
        if (xQueueReceive(rt->queue, &q, portMAX_DELAY) != pdTRUE)
            continue;
        q->rc = run(q);
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
    if (!rt || !q->a || ALFS_RT_IN_ISR() || (t && t == rt->task))
        return ALFS_EINVAL;
    if (!t)
        return run(q);
    q->done = xSemaphoreCreateBinaryStatic(&done);
    xQueueSend(rt->queue, &q, portMAX_DELAY);
    xSemaphoreTake(q->done, portMAX_DELAY);
    return q->rc;
}

static int32_t ask(alfs_rt_t *rt, uint8_t op, void *a, void *h, const void *in, void *out, uint32_t n, uint32_t v) {
    req_t q = {.op = op, .a = a, .h = h, .in = in, .out = out, .n = n, .v = v};
    return submit(rt, &q);
}

/* format and mount, which also pass a driver. */
static int area(alfs_rt_t *rt, uint8_t op, void *a, const alfs_area_t *cfg, const alfs_driver_t *drv,
                uint8_t *scratch) {
    req_t q = {.op = op, .a = a, .in = cfg, .out = scratch, .drv = drv};
    return (int)submit(rt, &q);
}

/* An erase of steps steps, one request each (RT-07). */
static int erase(alfs_rt_t *rt, uint8_t op, void *a, uint32_t log, uint32_t steps) {
    int rc = ALFS_OK;
    for (uint32_t i = 0; i < steps && rc == ALFS_OK; i++)
        rc = (int)ask(rt, op, a, NULL, NULL, NULL, 0, log);
    return rc;
}

/* A log handle's open, which also passes a tag. */
static int open_handle(alfs_rt_t *rt, uint8_t op, alfs_log_t *lg, void *h, uint32_t log, uint8_t tag,
                       uint8_t *block, uint32_t size) {
    req_t q = {.op = op, .a = lg, .h = h, .out = block, .n = size, .v = log, .tag = tag};
    return (int)submit(rt, &q);
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

/* ---- a log area ------------------------------------------------------- */

int alfs_rt_log_mount(alfs_rt_t *rt, alfs_log_t *lg, const alfs_area_t *cfg, const alfs_driver_t *drv,
                      uint8_t *scratch) {
    return area(rt, LOG_MOUNT, lg, cfg, drv, scratch);
}

int alfs_rt_log_erase(alfs_rt_t *rt, alfs_log_t *lg, uint32_t log, uint32_t steps) {
    return erase(rt, LOG_ERASE, lg, log, steps);
}

int alfs_rt_log_open(alfs_rt_t *rt, alfs_log_t *lg, alfs_rt_writer_t *w, uint32_t log, uint8_t tag, uint8_t *block,
                     uint32_t size) {
    int rc = w ? open_handle(rt, LOG_OPEN, lg, &w->w, log, tag, block, size) : ALFS_EINVAL;
    if (!rc)
        w->owner = me();
    return rc;
}

int alfs_rt_log_open_read(alfs_rt_t *rt, alfs_log_t *lg, alfs_rt_reader_t *r, uint32_t log, uint8_t tag,
                          uint8_t *block, uint32_t size) {
    int rc = r ? open_handle(rt, LOG_OPEN_READ, lg, &r->r, log, tag, block, size) : ALFS_EINVAL;
    if (!rc)
        r->owner = me();
    return rc;
}

/* RT-06: a log handle is used only by the task that opened it, since its
 * block is filled and drained in that task (RT-03). */
static bool wmine(const alfs_rt_writer_t *w) {
    return w && w->owner == me();
}

static bool rmine(const alfs_rt_reader_t *r) {
    return r && r->owner == me();
}

/* RT-03: a row that fits in the block goes there in the caller's task. */
int alfs_rt_log_write(alfs_rt_t *rt, alfs_log_t *lg, alfs_rt_writer_t *w, const void *row, uint32_t len) {
    if (!wmine(w) || !row)
        return ALFS_EINVAL;
    if (alfs_log_take(&w->w, row, len))
        return ALFS_OK;
    return (int)ask(rt, LOG_WRITE, lg, &w->w, row, NULL, len, 0);
}

int alfs_rt_log_sync(alfs_rt_t *rt, alfs_log_t *lg, alfs_rt_writer_t *w) {
    return wmine(w) ? (int)ask(rt, LOG_SYNC, lg, &w->w, NULL, NULL, 0, 0) : ALFS_EINVAL;
}

int alfs_rt_log_close(alfs_rt_t *rt, alfs_log_t *lg, alfs_rt_writer_t *w) {
    return wmine(w) ? (int)ask(rt, LOG_CLOSE, lg, &w->w, NULL, NULL, 0, 0) : ALFS_EINVAL;
}

/* RT-03: a row the block holds is read in the caller's task. */
int32_t alfs_rt_log_read(alfs_rt_t *rt, alfs_log_t *lg, alfs_rt_reader_t *r, void *out, uint32_t cap) {
    if (!rmine(r) || !out)
        return ALFS_EINVAL;
    int32_t n = alfs_log_give(&r->r, out, cap);
    return n ? n : ask(rt, LOG_READ, lg, &r->r, NULL, out, cap, 0);
}

int alfs_rt_log_close_read(alfs_rt_t *rt, alfs_log_t *lg, alfs_rt_reader_t *r) {
    return rmine(r) ? (int)ask(rt, LOG_CLOSE_READ, lg, &r->r, NULL, NULL, 0, 0) : ALFS_EINVAL;
}

int alfs_rt_log_list(alfs_rt_t *rt, alfs_log_t *lg, uint32_t after, alfs_log_info_t *info) {
    return info ? (int)ask(rt, LOG_LIST, lg, NULL, NULL, info, 0, after) : ALFS_EINVAL;
}

int alfs_rt_log_status(alfs_rt_t *rt, alfs_log_t *lg, alfs_log_status_t *st) {
    return st ? (int)ask(rt, LOG_STATUS, lg, NULL, NULL, st, 0, 0) : ALFS_EINVAL;
}

/* ---- the object area --------------------------------------------------- */

int alfs_rt_obj_format(alfs_rt_t *rt, alfs_obj_t *o, const alfs_area_t *cfg, const alfs_driver_t *drv,
                       uint8_t *scratch) {
    return area(rt, OBJ_FORMAT, o, cfg, drv, scratch);
}

int alfs_rt_obj_mount(alfs_rt_t *rt, alfs_obj_t *o, const alfs_area_t *cfg, const alfs_driver_t *drv,
                      uint8_t *scratch) {
    return area(rt, OBJ_MOUNT, o, cfg, drv, scratch);
}

int alfs_rt_obj_erase(alfs_rt_t *rt, alfs_obj_t *o, uint32_t steps) {
    return erase(rt, OBJ_ERASE, o, 0, steps);
}

int alfs_rt_obj_put(alfs_rt_t *rt, alfs_obj_t *o, alfs_put_t *p, const char *name, uint32_t size) {
    return (int)ask(rt, OBJ_PUT, o, p, name, NULL, size, 0);
}

int32_t alfs_rt_obj_write(alfs_rt_t *rt, alfs_obj_t *o, alfs_put_t *p, const void *data, uint32_t len) {
    return ask(rt, OBJ_WRITE, o, p, data, NULL, len, 0);
}

int alfs_rt_obj_commit(alfs_rt_t *rt, alfs_obj_t *o, alfs_put_t *p) {
    return (int)ask(rt, OBJ_COMMIT, o, p, NULL, NULL, 0, 0);
}

int alfs_rt_obj_abort(alfs_rt_t *rt, alfs_obj_t *o, alfs_put_t *p) {
    return (int)ask(rt, OBJ_ABORT, o, p, NULL, NULL, 0, 0);
}

int alfs_rt_obj_delete(alfs_rt_t *rt, alfs_obj_t *o, const char *name) {
    return (int)ask(rt, OBJ_DELETE, o, NULL, name, NULL, 0, 0);
}

int alfs_rt_obj_open(alfs_rt_t *rt, alfs_obj_t *o, alfs_get_t *g, const char *name) {
    return (int)ask(rt, OBJ_OPEN, o, g, name, NULL, 0, 0);
}

int32_t alfs_rt_obj_read(alfs_rt_t *rt, alfs_obj_t *o, alfs_get_t *g, void *out, uint32_t len) {
    return ask(rt, OBJ_READ, o, g, NULL, out, len, 0);
}

int alfs_rt_obj_close(alfs_rt_t *rt, alfs_obj_t *o, alfs_get_t *g) {
    return (int)ask(rt, OBJ_CLOSE, o, g, NULL, NULL, 0, 0);
}

int alfs_rt_obj_list(alfs_rt_t *rt, alfs_obj_t *o, uint32_t index, alfs_obj_info_t *info) {
    return info ? (int)ask(rt, OBJ_LIST, o, NULL, NULL, info, 0, index) : ALFS_EINVAL;
}

int alfs_rt_obj_status(alfs_rt_t *rt, alfs_obj_t *o, alfs_obj_status_t *st) {
    return st ? (int)ask(rt, OBJ_STATUS, o, NULL, NULL, st, 0, 0) : ALFS_EINVAL;
}
