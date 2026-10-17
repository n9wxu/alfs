/*
 * The flash task's tests (docs/TEST_PLAN.md 5), on FreeRTOS's POSIX port.
 *
 * Each instance's driver is the simulator's, wrapped: it counts every
 * flash operation that runs outside the flash task once the scheduler is
 * up, and any two that overlap, and it yields in the middle of each
 * operation so that other tasks run while one is under way. The tests run
 * in one task, after main has made the calls rt_before_scheduler checks.
 */
#include "alfs_rtos.h"
#include "semphr.h"
#include "sim.h"
#include "tf.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SECTOR 4096u
#define PAGE 256u
#define CFG_SECTORS 3u
#define OBJ_SECTORS 4u
#define LOG_SECTORS 64u
#define CHIP ((2u + CFG_SECTORS + OBJ_SECTORS + LOG_SECTORS) * SECTOR)
#define STACK_WORDS 1024u
#define FAILED_OPEN 0xFFFFFFFFu /* published instead of a log number */
#define WATCHDOG_S 240u         /* the whole suite takes seconds */

/* ---- an instance, behind a driver that checks who calls it ------------- */

typedef struct inst {
    sim_t sim;
    alfs_driver_t simd, drv;
    alfs_area_t lcfg, ocfg, ccfg;
    uint8_t lscratch[PAGE], oscratch[PAGE], cscratch[PAGE];
    alfs_log_t lg, cg; /* the flight log area, the configuration area */
    alfs_obj_t ob;
    alfs_rt_t rt;
    int in_op, overlaps, outsiders, ops;
    TickType_t erase_ticks;        /* an erase takes this long */
    int (*reenter)(struct inst *); /* called from inside the next program */
    int reenter_rc;
} inst_t;

static uint32_t seed;

static bool running(void) {
    return xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED;
}

static void op_begin(inst_t *in) {
    in->ops++;
    if (in->in_op++)
        in->overlaps++;
    if (running() && xTaskGetCurrentTaskHandle() != in->rt.task)
        in->outsiders++;
    if (running())
        taskYIELD();
}

static int w_read(void *ctx, uint32_t a, void *b, uint32_t n) {
    inst_t *in = ctx;
    op_begin(in);
    int rc = in->simd.read(in->simd.ctx, a, b, n);
    in->in_op--;
    return rc;
}

static int w_prog(void *ctx, uint32_t a, const void *b, uint32_t n) {
    inst_t *in = ctx;
    op_begin(in);
    if (in->reenter) {
        int (*fn)(inst_t *) = in->reenter;
        in->reenter = NULL;
        in->reenter_rc = fn(in);
    }
    int rc = in->simd.prog(in->simd.ctx, a, b, n);
    in->in_op--;
    return rc;
}

static int w_erase(void *ctx, uint32_t a) {
    inst_t *in = ctx;
    op_begin(in);
    if (running() && in->erase_ticks)
        vTaskDelay(in->erase_ticks);
    int rc = in->simd.erase(in->simd.ctx, a);
    in->in_op--;
    return rc;
}

static void area(alfs_area_t *a, uint32_t start, uint32_t sectors) {
    memset(a, 0, sizeof(*a));
    a->start = start;
    a->size = sectors * SECTOR;
    a->chip_size = CHIP;
    a->sector_size = SECTOR;
    a->page_size = PAGE;
}

/* The chip: a gap sector, the configuration area, the object area, the
 * flight log area, a gap sector. */
static inst_t *inst_open(UBaseType_t prio) {
    inst_t *in = calloc(1, sizeof(*in));
    sim_init(&in->sim, CHIP, SECTOR, PAGE, 0xFF, seed);
    sim_fill_random(&in->sim, 0, CHIP);
    in->simd = sim_driver(&in->sim);
    in->drv = (alfs_driver_t){in, w_read, w_prog, w_erase};
    area(&in->ccfg, SECTOR, CFG_SECTORS);
    area(&in->ocfg, (1u + CFG_SECTORS) * SECTOR, OBJ_SECTORS);
    area(&in->lcfg, (1u + CFG_SECTORS + OBJ_SECTORS) * SECTOR, LOG_SECTORS);
    alfs_rt_mem_t m = {calloc(STACK_WORDS, sizeof(StackType_t)), STACK_WORDS, calloc(1, sizeof(StaticTask_t)),
                       calloc(8, sizeof(void *)), 8, calloc(1, sizeof(StaticQueue_t)), prio, 0};
    if (alfs_rt_start(&in->rt, &m))
        tf_failed = 1;
    return in;
}

