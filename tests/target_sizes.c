/* SIMP-03 on the target: the instance and a file handle, blocks apart.
 * C99 has no _Static_assert: an array of negative size fails the build. */
#include "alfs.h"

typedef char simp03_instance_at_most_96[sizeof(alfs_t) <= 96 ? 1 : -1];
typedef char simp03_file_at_most_48[sizeof(alfs_file_t) <= 48 ? 1 : -1];

int alfs_target_sizes_checked;
