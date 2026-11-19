/*
 * bsl_scan.c
 *
 * This implementation is inspired by the algorithm described in:
 *   Yicong Luo et al., "<Bridging Cache-Friendliness and Concurrency: A Locality-Optimized In-Memory B-Skiplist>", ICPP 2025
 *   arXiv: https://arxiv.org/abs/2507.21492
 *
 * Original C++ reference implementation (Apache 2.0):
 *   https://github.com/Ratbuyer/bskip_artifact
 *
 * This is an independent C reimplementation. The read-only traversal phase
 * (levels above level_to_promote) uses optimistic version validation instead
 * of reader-writer locks, falling back to pessimistic write locking during
 * the modification phase.
 */

#include "bskiplist.h"
#include "node.h"
#include "epoch.h"
#include "stats.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

static inline size_t
_bsl_scan_n_batch_optimistic(bsl_t *list, bsl_key_t start, size_t length, range_batch_cb cb, void *arg)
{
    if (unlikely(length <= 0)) return 0;

    size_t remaining;
    bsl_key_t current_start;

    /* Used only by layouts whose leaf_scan copies (LEAF_SCAN_BUFFER_ENTRIES > 0). */
    bsl_key_t buf_keys[BSL_SCAN_BUFFER_LEN];
    bsl_val_t buf_vals[BSL_SCAN_BUFFER_LEN];

top_retry:

    remaining = length;
    current_start = start;

    bsl_node_t *curr = list->headers[MAX_LEVEL - 1];
    hocc64_t curr_v = NODE_LOAD_VERSION(curr);

    for (int level = MAX_LEVEL - 1; level > 0; level--)
    {
        while (node_next_header(curr) <= current_start)
        {
            bsl_node_t *next = node_next(curr);
            if (!next)
            {
                RECORD_RETRY();
                goto top_retry;
            }

            hocc64_t next_v = NODE_LOAD_VERSION(next);

            if (!NODE_VALIDATE(curr, curr_v))
            {
                RECORD_RETRY();
                goto top_retry;
            }

            curr = next;
            curr_v = next_v;
        }

        bsl_node_t *child = internal_route(curr, current_start);
        if (!child)
        {
            RECORD_RETRY();
            goto top_retry; 
        }

        /* two-phase HOH validation, see bsl_get.c */
        if (curr_v & HOCC_WRITER_BIT || !NODE_VALIDATE(curr, curr_v))
        {
            RECORD_RETRY();
            goto top_retry; 
        }
        
        hocc64_t child_v = NODE_LOAD_VERSION(child);

        if (!NODE_VALIDATE(curr, curr_v))
        {
            RECORD_RETRY();
            goto top_retry;
        }

        curr = child;
        curr_v = child_v;
    }
    
    while (node_next_header(curr) <= current_start)
    {
        bsl_node_t *next = node_next(curr);
        if (!next)
        {
            RECORD_RETRY();
            goto top_retry;
        }

        hocc64_t next_v = NODE_LOAD_VERSION(next);

        if (!NODE_VALIDATE(curr, curr_v))
        {
            RECORD_RETRY();
            goto top_retry;
        }

        curr = next;
        curr_v = next_v;
    }

    /* One loop for all leaves; a scan from 0 starts at 1, past the head sentinel. */
    bsl_node_t *leaf = curr;
    const bsl_key_t from = (current_start == BSL_KEY_MIN) ? BSL_KEY_MIN + 1 : current_start;

    while (remaining)
    {
        const bsl_key_t *keys;
        const bsl_val_t *vals;
        size_t batch_size = leaf_scan(leaf, from, remaining, buf_keys, buf_vals, &keys, &vals);

        if (!NODE_VALIDATE(leaf, curr_v) || (curr_v & HOCC_WRITER_BIT)) {
            RECORD_RETRY("Leaf changed before batch");
            goto top_retry;
        }

        if (batch_size > 0)
        {
            bsl_range_t range = {
                .keys = keys,
                .vals = vals,
                .count = batch_size,
                .offset = length - remaining
            };

            cb(range, arg);

            if (!NODE_VALIDATE(leaf, curr_v))
            {
                RECORD_RETRY("Leaf: diff version after summing");
                goto top_retry;
            }
      
            remaining -= batch_size;
            // current_start = last_key + 1;
        }

        // leaf_node_t *next = (leaf_node_t *)LOAD_ACQUIRE(leaf->header.next);
        
        if (remaining == 0) {
            break;
        }

        bsl_key_t next_header = node_next_header(leaf);
        bsl_node_t *next = node_next_acquire(leaf);

        if (!NODE_VALIDATE(leaf, curr_v)) {
            RECORD_RETRY("Leaf changed before forward");
            goto top_retry;
        }

        if (next_header == BSL_KEY_MAX) {
            break;
        }

        if (!next) {
            RECORD_RETRY();
            goto top_retry;
        }

        hocc64_t next_v = NODE_LOAD_VERSION(next);

        if (!NODE_VALIDATE(leaf, curr_v))
        {
            RECORD_RETRY("Leaf: Forward");
            goto top_retry;
        }

        leaf = next;
        curr_v = next_v;
    }

    return length - remaining;
}

size_t bsl_scan_n_batch(bsl_t *list, bsl_key_t start, size_t length,
                        range_batch_cb cb, void *arg)
{
    if (unlikely(!cb)) return 0;

    epoch_enter();
    size_t n = _bsl_scan_n_batch_optimistic(list, start, length, cb, arg);
    epoch_exit();
    return n;
}

/*
 * Copy-out entry point, layered on the callback scan above so that there is
 * only one traversal to maintain. The collector is offset-addressed, which is
 * what makes it correct under a retry: the scan restarts from `start`, so a
 * batch may be delivered more than once, and writing at range.offset
 * overwrites the earlier delivery instead of appending after it.
 */
typedef struct {
    bsl_key_t *keys;
    bsl_val_t *vals;
    size_t     capacity;
} scan_collect_t;

static void
_scan_collect_cb(bsl_range_t range, void *arg)
{
    scan_collect_t *out = (scan_collect_t *)arg;

    if (unlikely(range.offset + range.count > out->capacity)) return;

    memcpy(out->keys + range.offset, range.keys,
           range.count * sizeof(bsl_key_t));
    memcpy(out->vals + range.offset, range.vals,
           range.count * sizeof(bsl_val_t));
}

size_t bsl_scan_n(bsl_t *list, bsl_key_t start, size_t length,
                  bsl_key_t *out_keys, bsl_val_t *out_vals, size_t capacity)
{
    if (unlikely(!out_keys || !out_vals)) return 0;
    if (length > capacity) length = capacity;

    scan_collect_t out = { out_keys, out_vals, capacity };

    return bsl_scan_n_batch(list, start, length, _scan_collect_cb, &out);
}