static int mount_log(inst_t *in) {
    return alfs_rt_log_mount(&in->rt, &in->lg, &in->lcfg, &in->drv, in->lscratch);
}

/* Erase steps of log (0: the dirty sectors) until none is left. */
static int erase_log(inst_t *in, alfs_log_t *lg, uint32_t log) {
    int rc;
    while ((rc = alfs_rt_log_erase(&in->rt, lg, log, 1)) == ALFS_OK)
        ;
    return rc == ALFS_CLEAN ? 0 : rc;
}

/* The configuration area mounted and erased, the object area formatted,
 * the flight log area mounted and erased. */
static int inst_setup(inst_t *in) {
    int rc = alfs_rt_log_mount(&in->rt, &in->cg, &in->ccfg, &in->drv, in->cscratch);
    if (!rc)
        rc = erase_log(in, &in->cg, 0);
    if (!rc)
        rc = alfs_rt_obj_format(&in->rt, &in->ob, &in->ocfg, &in->drv, in->oscratch);
    if (!rc)
        rc = mount_log(in);
    return rc ? rc : erase_log(in, &in->lg, 0);
}

/* The flash task is left waiting on its queue: the POSIX port deletes a
 * task by joining its thread from the running one, which can hang. */
static void inst_close(inst_t *in) {
    sim_free(&in->sim);
}

#define CLEAN_RUN(in)                                                                              \
    do {                                                                                           \
        if ((in)->outsiders || (in)->overlaps || (in)->sim.violations)                             \
            printf("    %d operations outside the flash task, %d overlapping, flash: %s\n",        \
                   (in)->outsiders, (in)->overlaps, (in)->sim.violations ? (in)->sim.why : "ok"); \
        CHECK_EQ((in)->outsiders, 0);                                                              \
        CHECK_EQ((in)->overlaps, 0);                                                               \
        CHECK_EQ((in)->sim.violations, 0);                                                         \
    } while (0)

/* ---- tasks ------------------------------------------------------------ */

typedef struct worker {
    StaticTask_t tcb;
    StackType_t stack[STACK_WORDS];
    StaticSemaphore_t done_buf;
    SemaphoreHandle_t done;
    void (*fn)(void *);
    void *arg;
} worker_t;

/* A worker waits forever once done: the POSIX port deletes a task by
 * joining its thread from the running one, which can stop every task. */
static void worker_entry(void *p) {
    worker_t *w = p;
    w->fn(w->arg);
    xSemaphoreGive(w->done);
    for (;;)
        vTaskDelay(portMAX_DELAY);
}

static worker_t *spawn(void (*fn)(void *), void *arg, UBaseType_t prio) {
    worker_t *w = calloc(1, sizeof(*w));
    w->fn = fn;
    w->arg = arg;
    w->done = xSemaphoreCreateBinaryStatic(&w->done_buf);
    xTaskCreateStatic(worker_entry, "worker", STACK_WORDS, w, prio, w->stack, &w->tcb);
    return w;
}

static void join(worker_t *w) {
    xSemaphoreTake(w->done, portMAX_DELAY);
}

static uint8_t pattern(uint32_t stream, uint32_t i) {
    return (uint8_t)(stream * 77u + i * 13u + (i >> 8));
}

/* Writes n bytes of stream's pattern as rows of `piece` bytes, syncing
 * every `every` rows. 0, or the error. */
static int write_pattern(alfs_rt_t *rt, alfs_log_t *lg, alfs_rt_writer_t *w, uint32_t stream, uint32_t n,
                         uint32_t piece, uint32_t every) {
    uint8_t b[PAGE];
    for (uint32_t done = 0, k = 0; done < n; k++) {
        uint32_t m = n - done < piece ? n - done : piece;
        for (uint32_t i = 0; i < m; i++)
            b[i] = pattern(stream, done + i);
        int rc = alfs_rt_log_write(rt, lg, w, b, m);
        if (rc)
            return rc;
        done += m;
        if (every && k % every == every - 1 && alfs_rt_log_sync(rt, lg, w))
            return -101;
    }
    return 0;
}

