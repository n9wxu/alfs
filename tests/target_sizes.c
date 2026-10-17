/* SIMP-03 on the target: instances and handles, blocks apart.
 * C99 has no _Static_assert: an array of negative size fails the build. */
#include "alfs_log.h"
#include "alfs_obj.h"

typedef char simp03_log_at_most_96[sizeof(alfs_log_t) <= 96 ? 1 : -1];
typedef char simp03_obj_at_most_96[sizeof(alfs_obj_t) <= 96 ? 1 : -1];
typedef char simp03_writer_at_most_32[sizeof(alfs_writer_t) <= 32 ? 1 : -1];
typedef char simp03_reader_at_most_32[sizeof(alfs_reader_t) <= 32 ? 1 : -1];
typedef char simp03_put_at_most_16[sizeof(alfs_put_t) <= 16 ? 1 : -1];
typedef char simp03_get_at_most_16[sizeof(alfs_get_t) <= 16 ? 1 : -1];

int alfs_target_sizes_checked;
