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

#include <stdlib.h>
#include <string.h>

#define SECTOR 4096u
#define PAGE 256u
#define SECTORS 64u
#define STACK_WORDS 1024u

/* ---- an instance, behind a driver that checks who calls it ------------- */

typedef struct inst {
    sim_t sim;
    alfs_driver_t simd, drv;
    alfs_config_t cfg;
    uint8_t scratch[PAGE];
    alfs_rt_t rt;
    int in_op, overlaps, outsiders, ops;
    TickType_t erase_ticks;    /* an erase takes this long */
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

static inst_t *inst_open(UBaseType_t prio) {
    inst_t *in = calloc(1, sizeof(*in));
    sim_init(&in->sim, (SECTORS + 2) * SECTOR, SECTOR, PAGE, 0, 0xFF, seed);
    sim_fill_random(&in->sim, 0, (SECTORS + 2) * SECTOR);
    in->simd = sim_driver(&in->sim);
    in->drv = (alfs_driver_t){in, w_read, w_prog, w_erase, NULL};
    in->cfg.region_start = SECTOR;
    in->cfg.region_size = SECTORS * SECTOR;
    in->cfg.chip_size = (SECTORS + 2) * SECTOR;
    in->cfg.sector_size = SECTOR;
    in->cfg.page_size = PAGE;
    in->cfg.erase_value = 0xFF;
    alfs_rt_mem_t m = {calloc(STACK_WORDS, sizeof(StackType_t)), STACK_WORDS, calloc(1, sizeof(StaticTask_t)),
                       calloc(8, sizeof(void *)), 8, calloc(1, sizeof(StaticQueue_t)), prio, 0};
    if (alfs_rt_start(&in->rt, &m))
        tf_failed = 1;
    return in;
}

/* Formats, and erases until nothing is dirty. */
static int inst_setup(inst_t *in) {
    int rc = alfs_rt_format(&in->rt, &in->cfg, &in->drv, in->scratch);
    while (!rc)
        rc = alfs_rt_erase(&in->rt, 1);
    return rc == ALFS_CLEAN ? 0 : rc;
}

static void inst_close(inst_t *in) {
    vTaskDelete(in->rt.task); /* its memory is left: the idle task may not have run */
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

static void worker_entry(void *p) {
    worker_t *w = p;
    w->fn(w->arg);
    xSemaphoreGive(w->done);
    vTaskDelete(NULL);
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

static uint8_t pattern(uint32_t file, uint32_t i) {
    return (uint8_t)(file * 77u + i * 13u + (i >> 8));
}

/* Writes n bytes of file's pattern, a piece at a time, syncing every
 * `every` pieces. 0, or the error. */
static int write_pattern(alfs_rt_t *rt, alfs_rt_file_t *f, uint32_t file, uint32_t n, uint32_t piece,
                         uint32_t every) {
    uint8_t b[512];
    for (uint32_t done = 0, k = 0; done < n; k++) {
        uint32_t m = n - done < piece ? n - done : piece;
        for (uint32_t i = 0; i < m; i++)
            b[i] = pattern(file, done + i);
        int32_t got = alfs_rt_write(rt, f, b, m);
        if (got != (int32_t)m)
            return got < 0 ? (int)got : -100;
        done += m;
        if (every && k % every == every - 1 && alfs_rt_sync(rt, f))
            return -101;
    }
    return 0;
}

/* Reads a whole file and checks it against file's pattern: its length, or
 * an error. */
static int32_t read_pattern(alfs_rt_t *rt, const char *name, uint32_t version, uint32_t file) {
    alfs_rt_file_t f;
    uint8_t blk[PAGE], b[100];
    int rc = alfs_rt_open_read(rt, &f, name, version, blk, sizeof(blk));
    if (rc)
        return rc;
    uint32_t n = 0;
    for (;;) {
        int32_t k = alfs_rt_read(rt, &f, b, sizeof(b));
        if (k < 0) {
            alfs_rt_close(rt, &f);
            return k;
        }
        if (!k)
            break;
        for (int32_t i = 0; i < k; i++)
            if (b[i] != pattern(file, n + (uint32_t)i)) {
                alfs_rt_close(rt, &f);
                return -102;
            }
        n += (uint32_t)k;
    }
    rc = alfs_rt_close(rt, &f);
    return rc ? rc : (int32_t)n;
}

/* ---- the tests -------------------------------------------------------- */

static void rt_round_trip(void) {
    inst_t *in = inst_open(3);
    CHECK_EQ(uxTaskPriorityGet(in->rt.task), 3);
    CHECK_EQ(inst_setup(in), 0);
    CHECK_EQ(alfs_rt_mount(&in->rt, &in->cfg, &in->drv, in->scratch), 0);
    alfs_rt_file_t f;
    uint8_t blk[PAGE];
    CHECK_EQ(alfs_rt_create(&in->rt, &f, "log", blk, sizeof(blk)), 0);
    CHECK_EQ(write_pattern(&in->rt, &f, 1, 3000, 70, 5), 0);
    CHECK_EQ(alfs_rt_close(&in->rt, &f), 0);
    CHECK_EQ(read_pattern(&in->rt, "log", 0, 1), 3000);
    alfs_info_t info;
    CHECK_EQ(alfs_rt_list(&in->rt, 0, &info), 0);
    CHECK(strcmp(info.name, "log") == 0);
    CHECK(info.closed);
    CHECK_EQ(info.size, 3000);
    CHECK_EQ(alfs_rt_delete(&in->rt, "log", f.f.version), 0);
    CHECK_EQ(alfs_rt_list(&in->rt, 0, &info), ALFS_ENOENT);
    alfs_status_t st;
    CHECK_EQ(alfs_rt_status(&in->rt, &st), 0);
    CHECK(!st.clean);
    CHECK(in->ops > 0);
    CLEAN_RUN(in);
    inst_close(in);
}

typedef struct {
    inst_t *in;
    uint32_t file;
    int rc;
} job_t;

static void writer(void *p) {
    job_t *j = p;
    alfs_rt_file_t f;
    uint8_t blk[PAGE];
    char name[8] = {'w', (char)('0' + j->file), 0};
    j->rc = alfs_rt_create(&j->in->rt, &f, name, blk, sizeof(blk));
    if (!j->rc)
        j->rc = write_pattern(&j->in->rt, &f, j->file, 4400, 22, 10);
    if (!j->rc)
        j->rc = alfs_rt_close(&j->in->rt, &f);
}

static void reader(void *p) {
    job_t *j = p;
    for (int t = 0; t < 5 && !j->rc; t++) {
        int32_t n = read_pattern(&j->in->rt, "r", 0, j->file);
        j->rc = n == 3000 ? 0 : (int)(n < 0 ? n : -103);
    }
}

static void eraser(void *p) {
    job_t *j = p;
    j->rc = alfs_rt_delete(&j->in->rt, "junk", j->file);
    for (int rc = ALFS_OK; !j->rc && rc == ALFS_OK;) {
        rc = alfs_rt_erase(&j->in->rt, 1);
        if (rc < 0)
            j->rc = rc;
    }
}

static void rt_serialises(void) {
    inst_t *in = inst_open(2);
    CHECK_EQ(inst_setup(in), 0);
    alfs_rt_file_t f;
    uint8_t blk[PAGE];
    CHECK_EQ(alfs_rt_create(&in->rt, &f, "r", blk, sizeof(blk)), 0);
    CHECK_EQ(write_pattern(&in->rt, &f, 9, 3000, 100, 0), 0);
    CHECK_EQ(alfs_rt_close(&in->rt, &f), 0);
    CHECK_EQ(alfs_rt_create(&in->rt, &f, "junk", blk, sizeof(blk)), 0);
    CHECK_EQ(write_pattern(&in->rt, &f, 8, 9000, 100, 0), 0);
    CHECK_EQ(alfs_rt_close(&in->rt, &f), 0);
    job_t jobs[5] = {{in, 1, 0}, {in, 2, 0}, {in, 3, 0}, {in, 9, 0}, {in, f.f.version, 0}};
    worker_t *w[5];
    for (int i = 0; i < 3; i++)
        w[i] = spawn(writer, &jobs[i], 1);
    w[3] = spawn(reader, &jobs[3], 1);
    w[4] = spawn(eraser, &jobs[4], 1);
    for (int i = 0; i < 5; i++)
        join(w[i]);
    for (int i = 0; i < 5; i++) {
        if (jobs[i].rc)
            printf("    job %d: %d\n", i, jobs[i].rc);
        CHECK_EQ(jobs[i].rc, 0);
    }
    for (uint32_t i = 1; i <= 3; i++) {
        char name[8] = {'w', (char)('0' + i), 0};
        CHECK_EQ(read_pattern(&in->rt, name, 0, i), 4400);
    }
    CLEAN_RUN(in);
    inst_close(in);
}

static void rt_fast_path(void) {
    inst_t *in = inst_open(2);
    CHECK_EQ(inst_setup(in), 0);
    alfs_rt_file_t f;
    uint8_t blk[PAGE], b[100];
    memset(b, 0x3C, sizeof(b));
    CHECK_EQ(alfs_rt_create(&in->rt, &f, "fast", blk, sizeof(blk)), 0);
    uint32_t r0 = in->rt.requests;
    CHECK_EQ(alfs_rt_write(&in->rt, &f, b, 100), 100); /* 100 of 256 */
    CHECK_EQ(alfs_rt_write(&in->rt, &f, b, 100), 100); /* 200 */
    CHECK_EQ(in->rt.requests, r0);
    CHECK_EQ(alfs_rt_write(&in->rt, &f, b, 100), 100); /* fills the block */
    CHECK_EQ(in->rt.requests, r0 + 1);
    CHECK_EQ(alfs_rt_close(&in->rt, &f), 0);
    CHECK_EQ(alfs_rt_open_read(&in->rt, &f, "fast", 0, blk, sizeof(blk)), 0);
    r0 = in->rt.requests;
    CHECK_EQ(alfs_rt_read(&in->rt, &f, b, 10), 10); /* fetches a chunk */
    CHECK_EQ(in->rt.requests, r0 + 1);
    CHECK_EQ(alfs_rt_read(&in->rt, &f, b, 10), 10); /* from the block */
    CHECK_EQ(in->rt.requests, r0 + 1);
    CHECK_EQ(alfs_rt_close(&in->rt, &f), 0);
    CLEAN_RUN(in);
    inst_close(in);
}

/* Made by main before the scheduler starts (rt_before_scheduler). */
static inst_t *early;
static int early_rc = -1;

static void before_scheduler(void) {
    early = inst_open(2);
    alfs_rt_file_t f;
    static uint8_t blk[PAGE];
    early_rc = inst_setup(early);
    if (!early_rc)
        early_rc = alfs_rt_create(&early->rt, &f, "early", blk, sizeof(blk));
    if (!early_rc)
        early_rc = write_pattern(&early->rt, &f, 5, 1000, 50, 4);
    if (!early_rc)
        early_rc = alfs_rt_close(&early->rt, &f);
}

static void rt_before_scheduler(void) {
    CHECK_EQ(early_rc, 0);
    CHECK(early->ops > 0);
    CHECK_EQ(early->rt.requests, 0); /* every call ran directly */
    CHECK_EQ(read_pattern(&early->rt, "early", 0, 5), 1000);
    CHECK(early->rt.requests > 0);
    CLEAN_RUN(early);
    inst_close(early);
}

static int status_from_driver(inst_t *in) {
    alfs_status_t st;
    return alfs_rt_status(&in->rt, &st);
}

typedef struct {
    inst_t *in;
    alfs_rt_file_t *f;
    int write_rc, close_rc;
} stranger_t;

static void stranger(void *p) {
    stranger_t *s = p;
    uint8_t b[4] = {1, 2, 3, 4};
    s->write_rc = (int)alfs_rt_write(&s->in->rt, s->f, b, 4);
    s->close_rc = alfs_rt_close(&s->in->rt, s->f);
}

static void rt_reentry_refused(void) {
    inst_t *in = inst_open(2);
    CHECK_EQ(inst_setup(in), 0);
    alfs_rt_file_t f;
    uint8_t blk[PAGE];
    in->reenter = status_from_driver;
    CHECK_EQ(alfs_rt_create(&in->rt, &f, "x", blk, sizeof(blk)), 0); /* its program calls back */
    CHECK_EQ(in->reenter_rc, ALFS_EINVAL);
    stranger_t s = {in, &f, 0, 0};
    join(spawn(stranger, &s, 1));
    CHECK_EQ(s.write_rc, ALFS_EINVAL);
    CHECK_EQ(s.close_rc, ALFS_EINVAL);
    CHECK_EQ(alfs_rt_close(&in->rt, &f), 0);
    alfs_info_t info;
    CHECK_EQ(alfs_rt_list(&in->rt, 0, &info), 0);
    CHECK_EQ(info.size, 0); /* nothing the stranger wrote */
    CLEAN_RUN(in);
    inst_close(in);
}

static volatile int finished; /* order in which the jobs below finished */

typedef struct {
    inst_t *in;
    int rc, order;
} timed_t;

static void long_erase(void *p) {
    timed_t *t = p;
    int rc;
    while ((rc = alfs_rt_erase(&t->in->rt, 1)) == ALFS_OK)
        ;
    t->rc = rc == ALFS_CLEAN ? 0 : rc;
    t->order = ++finished;
}

static void short_write(void *p) {
    timed_t *t = p;
    alfs_rt_file_t f;
    uint8_t blk[PAGE];
    vTaskDelay(2); /* the erase is under way */
    t->rc = alfs_rt_create(&t->in->rt, &f, "w", blk, sizeof(blk));
    if (!t->rc)
        t->rc = write_pattern(&t->in->rt, &f, 4, 600, 100, 2);
    if (!t->rc)
        t->rc = alfs_rt_close(&t->in->rt, &f);
    t->order = ++finished;
}

static void rt_erase_interleaves(void) {
    inst_t *in = inst_open(2);
    CHECK_EQ(alfs_rt_format(&in->rt, &in->cfg, &in->drv, in->scratch), 0); /* all but one dirty */
    in->erase_ticks = 1;
    finished = 0;
    timed_t e = {in, -1, 0}, w = {in, -1, 0};
    worker_t *we = spawn(long_erase, &e, 1), *ww = spawn(short_write, &w, 1);
    join(we);
    join(ww);
    CHECK_EQ(e.rc, 0);
    CHECK_EQ(w.rc, 0);
    CHECK(w.order < e.order);
    CHECK_EQ(read_pattern(&in->rt, "w", 0, 4), 600);
    CLEAN_RUN(in);
    inst_close(in);
}

typedef struct {
    inst_t *in;
    volatile uint32_t version, synced;
    volatile int done;
    int rc;
} live_t;

static void live_writer(void *p) {
    live_t *l = p;
    alfs_rt_file_t f;
    uint8_t blk[PAGE];
    l->rc = alfs_rt_create(&l->in->rt, &f, "live", blk, sizeof(blk));
    l->version = f.f.version;
    for (uint32_t k = 0; k < 40 && !l->rc; k++) {
        uint8_t b[100];
        for (uint32_t i = 0; i < sizeof(b); i++)
            b[i] = pattern(6, k * 100 + i);
        if (alfs_rt_write(&l->in->rt, &f, b, sizeof(b)) != (int32_t)sizeof(b) || alfs_rt_sync(&l->in->rt, &f))
            l->rc = -104;
        l->synced = (k + 1) * 100;
        vTaskDelay(1);
    }
    if (!l->rc)
        l->rc = alfs_rt_close(&l->in->rt, &f);
    l->done = 1;
}

static void rt_read_while_writing(void) {
    inst_t *in = inst_open(2);
    CHECK_EQ(inst_setup(in), 0);
    live_t l = {in, 0, 0, 0, 0};
    worker_t *w = spawn(live_writer, &l, 1);
    while (!l.version)
        vTaskDelay(1);
    alfs_rt_file_t f;
    uint8_t blk[PAGE], b[64];
    CHECK_EQ(alfs_rt_open_read(&in->rt, &f, NULL, l.version, blk, sizeof(blk)), 0);
    uint32_t n = 0;
    for (int guard = 0; guard < 100000; guard++) {
        int done = l.done;
        uint32_t synced = l.synced;
        int32_t k = alfs_rt_read(&in->rt, &f, b, sizeof(b));
        CHECK(k >= 0);
        for (int32_t i = 0; i < k; i++)
            CHECK_EQ(b[i], pattern(6, n + (uint32_t)i));
        n += (uint32_t)k;
        if (!k && done && n >= synced)
            break;
        if (!k)
            vTaskDelay(1);
    }
    join(w);
    CHECK_EQ(l.rc, 0);
    CHECK_EQ(n, 4000);
    CHECK_EQ(alfs_rt_close(&in->rt, &f), 0);
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
    };
    const char *filter = getenv("ALFS_RT_FILTER");
    int failures = tf_run(tests, (int)(sizeof(tests) / sizeof(tests[0])), filter && *filter ? filter : NULL);
    if (failures)
        printf("seed %u\n", seed);
    exit(failures ? 1 : 0);
}

int main(int argc, char **argv) {
    static StaticTask_t tcb;
    static StackType_t stack[STACK_WORDS];
    const char *env = getenv("ALFS_SEED");
    seed = env ? (uint32_t)strtoul(env, NULL, 0) : 0;
    if (argc > 1)
        setenv("ALFS_RT_FILTER", argv[1], 1);
    before_scheduler();
    xTaskCreateStatic(runner, "runner", STACK_WORDS, NULL, 1, stack, &tcb);
    vTaskStartScheduler();
    return 1;
}
