/* SIMP-03 on the target: the instance and a stream, page buffers apart.
 * C99 has no _Static_assert: an array of negative size fails the build. */
#include "alfs.h"

typedef char simp03_instance_at_most_64[sizeof(alfs_t) <= 64 ? 1 : -1];
typedef char simp03_stream_at_most_32[sizeof(alfs_stream_t) <= 32 ? 1 : -1];

int alfs_target_sizes_checked;
