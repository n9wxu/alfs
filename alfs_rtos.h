/*
 * alfs_rtos: one FreeRTOS task that performs every flash operation of an
 * alfs instance, for any number of the application's tasks.
 *
 * Each call below makes a request to the flash task and blocks its caller
 * until the task has done it, except a write whose bytes fit in the file's
 * block without filling it, and a read the block already holds: those copy
 * in the caller's task and send nothing (docs/REQUIREMENTS.md 6.7). A write
 * returns when every byte it was given is in flash or in the block.
 *
 * The flash task runs at the priority, and on the cores, the caller gives;
 * alfs changes no task's priority. A driver that must raise its priority
 * for an operation, as pyro's flash_op() does, raises it around that
 * operation only.
 *
 * Before the scheduler starts, a call runs directly in its caller. A call
 * from the flash task itself (a driver calling back) or from an interrupt,
 * and a call with a file another task opened, return ALFS_EINVAL.
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

#include "alfs.h"

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

/* A served instance. Opaque. */
typedef struct alfs_rt {
    alfs_t fs;
    TaskHandle_t task;
    QueueHandle_t queue;
    uint32_t requests; /* served since alfs_rt_start */
} alfs_rt_t;

/* A file, and the task that opened it. Opaque but for f.version. */
typedef struct alfs_rt_file {
    alfs_file_t f;
    TaskHandle_t owner;
} alfs_rt_file_t;

/* Creates the flash task and its queue. ALFS_EINVAL: memory missing. */
int alfs_rt_start(alfs_rt_t *rt, const alfs_rt_mem_t *mem);

/* As alfs_format, alfs_mount: the region, through the flash task. */
int alfs_rt_format(alfs_rt_t *rt, const alfs_config_t *cfg, const alfs_driver_t *drv, uint8_t *scratch);
int alfs_rt_mount(alfs_rt_t *rt, const alfs_config_t *cfg, const alfs_driver_t *drv, uint8_t *scratch);

/*
 * As alfs_erase, but one request a step, so the flash task serves other
 * requests between them (RT-07).
 */
int alfs_rt_erase(alfs_rt_t *rt, uint32_t steps);

/* As alfs_create, alfs_open_read: the file is the calling task's. */
int alfs_rt_create(alfs_rt_t *rt, alfs_rt_file_t *f, const char *name, uint8_t *block, uint32_t size);
int alfs_rt_open_read(alfs_rt_t *rt, alfs_rt_file_t *f, const char *name, uint32_t version, uint8_t *block,
                      uint32_t size);

/* As alfs_write, alfs_read, alfs_sync, alfs_close. */
int32_t alfs_rt_write(alfs_rt_t *rt, alfs_rt_file_t *f, const void *data, uint32_t len);
int32_t alfs_rt_read(alfs_rt_t *rt, alfs_rt_file_t *f, void *out, uint32_t len);
int alfs_rt_sync(alfs_rt_t *rt, alfs_rt_file_t *f);
int alfs_rt_close(alfs_rt_t *rt, alfs_rt_file_t *f);

/* As alfs_delete, alfs_list, alfs_status. */
int alfs_rt_delete(alfs_rt_t *rt, const char *name, uint32_t version);
int alfs_rt_list(alfs_rt_t *rt, uint32_t after, alfs_info_t *info);
int alfs_rt_status(alfs_rt_t *rt, alfs_status_t *st);

#endif
