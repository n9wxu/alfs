#include "tf.h"

int tf_failed;

int tf_run(const tf_test_t *tests, int n, const char *filter) {
    int failures = 0, run = 0;
    for (int i = 0; i < n; i++) {
        if (filter && !strstr(tests[i].name, filter))
            continue;
        tf_failed = 0;
        tests[i].fn();
        run++;
        printf("%s  %s\n", tf_failed ? "FAIL" : "ok  ", tests[i].name);
        failures += tf_failed;
    }
    printf("%d of %d passed\n", run - failures, run);
    return failures;
}