/* Reads a whole stream, row after row, and checks it against its
 * pattern: its length, or an error. */
static int32_t read_pattern(alfs_rt_t *rt, alfs_log_t *lg, uint32_t log, uint8_t tag, uint32_t stream) {
    alfs_rt_reader_t r;
    uint8_t blk[PAGE], b[PAGE];
    int rc = alfs_rt_log_open_read(rt, lg, &r, log, tag, blk, sizeof(blk));
    if (rc)
        return rc;
    uint32_t n = 0;
    for (;;) {
        int32_t k = alfs_rt_log_read(rt, lg, &r, b, sizeof(b));
        if (k < 0) {
            alfs_rt_log_close_read(rt, lg, &r);
            return k;
        }
        if (!k)
            break;
        for (int32_t i = 0; i < k; i++)
            if (b[i] != pattern(stream, n + (uint32_t)i)) {
                alfs_rt_log_close_read(rt, lg, &r);
                return -102;
            }
        n += (uint32_t)k;
    }
    rc = alfs_rt_log_close_read(rt, lg, &r);
    return rc ? rc : (int32_t)n;
}

/* Puts an object of n bytes of stream's pattern, in pieces. */
static int put_pattern(alfs_rt_t *rt, alfs_obj_t *o, const char *name, uint32_t stream, uint32_t n) {
    alfs_put_t p;
    uint8_t b[100];
    int rc = alfs_rt_obj_put(rt, o, &p, name, n);
    for (uint32_t done = 0; !rc && done < n;) {
        uint32_t m = n - done < sizeof(b) ? n - done : (uint32_t)sizeof(b);
        for (uint32_t i = 0; i < m; i++)
            b[i] = pattern(stream, done + i);
        int32_t k = alfs_rt_obj_write(rt, o, &p, b, m);
        if (k != (int32_t)m)
            return k < 0 ? (int)k : -100;
        done += m;
    }
    return rc ? rc : alfs_rt_obj_commit(rt, o, &p);
}

/* Gets an object and checks it against stream's pattern: its size, or an
 * error. */
static int32_t get_pattern(alfs_rt_t *rt, alfs_obj_t *o, const char *name, uint32_t stream) {
    alfs_get_t g;
    uint8_t b[64];
    int rc = alfs_rt_obj_open(rt, o, &g, name);
    if (rc)
        return rc;
    uint32_t n = 0;
    for (;;) {
        int32_t k = alfs_rt_obj_read(rt, o, &g, b, sizeof(b));
        if (k <= 0) {
            alfs_rt_obj_close(rt, o, &g);
            return k < 0 ? k : (int32_t)n;
        }
        for (int32_t i = 0; i < k; i++)
            if (b[i] != pattern(stream, n + (uint32_t)i)) {
                alfs_rt_obj_close(rt, o, &g);
                return -102;
            }
        n += (uint32_t)k;
    }
}

/* One log of the configuration area, through the flash task: rows of a
 * setting written, closed, read back; then the logs before it erased.
 * 0, or the error. */
static int config_log(inst_t *in, uint32_t stream) {
    alfs_rt_writer_t w;
    uint8_t blk[PAGE];
    int rc = alfs_rt_log_open(&in->rt, &in->cg, &w, 0, 1, blk, sizeof(blk));
    if (!rc)
        rc = write_pattern(&in->rt, &in->cg, &w, stream, 600, 40, 5);
    if (!rc)
        rc = alfs_rt_log_close(&in->rt, &in->cg, &w);
    if (!rc && read_pattern(&in->rt, &in->cg, w.w.log, 1, stream) != 600)
        rc = -105;
    alfs_log_info_t info;
    while (!rc && (rc = alfs_rt_log_list(&in->rt, &in->cg, 0, &info)) == ALFS_OK && info.log != w.w.log)
        rc = erase_log(in, &in->cg, info.log);
    return rc;
}

/* ---- the tests -------------------------------------------------------- */

