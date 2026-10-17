/*
 * alfs_rtos: one FreeRTOS task that performs every flash operation of a
 * log area and an object area, for any number of the application's tasks.
 *
 * Each call below makes a request to the flash task and blocks its caller
 * until the task has done it, except a log write whose bytes fit in the
 * writer's block without filling it, and a log read the block already
 * holds: those copy in the caller's task and send nothing
 * (docs/REQUIREMENTS.md 6.7). A log write returns when every byte it was
 * given is in flash or in the block.
 *
 * The flash task runs at the priority, and on the cores, the caller gives;
 * alfs changes no task's priority. A driver that must raise its priority
 * for an operation, as pyro's flash_op() does, raises it around that
 * operation only.
 *
 * Before the scheduler starts, a call runs directly in its caller. A call
 * from the flash task itself (a driver calling back) or from an interrupt,
 * and a call with a log handle another task opened, return ALFS_EINVAL.
 *
 * Memory: everything is the caller's, allocated statically. Each request
 * puts a request and a StaticSemaphore_t on the calling task's stack.
 * Needs configSUPPORT_STATIC_ALLOCATION and INCLUDE_xTaskGetSchedulerState.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ALFS_RTOS_H
#define ALFS_RTOS_H

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "alfs_log.h"
#include "alfs_obj.h"

typedef struct alfs_rt_mem {
    StackType_t *stack;     /* the flash task's stack */
    uint32_t stack_words;
    StaticTask_t *tcb;
    uint8_t *queue_storage; /* queue_len pointers' worth */
    UBaseType_t queue_len;  /* requests that may wait at once */
    StaticQueue_t *queue;
    UBaseType_t priority;
    UBaseType_t affinity;   /* a mask of cores, where the kernel has core
                             * affinity; 0 lets the task run on any */
} alfs_rt_mem_t;

/* The two areas, served. Opaque. */
typedef struct alfs_rt {
    alfs_log_t log;
    alfs_obj_t obj;
    TaskHandle_t task;
    QueueHandle_t queue;
    uint32_t requests; /* served since alfs_rt_start */
} alfs_rt_t;

/* A log writer or reader, and the task that opened it. Opaque but for
 * w.log. */
typedef struct alfs_rt_writer {
    alfs_writer_t w;
    TaskHandle_t owner;
} alfs_rt_writer_t;

typedef struct alfs_rt_reader {
    alfs_reader_t r;
    TaskHandle_t owner;
} alfs_rt_reader_t;

/* Creates the flash task and its queue. ALFS_EINVAL: memory missing. */
int alfs_rt_start(alfs_rt_t *rt, const alfs_rt_mem_t *mem);

/* The log area: as alfs_log.h, through the flash task. An erase of n steps
 * is n requests, so the flash task serves others between them (RT-07). */
int alfs_rt_log_mount(alfs_rt_t *rt, const alfs_area_t *cfg, const alfs_driver_t *drv, uint8_t *scratch);
int alfs_rt_log_erase(alfs_rt_t *rt, uint32_t log, uint32_t steps);
int alfs_rt_log_open(alfs_rt_t *rt, alfs_rt_writer_t *w, uint32_t log, uint8_t tag, uint8_t *block, uint32_t size);
int32_t alfs_rt_log_write(alfs_rt_t *rt, alfs_rt_writer_t *w, const void *data, uint32_t len);
int alfs_rt_log_sync(alfs_rt_t *rt, alfs_rt_writer_t *w);
int alfs_rt_log_close(alfs_rt_t *rt, alfs_rt_writer_t *w);
int alfs_rt_log_open_read(alfs_rt_t *rt, alfs_rt_reader_t *r, uint32_t log, uint8_t tag, uint8_t *block,
                          uint32_t size);
int32_t alfs_rt_log_read(alfs_rt_t *rt, alfs_rt_reader_t *r, void *out, uint32_t len);
int alfs_rt_log_close_read(alfs_rt_t *rt, alfs_rt_reader_t *r);
int alfs_rt_log_list(alfs_rt_t *rt, uint32_t after, alfs_log_info_t *info);
int alfs_rt_log_status(alfs_rt_t *rt, alfs_log_status_t *st);

/* The object area: as alfs_obj.h, through the flash task. */
int alfs_rt_obj_format(alfs_rt_t *rt, const alfs_area_t *cfg, const alfs_driver_t *drv, uint8_t *scratch);
int alfs_rt_obj_mount(alfs_rt_t *rt, const alfs_area_t *cfg, const alfs_driver_t *drv, uint8_t *scratch);
int alfs_rt_obj_erase(alfs_rt_t *rt, uint32_t steps);
int alfs_rt_obj_put(alfs_rt_t *rt, alfs_put_t *p, const char *name, uint32_t size);
int32_t alfs_rt_obj_write(alfs_rt_t *rt, alfs_put_t *p, const void *data, uint32_t len);
int alfs_rt_obj_commit(alfs_rt_t *rt, alfs_put_t *p);
int alfs_rt_obj_abort(alfs_rt_t *rt, alfs_put_t *p);
int alfs_rt_obj_delete(alfs_rt_t *rt, const char *name);
int alfs_rt_obj_open(alfs_rt_t *rt, alfs_get_t *g, const char *name);
int32_t alfs_rt_obj_read(alfs_rt_t *rt, alfs_get_t *g, void *out, uint32_t len);
int alfs_rt_obj_close(alfs_rt_t *rt, alfs_get_t *g);
int alfs_rt_obj_list(alfs_rt_t *rt, uint32_t index, alfs_obj_info_t *info);
int alfs_rt_obj_status(alfs_rt_t *rt, alfs_obj_status_t *st);

#endif
