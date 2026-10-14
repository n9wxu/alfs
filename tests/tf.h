/*
 * A minimal test harness: no dependencies. A test is a function; CHECK
 * fails it and returns. tf_run() runs every test whose name contains the
 * filter (argv[1]), prints each result, and returns the failure count.
 */
#ifndef TF_H
#define TF_H

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    void (*fn)(void);
} tf_test_t;

extern int tf_failed;

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            printf("    %s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c);                              \
            tf_failed = 1;                                                                         \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        long long _a = (long long)(a), _b = (long long)(b);                                        \
        if (_a != _b) {                                                                            \
            printf("    %s:%d: %s == %s: %lld != %lld\n", __FILE__, __LINE__, #a, #b, _a, _b);     \
            tf_failed = 1;                                                                         \
            return;                                                                                \
        }                                                                                          \
    } while (0)

int tf_run(const tf_test_t *tests, int n, const char *filter);

#endif