static void rt_round_trip(void) {
    inst_t *in = inst_open(3);
    CHECK_EQ(uxTaskPriorityGet(in->rt.task), 3);
    CHECK_EQ(inst_setup(in), 0);
    CHECK_EQ(mount_log(in), 0);
    alfs_rt_writer_t w;
    uint8_t blk[PAGE];
    CHECK_EQ(alfs_rt_log_open(&in->rt, &in->lg, &w, 0, 1, blk, sizeof(blk)), 0);
    CHECK_EQ(write_pattern(&in->rt, &in->lg, &w, 1, 3000, 70, 5), 0);
    alfs_log_status_t st;
    CHECK_EQ(alfs_rt_log_status(&in->rt, &in->lg, &st), 0);
    CHECK_EQ(st.writing, w.w.log);
    CHECK_EQ(alfs_rt_log_close(&in->rt, &in->lg, &w), 0);
    CHECK_EQ(read_pattern(&in->rt, &in->lg, w.w.log, 1, 1), 3000);
    alfs_log_info_t info;
    CHECK_EQ(alfs_rt_log_list(&in->rt, &in->lg, 0, &info), 0);
    CHECK_EQ(info.log, w.w.log);
    CHECK_EQ(info.sectors, 1);
    CHECK_EQ(erase_log(in, &in->lg, w.w.log), 0);
    CHECK_EQ(alfs_rt_log_list(&in->rt, &in->lg, 0, &info), ALFS_ENOENT);
    CHECK(in->ops > 0);
    CLEAN_RUN(in);
    inst_close(in);
}

typedef struct {
    inst_t *in;
    uint32_t stream;
    volatile uint32_t *log; /* the log both writers write */
    uint32_t old;           /* the log the reader reads, or the eraser erases */
    int rc;
} job_t;

/* Stream 1 starts the log; stream 2 joins it. */
static void writer(void *p) {
    job_t *j = p;
    alfs_rt_writer_t w;
    uint8_t blk[PAGE];
    alfs_log_t *lg = &j->in->lg;
    if (j->stream == 1) {
        j->rc = alfs_rt_log_open(&j->in->rt, lg, &w, 0, 1, blk, sizeof(blk));
        *j->log = j->rc ? FAILED_OPEN : w.w.log;
    } else {
        while (!*j->log)
            vTaskDelay(1);
        j->rc = *j->log == FAILED_OPEN
                    ? -106
                    : alfs_rt_log_open(&j->in->rt, lg, &w, *j->log, (uint8_t)j->stream, blk, sizeof(blk));
    }
    if (!j->rc)
        j->rc = write_pattern(&j->in->rt, lg, &w, j->stream, 4400, 22, 10);
    if (!j->rc)
        j->rc = alfs_rt_log_close(&j->in->rt, lg, &w);
}

static void reader(void *p) {
    job_t *j = p;
    for (int t = 0; t < 5 && !j->rc; t++) {
        int32_t n = read_pattern(&j->in->rt, &j->in->lg, j->old, 9, 9);
        j->rc = n == 3000 ? 0 : (int)(n < 0 ? n : -103);
    }
}

/* Objects, and logs of the configuration area, through the same task. */
static void putter(void *p) {
    job_t *j = p;
    for (uint32_t t = 0; t < 4 && !j->rc; t++) {
        j->rc = put_pattern(&j->in->rt, &j->in->ob, "lua", 20 + t, 300);
        if (!j->rc && get_pattern(&j->in->rt, &j->in->ob, "lua", 20 + t) != 300)
            j->rc = -104;
        if (!j->rc)
            j->rc = config_log(j->in, 30 + t);
    }
}

/* Erases the old log, waiting out readers (IF-13). */
static void eraser(void *p) {
    job_t *j = p;
    for (int rc = ALFS_OK; !j->rc && rc != ALFS_CLEAN;) {
        rc = alfs_rt_log_erase(&j->in->rt, &j->in->lg, j->old, 1);
        if (rc == ALFS_EOPEN)
            vTaskDelay(1);
        else if (rc < 0)
            j->rc = rc;
    }
}

