/*
 * bsl_insert.c
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
#include "bsl_level.h"
#include "stats.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

static inline int 
_bsl_insert(bsl_t *list, bsl_key_t key, bsl_val_t value)
{
    if (key <= BSL_KEY_MIN || key >= BSL_KEY_MAX)
        return 0;

    int level_to_promote = bsl_level_for_key(key);

    /* new_nodes[i] is split off at level level_to_promote - 1 - i. */
    bsl_node_t* new_nodes[MAX_LEVEL] = {0}; 
    for (int i = 0; i < level_to_promote; i++)
    {
        int node_lv = level_to_promote - 1 - i;
        new_nodes[i] = node_lv > 0 ? bsl_internal_alloc(node_lv) : bsl_leaf_alloc();
        NODE_WRITE_LOCK(new_nodes[i]);
    }

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
        if (child == NULL) goto top_retry;
        
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
    
    assert(level == level_to_promote);

    NODE_WRITE_LOCK(curr);
    if (!NODE_VALIDATE(curr, curr_v + HOCC_WRITER_BIT))
    {
        NODE_WRITE_UNLOCK(curr);
        RECORD_RETRY();
        goto top_retry;
    }

    int put = BSL_FULL;
    int found_key;
    bsl_node_t *found_child = NULL;

    if (level == 0)
    {
        put = leaf_upsert(curr, key, value);
        found_key = (put == BSL_UPDATED);
    }
    else
    {
        bsl_node_t *found_left;
        found_key = internal_find(curr, key, &found_child, &found_left);
    }

    if (found_key)
    {
        if (level == 0)
        {
            assert(level_to_promote == 0);
            NODE_WRITE_UNLOCK(curr);
            return 1;
        }
        else
        {
            bsl_node_t *child = found_child;
            hocc64_t child_v = NODE_LOAD_VERSION(child);
            
            NODE_WRITE_UNLOCK(curr);
            
            while (node_level(child) > 0)
            {
                bsl_node_t *next_child = internal_first_child(child);
                if (next_child == NULL)
                {
                    RECORD_RETRY();
                    goto top_retry;
                }

                /* two-phase HOH validation, see bsl_get.c */
                if (child_v & HOCC_WRITER_BIT || !NODE_VALIDATE(child, child_v))
                {
                    RECORD_RETRY();
                    goto top_retry;
                }

                hocc64_t next_v = NODE_LOAD_VERSION(next_child);

                if (!NODE_VALIDATE(child, child_v))
                {
                    RECORD_RETRY();
                    goto top_retry;
                }

                child = next_child;
                child_v = next_v;
            }

            NODE_WRITE_LOCK(child);
            if (!NODE_VALIDATE(child, child_v + HOCC_WRITER_BIT))
            {
                NODE_WRITE_UNLOCK(child);
                RECORD_RETRY();
                goto top_retry;
            }

            /* key is the leaf's first key: this is an overwrite */
            int put_leaf = leaf_upsert(child, key, value);
            assert(put_leaf == BSL_UPDATED);
            (void)put_leaf;

            NODE_WRITE_UNLOCK(child);

            for (int i = 0; i < level_to_promote; i++)
            {
                if (new_nodes[i]) bsl_node_free(new_nodes[i]);
            }
            return 1;
        } 
    }

    bsl_node_t *parent_to_unlock = NULL;
    int num_split = 0;
    bsl_node_t *left_child = NULL;

    if (level > 0)
        put = internal_insert(curr, key, new_nodes[num_split], &left_child);

    if (put == BSL_FULL)
    {
        bsl_node_t *new_node = (level == 0) ? bsl_leaf_alloc() : bsl_internal_alloc(level);
        NODE_WRITE_LOCK(new_node);

        node_set_next(new_node, node_next(curr));
        node_set_next_header(new_node, node_next_header(curr));

        bsl_key_t split_key = node_split_half(curr, new_node);

        node_set_next_header(curr, split_key);
        node_set_next(curr, new_node);

        if (key < split_key)
        {
            NODE_WRITE_UNLOCK(new_node);
            assert(num_split == 0);
            if (level > 0)
            {
                put = internal_insert(curr, key, new_nodes[num_split], &left_child);
                assert(put == BSL_INSERTED);
                parent_to_unlock = curr;
                curr = left_child;
            }
            else
            {
                leaf_insert(curr, key, value);
                NODE_WRITE_UNLOCK(curr);
                return 1;
            }
        }
        else
        {
            NODE_WRITE_UNLOCK(curr);

            if (level > 0)
            {
                put = internal_insert(new_node, key, new_nodes[num_split], &left_child);
                assert(put == BSL_INSERTED);
                parent_to_unlock = new_node;
                curr = left_child;
            } else {
                leaf_insert(new_node, key, value);
                NODE_WRITE_UNLOCK(new_node);
                return 1;
            }
        }
    }
    else
    {
        assert(level_to_promote == level);
        if (level > 0)
        {
            /* inserted by internal_insert above */
            parent_to_unlock = curr;
            curr = left_child;
        } else {
            /* inserted by leaf_upsert above */
            NODE_WRITE_UNLOCK(curr);
            return 1;
        }
    }
    

    for (; level-- > 0;)
    {
        NODE_WRITE_LOCK(curr);

        if (parent_to_unlock)
        {
            NODE_WRITE_UNLOCK(parent_to_unlock);
            parent_to_unlock = NULL;
        }

        bsl_node_t *prev = curr;

        while (node_next_header(curr) <= key)
        {
            NODE_WRITE_LOCK(node_next(curr));
            prev = curr;
            curr = node_next(curr);
            NODE_WRITE_UNLOCK(prev);
        }

        assert(level_to_promote > level);
        bsl_node_t *new_node = new_nodes[num_split];
        num_split++;

        node_set_next_header(new_node, node_next_header(curr));
        node_set_next(new_node, node_next(curr));

        if (level > 0)
        {
            internal_split_promote(curr, key, new_nodes[num_split], new_node);
        } 
        else
        {
            leaf_split_promote(curr, key, value, new_node);
        }
        
        node_set_next_header(curr, node_first_key(new_node));
        node_set_next(curr, new_node);


        /*
        bsl_key_t next_header = new_node->next_header;
        if (level == 0 && bsl_random_level(next_header) == 0 && next_header != BSL_KEY_MAX)
        {
            node_header_t *next_node = (node_header_t *)new_node->next;
            NODE_WRITE_LOCK(next_node);

            int total_elts = new_node->num_elts + next_node->num_elts;

            if (total_elts <= (B_LEAF * 7 / 10)) 
            {
                memcpy(NODE_KEYS(new_node) + new_node->num_elts, NODE_KEYS(next_node), next_node->num_elts * sizeof(bsl_key_t));
                memcpy(LEAF_VALUES(new_node) + new_node->num_elts, LEAF_VALUES(next_node), next_node->num_elts * sizeof(bsl_val_t));

                new_node->num_elts = total_elts;
                STORE_RELAXED(new_node->next, next_node->next);
                STORE_RELAXED(new_node->next_header, next_node->next_header);

                ebr_retire(next_node);
            }
                
            NODE_WRITE_UNLOCK(next_node);
        }
        */
    

        NODE_WRITE_UNLOCK(new_node);

        if (level > 0)
        {
            parent_to_unlock = curr;
            curr = internal_last_child(curr);
        }
        else
        {
            NODE_WRITE_UNLOCK(curr);
        }
    }

    assert(num_split == level_to_promote);
    return 1;
}

int bsl_insert(bsl_t *list, bsl_key_t key, bsl_val_t value)
{
    epoch_enter();
    int ret = _bsl_insert(list, key, value);
    epoch_exit();
    return ret;
}