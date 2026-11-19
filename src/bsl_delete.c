
/*
 * bsl_delete.c
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
#include "bsl_level.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>

/*
 * Looks key up in node. *first_key: key is the node's first key. A leaf
 * entry that is not a first key is removed here (leaf_remove).
 */
static inline int
delete_find(bsl_node_t *node, int level, bsl_key_t key, int *first_key,
            bsl_node_t **child, bsl_node_t **left_child)
{
    if (level == 0)
    {
        *first_key = (node_first_key(node) == key);
        return *first_key || leaf_remove(node, key);
    }

    int found = internal_find(node, key, child, left_child);
    *first_key = found && *left_child == NULL;
    return found;
}


static inline int _bsl_delete(bsl_t *list, bsl_key_t key)
{
    if (key <= BSL_KEY_MIN || key >= BSL_KEY_MAX)
        return 0;

    int level_to_promote = bsl_level_for_key(key);

top_retry:;

    bsl_node_t *curr = list->headers[MAX_LEVEL - 1];
    hocc64_t curr_v = NODE_LOAD_VERSION(curr);

    int level;
    for (level = MAX_LEVEL - 1; level > level_to_promote; level--)
    {
        while (node_next_header(curr) <= key)
        {
            bsl_node_t *next = node_next(curr);
            if (next == NULL)
            {
                RECORD_RETRY();
                goto top_retry;
            } 

            hocc64_t next_v = NODE_LOAD_VERSION(next);

            if (curr_v & HOCC_WRITER_BIT || !NODE_VALIDATE(curr, curr_v))
            {
                RECORD_RETRY();
                goto top_retry;
            }

            curr = next;
            curr_v = next_v;
        }

        bsl_node_t *child = internal_route(curr, key);
        if (child == NULL)
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

    assert(level == level_to_promote);

    NODE_WRITE_LOCK(curr);
    if (!NODE_VALIDATE(curr, curr_v + HOCC_WRITER_BIT))
    {
        NODE_WRITE_UNLOCK(curr);
        RECORD_RETRY();
        goto top_retry;
    }

    int first_key = 0;
    bsl_node_t *found_child = NULL;
    bsl_node_t *found_left = NULL;
    int found_key = delete_find(curr, level, key, &first_key, &found_child, &found_left);

    bsl_node_t *prev = NULL;

    while (!found_key && node_next_header(curr) <= key)
    {
        NODE_WRITE_LOCK(node_next(curr));
        prev = curr;
        curr = node_next(curr);

        found_key = delete_find(curr, level, key, &first_key, &found_child, &found_left);

        if (!found_key) 
            NODE_WRITE_UNLOCK(prev);
    }

    if (!found_key)
    {
        NODE_WRITE_UNLOCK(curr);
        return 0;
    }

    bsl_node_t *current;
    bsl_node_t *left_node;

    if (!first_key)
    {
        if (level == 0)
        {
            /* already removed by delete_find (leaf_remove) */
            if (prev) 
                NODE_WRITE_UNLOCK(prev);
            NODE_WRITE_UNLOCK(curr);
            return 1;
        }
        else
        {
            current = found_child;
            left_node = found_left;

            internal_remove(curr, key);

            NODE_WRITE_LOCK(left_node);
            NODE_WRITE_LOCK(current);

            if (prev) 
                NODE_WRITE_UNLOCK(prev);
            NODE_WRITE_UNLOCK(curr);
        }
    }
    else
    {
        assert(prev);
        left_node = prev;
        current = curr;
    }

    assert(left_node && current);

    prev = NULL;
    for (level = node_level(current); level > 0; level--)
    {
        while (node_next(left_node) != current)
        {
            NODE_WRITE_LOCK(node_next(left_node));
            prev = left_node;
            left_node = node_next(left_node);
            NODE_WRITE_UNLOCK(prev);
        }

        assert(node_first_key(current) == key);
        assert(node_next_header(left_node) == key);

        bsl_node_t *old_curr = current;
        bsl_node_t *old_left = left_node;

        current = internal_first_child(current);
        left_node = internal_last_child(left_node);

        if (node_count(old_curr) == 1)
        {
            node_set_next_header(old_left, node_next_header(old_curr));
            node_set_next(old_left, node_next(old_curr));
            internal_remove(old_curr, key);     /* its last entry */

            NODE_WRITE_UNLOCK(old_curr);
            ebr_retire(old_curr);
        }
        else
        {
            internal_remove(old_curr, key);
            node_set_next_header(old_left, node_first_key(old_curr));
            NODE_WRITE_UNLOCK(old_curr);
        }

        NODE_WRITE_LOCK(left_node);
        NODE_WRITE_LOCK(current);
        NODE_WRITE_UNLOCK(old_left);
    }

    assert(node_level(current) == 0 && node_level(left_node) == 0);

    prev = NULL;
    while (node_next(left_node) != current)
    {
        NODE_WRITE_LOCK(node_next(left_node));
        prev = left_node;
        left_node = node_next(left_node);
        NODE_WRITE_UNLOCK(prev);
    }

    if (node_count(current) == 1)
    {
        node_set_next_header(left_node, node_next_header(current));
        node_set_next(left_node, node_next(current));
        leaf_remove(current, key);              /* its last entry */
        NODE_WRITE_UNLOCK(current);
        ebr_retire(current);
    }
    else
    {
        leaf_remove(current, key);
        node_set_next_header(left_node, node_first_key(current));
        NODE_WRITE_UNLOCK(current);
    }

    NODE_WRITE_UNLOCK(left_node);

    return 1;
}

int bsl_delete(bsl_t *list, bsl_key_t key)
{
    epoch_enter();
    int ret = _bsl_delete(list, key);
    epoch_exit();
    return ret;
}