static void rt_serialises(void) {
    inst_t *in = inst_open(2);
    CHECK_EQ(inst_setup(in), 0);
    alfs_rt_writer_t w;
    uint8_t blk[PAGE];
    CHECK_EQ(alfs_rt_log_open(&in->rt, &in->lg, &w, 0, 9, blk, sizeof(blk)), 0);
    CHECK_EQ(write_pattern(&in->rt, &in->lg, &w, 9, 3000, 100, 0), 0);
    CHECK_EQ(alfs_rt_log_close(&in->rt, &in->lg, &w), 0);
    uint32_t kept = w.w.log;
    CHECK_EQ(alfs_rt_log_open(&in->rt, &in->lg, &w, 0, 8, blk, sizeof(blk)), 0);
    CHECK_EQ(write_pattern(&in->rt, &in->lg, &w, 8, 9000, 100, 0), 0);
    CHECK_EQ(alfs_rt_log_close(&in->rt, &in->lg, &w), 0);
    uint32_t junk = w.w.log;
    volatile uint32_t log = 0;
    job_t jobs[5] = {{in, 1, &log, 0, 0}, {in, 2, &log, 0, 0}, {in, 0, NULL, kept, 0}, {in, 0, NULL, 0, 0},
                     {in, 0, NULL, junk, 0}};
    worker_t *wk[5];
    wk[0] = spawn(writer, &jobs[0], 1);
    wk[1] = spawn(writer, &jobs[1], 1);
    wk[2] = spawn(reader, &jobs[2], 1);
    wk[3] = spawn(putter, &jobs[3], 1);
    wk[4] = spawn(eraser, &jobs[4], 1);
    for (int i = 0; i < 5; i++)
        join(wk[i]);
    for (int i = 0; i < 5; i++) {
        if (jobs[i].rc)
            printf("    job %d: %d\n", i, jobs[i].rc);
        CHECK_EQ(jobs[i].rc, 0);
    }
    CHECK_EQ(read_pattern(&in->rt, &in->lg, log, 1, 1), 4400);
    CHECK_EQ(read_pattern(&in->rt, &in->lg, log, 2, 2), 4400);
    CHECK_EQ(read_pattern(&in->rt, &in->lg, kept, 9, 9), 3000);
    CHECK_EQ(read_pattern(&in->rt, &in->lg, junk, 8, 8), ALFS_ENOENT);
    CHECK_EQ(get_pattern(&in->rt, &in->ob, "lua", 23), 300);
    alfs_log_info_t info;
    CHECK_EQ(alfs_rt_log_list(&in->rt, &in->cg, 0, &info), 0); /* the putter's last */
    CHECK_EQ(read_pattern(&in->rt, &in->cg, info.log, 1, 33), 600);
    CLEAN_RUN(in);
    inst_close(in);
}

static void rt_fast_path(void) {
    inst_t *in = inst_open(2);
    CHECK_EQ(inst_setup(in), 0);
    alfs_rt_writer_t w;
    alfs_rt_reader_t r;
    uint8_t blk[PAGE], b[100];
    memset(b, 0x3C, sizeof(b));
    CHECK_EQ(alfs_rt_log_open(&in->rt, &in->lg, &w, 0, 1, blk, sizeof(blk)), 0);
    uint32_t r0 = in->rt.requests;
    CHECK_EQ(alfs_rt_log_write(&in->rt, &in->lg, &w, b, 100), 0); /* 101 of 256 */
    CHECK_EQ(alfs_rt_log_write(&in->rt, &in->lg, &w, b, 100), 0); /* 202 */
    CHECK_EQ(in->rt.requests, r0);
    CHECK_EQ(alfs_rt_log_write(&in->rt, &in->lg, &w, b, 100), 0); /* does not fit */
    CHECK_EQ(in->rt.requests, r0 + 1);
    CHECK_EQ(alfs_rt_log_close(&in->rt, &in->lg, &w), 0);
    CHECK_EQ(alfs_rt_log_open_read(&in->rt, &in->lg, &r, w.w.log, 1, blk, sizeof(blk)), 0);
    r0 = in->rt.requests;
    CHECK_EQ(alfs_rt_log_read(&in->rt, &in->lg, &r, b, sizeof(b)), 100); /* fetches a chunk of two */
    CHECK_EQ(in->rt.requests, r0 + 1);
    CHECK_EQ(alfs_rt_log_read(&in->rt, &in->lg, &r, b, 99), ALFS_EINVAL); /* from the block, too small */
    CHECK_EQ(alfs_rt_log_read(&in->rt, &in->lg, &r, b, sizeof(b)), 100); /* from the block */
    CHECK_EQ(in->rt.requests, r0 + 1);
    CHECK_EQ(alfs_rt_log_close_read(&in->rt, &in->lg, &r), 0);
    CLEAN_RUN(in);
    inst_close(in);
}

