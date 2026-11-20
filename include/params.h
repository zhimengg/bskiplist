#ifndef BSL_PARAMS_H
#define BSL_PARAMS_H
#include <stdint.h>

typedef uint64_t bsl_key_t;
typedef uint64_t bsl_val_t;

#define BSL_KEY_MIN ((bsl_key_t)0)
#define BSL_KEY_MAX (~(bsl_key_t)0)

#define MAX_LEVEL 5
#define CACHE_LINE_SIZE 64

/* Promotion denominator, independent of the node layout (the original B_INTERNAL / 2).
 * CONFIG=small sets 4, the value B_INTERNAL / 2 gives for 160-byte nodes. */
#ifndef BSL_PROMOTE_DENOM
#define BSL_PROMOTE_DENOM 64
#endif

#endif