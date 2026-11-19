#ifndef BSL_NODE_H
#define BSL_NODE_H

/*
 * node.h - includes the node layout selected by the build (make LAYOUT=...;
 * default layout/baseline.h). The engine uses nodes only through it.
 */

#include <stdint.h>
#include <stddef.h>
#include "params.h"
#include "util.h"
#include "hocc64.h"

/* Opaque: each layout casts it to its own structs. */
typedef struct bsl_node bsl_node_t;

/* Results of leaf_upsert and internal_insert. */
enum {
    BSL_UPDATED  = 1,
    BSL_INSERTED = 2,
    BSL_FULL     = 3    /* no room; the node is unchanged */
};

#ifndef BSL_LAYOUT_HEADER
#define BSL_LAYOUT_HEADER "layout/baseline.h"
#endif
#include BSL_LAYOUT_HEADER

/* Length of the leaf_scan buffers; at least 1 (no zero-length arrays). */
#define BSL_SCAN_BUFFER_LEN (LEAF_SCAN_BUFFER_ENTRIES > 0 ? LEAF_SCAN_BUFFER_ENTRIES : 1)

/* Node lock wrappers, the same for every layout. */
#define NODE_LOAD_VERSION(n)    hocc_load(node_version(n))
#define NODE_VALIDATE(n, v)     hocc_validate(node_version(n), (v))
#define NODE_WRITE_LOCK(n)      hocc_write_lock(node_version(n))
#define NODE_WRITE_UNLOCK(n)    hocc_write_unlock(node_version(n))

#endif