/* Made by main before the scheduler starts (rt_before_scheduler). */
static inst_t *early;
static uint32_t early_log;
static int early_rc = -1;

static void before_scheduler(void) {
    early = inst_open(2);
    alfs_rt_writer_t w;
    static uint8_t blk[PAGE];
    early_rc = inst_setup(early);
    if (!early_rc)
        early_rc = put_pattern(&early->rt, &early->ob, "early", 7, 500);
    if (!early_rc)
        early_rc = alfs_rt_log_open(&early->rt, &early->lg, &w, 0, 5, blk, sizeof(blk));
    if (!early_rc)
        early_rc = write_pattern(&early->rt, &early->lg, &w, 5, 1000, 50, 4);
    if (!early_rc)
        early_rc = alfs_rt_log_close(&early->rt, &early->lg, &w);
    early_log = w.w.log;
}

static void rt_before_scheduler(void) {
    CHECK_EQ(early_rc, 0);
    CHECK(early->ops > 0);
    CHECK_EQ(early->rt.requests, 0); /* every call ran directly */
    CHECK_EQ(read_pattern(&early->rt, &early->lg, early_log, 5, 5), 1000);
    CHECK_EQ(get_pattern(&early->rt, &early->ob, "early", 7), 500);
    CHECK(early->rt.requests > 0);
    CLEAN_RUN(early);
    inst_close(early);
}

static int status_from_driver(inst_t *in) {
    alfs_log_status_t st;
    return alfs_rt_log_status(&in->rt, &in->lg, &st);
}

typedef struct {
    inst_t *in;
    alfs_rt_writer_t *w;
    int write_rc, close_rc;
} stranger_t;

static void stranger(void *p) {
    stranger_t *s = p;
    uint8_t b[4] = {1, 2, 3, 4};
    s->write_rc = alfs_rt_log_write(&s->in->rt, &s->in->lg, s->w, b, 4);
    s->close_rc = alfs_rt_log_close(&s->in->rt, &s->in->lg, s->w);
}

static void rt_reentry_refused(void) {
    inst_t *in = inst_open(2);
    CHECK_EQ(inst_setup(in), 0);
    alfs_rt_writer_t w;
    uint8_t blk[PAGE];
    in->reenter = status_from_driver;
    CHECK_EQ(alfs_rt_log_open(&in->rt, &in->lg, &w, 0, 1, blk, sizeof(blk)), 0); /* its program calls back */
    CHECK_EQ(in->reenter_rc, ALFS_EINVAL);
    stranger_t s = {in, &w, 0, 0};
    join(spawn(stranger, &s, 1));
    CHECK_EQ(s.write_rc, ALFS_EINVAL);
    CHECK_EQ(s.close_rc, ALFS_EINVAL);
    CHECK_EQ(alfs_rt_log_close(&in->rt, &in->lg, &w), 0);
    uint8_t got[8];
    alfs_rt_reader_t r;
    CHECK_EQ(alfs_rt_log_open_read(&in->rt, &in->lg, &r, w.w.log, 1, blk, sizeof(blk)), 0);
    CHECK_EQ(alfs_rt_log_read(&in->rt, &in->lg, &r, got, sizeof(got)), 0); /* nothing the stranger wrote */
    CHECK_EQ(alfs_rt_log_close_read(&in->rt, &in->lg, &r), 0);
    CLEAN_RUN(in);
    inst_close(in);
}

static volatile int finished; /* order in which the jobs below finished */

typedef struct {
    inst_t *in;
    uint32_t log;
    int rc, order;
} timed_t;

static void long_erase(void *p) {
    timed_t *t = p;
    t->rc = erase_log(t->in, &t->in->lg, 0);
    t->order = ++finished;
}

static void short_write(void *p) {
    timed_t *t = p;
    alfs_rt_writer_t w;
    uint8_t blk[PAGE];
    alfs_log_t *lg = &t->in->lg;
    vTaskDelay(3); /* the erase is under way */
    while ((t->rc = alfs_rt_log_open(&t->in->rt, lg, &w, 0, 1, blk, sizeof(blk))) == ALFS_ENOSPC)
        vTaskDelay(1); /* until the erase has readied two sectors */
    t->log = w.w.log;
    if (!t->rc)
        t->rc = write_pattern(&t->in->rt, lg, &w, 4, 600, 100, 2);
    if (!t->rc)
        t->rc = alfs_rt_log_close(&t->in->rt, lg, &w);
    t->order = ++finished;
}

static void rt_erase_interleaves(void) {
    inst_t *in = inst_open(2);
    CHECK_EQ(mount_log(in), 0); /* every sector dirty */
    in->erase_ticks = 1;
    finished = 0;
    timed_t e = {in, 0, -1, 0}, w = {in, 0, -1, 0};
    worker_t *we = spawn(long_erase, &e, 1), *ww = spawn(short_write, &w, 1);
    join(we);
    join(ww);
    CHECK_EQ(e.rc, 0);
    CHECK_EQ(w.rc, 0);
    CHECK(w.order < e.order);
    CHECK_EQ(read_pattern(&in->rt, &in->lg, w.log, 1, 4), 600);
    CLEAN_RUN(in);
    inst_close(in);
}

typedef struct {
    inst_t *in;
    volatile uint32_t log, synced;
    volatile int done;
    int rc;
} live_t;

static void live_writer(void *p) {
    live_t *l = p;
    alfs_rt_writer_t w;
    uint8_t blk[PAGE];
    alfs_log_t *lg = &l->in->lg;
    l->rc = alfs_rt_log_open(&l->in->rt, lg, &w, 0, 1, blk, sizeof(blk));
    l->log = l->rc ? FAILED_OPEN : w.w.log;
    if (l->rc)
        l->done = 1;
    for (uint32_t k = 0; k < 40 && !l->rc; k++) {
        uint8_t b[100];
        for (uint32_t i = 0; i < sizeof(b); i++)
            b[i] = pattern(6, k * 100 + i);
        if (alfs_rt_log_write(&l->in->rt, lg, &w, b, sizeof(b)) || alfs_rt_log_sync(&l->in->rt, lg, &w))
            l->rc = -104;
        l->synced = (k + 1) * 100;
        vTaskDelay(1);
    }
    if (!l->rc)
        l->rc = alfs_rt_log_close(&l->in->rt, lg, &w);
    l->done = 1;
}

static void rt_read_while_writing(void) {
    inst_t *in = inst_open(2);
    CHECK_EQ(inst_setup(in), 0);
    live_t l = {in, 0, 0, 0, 0};
    worker_t *w = spawn(live_writer, &l, 1);
    while (!l.log)
        vTaskDelay(1);
    CHECK(l.log != FAILED_OPEN);
    alfs_rt_reader_t r;
    uint8_t blk[PAGE], b[128];
    CHECK_EQ(alfs_rt_log_open_read(&in->rt, &in->lg, &r, l.log, 1, blk, sizeof(blk)), 0);
    uint32_t n = 0;
    for (int guard = 0; guard < 100000; guard++) {
        int done = l.done;
        uint32_t synced = l.synced;
        int32_t k = alfs_rt_log_read(&in->rt, &in->lg, &r, b, sizeof(b));
        CHECK(k >= 0);
        for (int32_t i = 0; i < k; i++)
            CHECK_EQ(b[i], pattern(6, n + (uint32_t)i));
        n += (uint32_t)(k > 0 ? k : 0);
        if (!k && done && n >= synced)
            break;
        if (k <= 0)
            vTaskDelay(1);
    }
    join(w);
    CHECK_EQ(l.rc, 0);
    CHECK_EQ(n, 4000);
    CHECK_EQ(alfs_rt_log_close_read(&in->rt, &in->lg, &r), 0);
    CLEAN_RUN(in);
    inst_close(in);
}

static void rt_objects(void) {
    inst_t *in = inst_open(2);
    CHECK_EQ(inst_setup(in), 0);
    alfs_obj_t *o = &in->ob;
    CHECK_EQ(put_pattern(&in->rt, o, "cfg", 1, 300), 0);
    CHECK_EQ(put_pattern(&in->rt, o, "lua/main.lua", 2, 3000), 0);
    int rc;
    while ((rc = alfs_rt_obj_erase(&in->rt, o, 1)) == ALFS_OK)
        ;
    CHECK_EQ(rc, ALFS_CLEAN);
    alfs_obj_status_t st;
    CHECK_EQ(alfs_rt_obj_status(&in->rt, o, &st), 0);
    CHECK(st.ready);
    CHECK_EQ(put_pattern(&in->rt, o, "cfg", 3, 320), 0);
    CHECK_EQ(get_pattern(&in->rt, o, "cfg", 3), 320);
    CHECK_EQ(get_pattern(&in->rt, o, "lua/main.lua", 2), 3000);
    alfs_obj_info_t info;
    CHECK_EQ(alfs_rt_obj_list(&in->rt, o, 0, &info), 0);
    CHECK(strcmp(info.name, "lua/main.lua") == 0);
    CHECK_EQ(alfs_rt_obj_list(&in->rt, o, 1, &info), 0);
    CHECK(strcmp(info.name, "cfg") == 0);
    CHECK_EQ(info.size, 320);
    CHECK_EQ(alfs_rt_obj_delete(&in->rt, o, "cfg"), 0);
    CHECK_EQ(get_pattern(&in->rt, o, "cfg", 3), ALFS_ENOENT);
    alfs_put_t p;
    CHECK_EQ(alfs_rt_obj_put(&in->rt, o, &p, "x", 10), 0);
    CHECK_EQ(alfs_rt_obj_abort(&in->rt, o, &p), 0);
    CHECK_EQ(alfs_rt_obj_mount(&in->rt, o, &in->ocfg, &in->drv, in->oscratch), 0);
    CHECK_EQ(get_pattern(&in->rt, o, "lua/main.lua", 2), 3000);

    /* The configuration area: a second log area, the same task. */
    for (uint32_t t = 0; t < 4; t++)
        CHECK_EQ(config_log(in, 40 + t), 0);
    CHECK_EQ(alfs_rt_log_mount(&in->rt, &in->cg, &in->ccfg, &in->drv, in->cscratch), 0);
    alfs_log_info_t li;
    CHECK_EQ(alfs_rt_log_list(&in->rt, &in->cg, 0, &li), 0);
    CHECK_EQ(read_pattern(&in->rt, &in->cg, li.log, 1, 43), 600);
    CHECK_EQ(alfs_rt_log_list(&in->rt, &in->cg, li.log, &li), ALFS_ENOENT); /* the only one */
    alfs_log_status_t ls;
    CHECK_EQ(alfs_rt_log_status(&in->rt, &in->cg, &ls), 0);
    CHECK_EQ(ls.sectors, CFG_SECTORS);
    CLEAN_RUN(in);
    inst_close(in);
}

/* ---- the runner ------------------------------------------------------- */

static void runner(void *p) {
    (void)p;
    static const tf_test_t tests[] = {
        {"rt_before_scheduler", rt_before_scheduler},
        {"rt_round_trip", rt_round_trip},
        {"rt_serialises", rt_serialises},
        {"rt_fast_path", rt_fast_path},
        {"rt_reentry_refused", rt_reentry_refused},
        {"rt_erase_interleaves", rt_erase_interleaves},
        {"rt_read_while_writing", rt_read_while_writing},
        {"rt_objects", rt_objects},
    };
    const char *filter = getenv("ALFS_RT_FILTER");
    int failures = tf_run(tests, (int)(sizeof(tests) / sizeof(tests[0])), filter && *filter ? filter : NULL);
    if (failures)
        printf("seed %u\n", seed);
    exit(failures ? 1 : 0);
}

/* A hang shows as a failure that names it: the output is by line, so the
 * last test passed is in the log. */
static void *watchdog(void *p) {
    (void)p;
    sleep(WATCHDOG_S);
    fprintf(stderr, "test_rtos: no end after %u s, seed %u\n", WATCHDOG_S, seed);
    _exit(3);
    return NULL;
}

int main(int argc, char **argv) {
    static StaticTask_t tcb;
    static StackType_t stack[STACK_WORDS];
    pthread_t wd;
    setvbuf(stdout, NULL, _IOLBF, 0);
    pthread_create(&wd, NULL, watchdog, NULL);
    const char *env = getenv("ALFS_SEED");
    seed = env ? (uint32_t)strtoul(env, NULL, 0) : 0;
    if (argc > 1)
        setenv("ALFS_RT_FILTER", argv[1], 1);
    before_scheduler();
    xTaskCreateStatic(runner, "runner", STACK_WORDS, NULL, 1, stack, &tcb);
    vTaskStartScheduler();
    return 1;
}
