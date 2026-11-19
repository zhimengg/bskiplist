/*
 * layout/baseline.h - the reference layout: the node arrangement and code of
 * commit 4622083 behind the layout contract.
 */

#ifndef BSL_LAYOUT_BASELINE_H
#define BSL_LAYOUT_BASELINE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include "params.h"
#include "util.h"
#include "hocc64.h"

#define NODE_SIZE 2080

/* Node memory and in-node search: the original include/node.h. */

typedef struct {
    hocc64_t ctrl;
    void *next;
    bsl_key_t next_header;
    uint32_t num_elts;
    uint32_t level;
}  /*__attribute__((aligned(CACHE_LINE_SIZE)))*/ node_header_t;
/*STATIC_ASSERT(sizeof(node_header_t) == CACHE_LINE_SIZE);*/

#define NODE_HEADER_SIZE (sizeof(node_header_t))
#define B_INTERNAL ((NODE_SIZE - NODE_HEADER_SIZE) / (sizeof(bsl_key_t) + sizeof(void*)))
#define B_LEAF ((NODE_SIZE - NODE_HEADER_SIZE) / (sizeof(bsl_key_t) + sizeof(bsl_val_t)))

typedef struct internal_node {
    node_header_t header;
    bsl_key_t keys[B_INTERNAL];
    void *children[B_INTERNAL];
} internal_node_t;

typedef struct leaf_node {
    node_header_t header;
    bsl_key_t keys[B_LEAF];
    bsl_val_t values[B_LEAF];
} leaf_node_t;

#define LEAF_VALUES_OFFSET       (offsetof(leaf_node_t, values))
#define INTERNAL_CHILDREN_OFFSET (offsetof(internal_node_t, children))

#define NODE_GC_NEXT_OFFSET (NODE_SIZE - sizeof(void*))
STATIC_ASSERT(NODE_GC_NEXT_OFFSET >= LEAF_VALUES_OFFSET);
STATIC_ASSERT(NODE_GC_NEXT_OFFSET >= INTERNAL_CHILDREN_OFFSET);

#define NODE_KEYS(n)            ((bsl_key_t *)((node_header_t *)(n) + 1))
#define INTERNAL_CHILDREN(n)    (((internal_node_t *)(n))->children)
#define LEAF_VALUES(n)          (((leaf_node_t *)(n))->values)

#define LEAF_MAX_ENTRIES     B_LEAF
#define INTERNAL_MAX_ENTRIES B_INTERNAL

/* leaf_scan returns views into the node. */
#define LEAF_SCAN_BUFFER_ENTRIES 0

static inline uint32_t
find_rank_linear(bsl_key_t *keys, uint32_t num_elts, bsl_key_t key)
{
    bsl_key_t *aligned_keys = __builtin_assume_aligned(keys, NODE_HEADER_SIZE);
    uint32_t i = 0;
    while (i < num_elts && aligned_keys[i] <= key)
        i++;
    return i > 0 ? i - 1 : 0;
}

static inline uint32_t
find_rank_binary(bsl_key_t *keys, uint32_t num_elts, bsl_key_t key)
{
    bsl_key_t *base = keys;
    uint32_t n = num_elts;
    while (n > 1)
    {
        uint32_t half = n >> 1;
        base = (LOAD_RELAXED(base[half]) <= key) ? &base[half] : base;
        n -= half;
    }

    uint32_t rank = (uint32_t)(base - keys);
    if (LOAD_RELAXED(keys[rank]) > key) return (rank > 0) ? rank - 1 : 0;
    return rank;
}

static inline uint32_t
find_rank(bsl_key_t *keys, uint32_t num_elts, bsl_key_t key)
{
#if defined(__AVX2__)
    return find_rank_linear(keys, num_elts, key);
#else
    return find_rank_binary(keys, num_elts, key);
#endif
}

/* Slot helpers: the original bsl_insert.c and bsl_delete.c. */

static inline int
split_keys(void *src_node, void *dst_node, uint32_t start_rank, int dst_offset)
{
    node_header_t *src_h = (node_header_t *)src_node;
    node_header_t *dst_h = (node_header_t *)dst_node;

    uint32_t move_cnt = src_h->num_elts - start_rank;
    assert(start_rank <= src_h->num_elts);

    bsl_key_t *src_keys = NODE_KEYS(src_node);
    bsl_key_t *dst_keys = NODE_KEYS(dst_node);

    memmove(dst_keys + dst_offset, src_keys + start_rank, move_cnt * sizeof(bsl_key_t));

    STORE_RELAXED(src_h->num_elts, (uint32_t)start_rank);
    __atomic_add_fetch(&dst_h->num_elts, (uint32_t)move_cnt, __ATOMIC_RELAXED);
    return move_cnt;
}

static inline void
insert_internal_slot(void *node, int rank, bsl_key_t key, void *child)
{
    node_header_t *h = (node_header_t *)node;
    bsl_key_t *keys = NODE_KEYS(node);
    void **children = INTERNAL_CHILDREN(node);

    memmove(keys + rank + 1, keys + rank, (h->num_elts - rank) * sizeof(bsl_key_t));
    memmove(children + rank + 1, children + rank, (h->num_elts - rank) * sizeof(void *));

    STORE_RELAXED(keys[rank], key);
    STORE_RELAXED(children[rank], child);

    __atomic_add_fetch(&h->num_elts, 1, __ATOMIC_RELAXED);
}

static inline void
insert_leaf_slot(void *node, int rank, bsl_key_t key, bsl_val_t value)
{
    node_header_t *h = (node_header_t *)node;
    bsl_key_t *keys = NODE_KEYS(node);
    bsl_val_t *values = LEAF_VALUES(node);

    memmove(keys + rank + 1, keys + rank, (h->num_elts - rank) * sizeof(bsl_key_t));
    memmove(values + rank + 1, values + rank, (h->num_elts - rank) * sizeof(bsl_val_t));

    STORE_RELAXED(keys[rank], key);
    STORE_RELAXED(values[rank], value);

    __atomic_add_fetch(&h->num_elts, 1, __ATOMIC_RELAXED);
}

static inline void delete_internal_slot(void *node, uint32_t rank)
{
    node_header_t *h = (node_header_t *)node;
    bsl_key_t *keys = NODE_KEYS(node);
    void **children = INTERNAL_CHILDREN(node);

    int num_to_move = h->num_elts - rank - 1;
    if (num_to_move > 0)
    {
        memmove(keys + rank, keys + rank + 1, num_to_move * sizeof(bsl_key_t));
        memmove(children + rank, children + rank + 1, num_to_move * sizeof(void *));
    }

    __atomic_sub_fetch(&h->num_elts, 1, __ATOMIC_RELAXED);
}

static inline void delete_leaf_slot(void *node, uint32_t rank)
{
    node_header_t *h = (node_header_t *)node;
    bsl_key_t *keys = NODE_KEYS(node);
    bsl_val_t *values = LEAF_VALUES(node);

    int num_to_move = h->num_elts - rank - 1;

    if (num_to_move > 0)
    {
        memmove(keys + rank, keys + rank + 1, num_to_move * sizeof(bsl_key_t));
        memmove(values + rank, values + rank + 1, num_to_move * sizeof(bsl_val_t));
    }

    __atomic_sub_fetch(&h->num_elts, 1, __ATOMIC_RELAXED);
}

/* Contract functions */

static inline hocc64_t *
node_version(bsl_node_t *n)
{
    return &((node_header_t *)n)->ctrl;
}

static inline int
node_level(bsl_node_t *n)
{
    return LOAD_RELAXED(((node_header_t *)n)->level);
}

static inline bsl_node_t *
node_next(bsl_node_t *n)
{
    return LOAD_RELAXED(((node_header_t *)n)->next);
}

static inline bsl_node_t *
node_next_acquire(bsl_node_t *n)
{
    return LOAD_ACQUIRE(((node_header_t *)n)->next);
}

static inline void
node_set_next(bsl_node_t *n, bsl_node_t *next)
{
    STORE_RELAXED(((node_header_t *)n)->next, (void *)next);
}

static inline bsl_key_t
node_next_header(bsl_node_t *n)
{
    return LOAD_RELAXED(((node_header_t *)n)->next_header);
}

static inline void
node_set_next_header(bsl_node_t *n, bsl_key_t key)
{
    STORE_RELAXED(((node_header_t *)n)->next_header, key);
}

static inline uint32_t
node_count(bsl_node_t *n)
{
    return ((node_header_t *)n)->num_elts;
}

static inline bsl_key_t
node_first_key(bsl_node_t *n)
{
    return NODE_KEYS(n)[0];
}

static inline int
leaf_lookup(bsl_node_t *n, bsl_key_t key, bsl_val_t *value)
{
    int        num  = LOAD_RELAXED(((node_header_t *)n)->num_elts);
    bsl_key_t *keys = NODE_KEYS(n);
    int        rank = find_rank(keys, num, key);

    if (keys[rank] == key)
    {
        *value = LEAF_VALUES(n)[rank];
        return 1;
    }
    return 0;
}

/* Entries with key >= start, as a view into the node. */
static inline size_t
leaf_scan(bsl_node_t *n, bsl_key_t start, size_t max,
          bsl_key_t *buf_keys, bsl_val_t *buf_vals,
          const bsl_key_t **keys_out, const bsl_val_t **vals_out)
{
    leaf_node_t *leaf = (leaf_node_t *)n;
    (void)buf_keys;
    (void)buf_vals;

    int num  = LOAD_RELAXED(leaf->header.num_elts);
    int rank = 0;

    if (LOAD_RELAXED(leaf->keys[0]) < start)
    {
        rank = find_rank(leaf->keys, num, start);
        if (LOAD_RELAXED(leaf->keys[rank]) != start)
            rank++;
    }

    if (rank >= num)
        return 0;

    size_t count = (size_t)(num - rank);
    if (max < count)
        count = max;

    *keys_out = &leaf->keys[rank];
    *vals_out = &leaf->values[rank];
    return count;
}

static inline int
leaf_upsert(bsl_node_t *n, bsl_key_t key, bsl_val_t value)
{
    node_header_t *h    = (node_header_t *)n;
    bsl_key_t     *keys = NODE_KEYS(n);
    uint32_t       rank = find_rank_linear(keys, h->num_elts, key);

    if (keys[rank] == key)
    {
        STORE_RELAXED(LEAF_VALUES(n)[rank], value);
        return BSL_UPDATED;
    }

    uint32_t max_key = B_LEAF;
    if (h->num_elts + 1 > max_key)
        return BSL_FULL;

    insert_leaf_slot(n, rank + 1, key, value);
    return BSL_INSERTED;
}

static inline void
leaf_insert(bsl_node_t *n, bsl_key_t key, bsl_val_t value)
{
    node_header_t *h = (node_header_t *)n;

    if (h->num_elts == 0)
    {
        insert_leaf_slot(n, 0, key, value);
        return;
    }

    uint32_t rank = find_rank_linear(NODE_KEYS(n), h->num_elts, key);
    insert_leaf_slot(n, rank + 1, key, value);
}

static inline int
leaf_remove(bsl_node_t *n, bsl_key_t key)
{
    node_header_t *h    = (node_header_t *)n;
    bsl_key_t     *keys = NODE_KEYS(n);
    uint32_t       rank = find_rank_linear(keys, h->num_elts, key);

    if (keys[rank] != key)
        return 0;

    delete_leaf_slot(n, rank);
    return 1;
}

static inline bsl_node_t *
internal_route(bsl_node_t *n, bsl_key_t key)
{
    bsl_key_t *keys = NODE_KEYS(n);
    int        rank = find_rank(keys, LOAD_RELAXED(((node_header_t *)n)->num_elts), key);

    return (bsl_node_t *)LOAD_RELAXED(INTERNAL_CHILDREN(n)[rank]);
}

static inline bsl_node_t *
internal_first_child(bsl_node_t *n)
{
    return (bsl_node_t *)INTERNAL_CHILDREN(n)[0];
}

static inline bsl_node_t *
internal_last_child(bsl_node_t *n)
{
    return (bsl_node_t *)INTERNAL_CHILDREN(n)[((node_header_t *)n)->num_elts - 1];
}

static inline int
internal_find(bsl_node_t *n, bsl_key_t key, bsl_node_t **child, bsl_node_t **left_child)
{
    node_header_t *h    = (node_header_t *)n;
    bsl_key_t     *keys = NODE_KEYS(n);
    uint32_t       rank = find_rank_linear(keys, h->num_elts, key);

    if (keys[rank] != key)
        return 0;

    void **children = INTERNAL_CHILDREN(n);
    *child      = (bsl_node_t *)children[rank];
    *left_child = rank != 0 ? (bsl_node_t *)children[rank - 1] : NULL;
    return 1;
}

static inline int
internal_insert(bsl_node_t *n, bsl_key_t key, bsl_node_t *child, bsl_node_t **left_child)
{
    node_header_t *h = (node_header_t *)n;

    if (h->num_elts == 0)
    {
        insert_internal_slot(n, 0, key, child);
        *left_child = NULL;
        return BSL_INSERTED;
    }

    uint32_t max_key = B_INTERNAL;
    if (h->num_elts + 1 > max_key)
        return BSL_FULL;

    uint32_t rank = find_rank_linear(NODE_KEYS(n), h->num_elts, key);
    insert_internal_slot(n, rank + 1, key, child);
    *left_child = (bsl_node_t *)INTERNAL_CHILDREN(n)[rank];
    return BSL_INSERTED;
}

static inline void
internal_remove(bsl_node_t *n, bsl_key_t key)
{
    uint32_t rank = find_rank_linear(NODE_KEYS(n), ((node_header_t *)n)->num_elts, key);
    delete_internal_slot(n, rank);
}

static inline size_t
internal_entries(bsl_node_t *n, size_t max, bsl_key_t *out_keys, bsl_node_t **out_children)
{
    size_t count = ((node_header_t *)n)->num_elts;
    if (max < count) count = max;
    for (size_t i = 0; i < count; i++)
    {
        out_keys[i]     = NODE_KEYS(n)[i];
        out_children[i] = (bsl_node_t *)INTERNAL_CHILDREN(n)[i];
    }
    return count;
}

static inline void
leaf_split_promote(bsl_node_t *src, bsl_key_t key, bsl_val_t value, bsl_node_t *dst)
{
    int rank = find_rank_linear(NODE_KEYS(src), ((node_header_t *)src)->num_elts, key);

    insert_leaf_slot(dst, 0, key, value);

    uint32_t elts_moved = split_keys(src, dst, rank + 1, 1);

    memmove(
        LEAF_VALUES(dst) + 1,
        LEAF_VALUES(src) + rank + 1,
        elts_moved * sizeof(bsl_val_t)
    );
}

static inline void
internal_split_promote(bsl_node_t *src, bsl_key_t key, bsl_node_t *child, bsl_node_t *dst)
{
    int rank = find_rank_linear(NODE_KEYS(src), ((node_header_t *)src)->num_elts, key);

    insert_internal_slot(dst, 0, key, child);

    uint32_t elts_moved = split_keys(src, dst, rank + 1, 1);

    memmove(
        INTERNAL_CHILDREN(dst) + 1,
        INTERNAL_CHILDREN(src) + rank + 1,
        elts_moved * sizeof(void *)
    );
}

static inline bsl_key_t
node_split_half(bsl_node_t *src, bsl_node_t *dst)
{
    node_header_t *src_h = (node_header_t *)src;

    int half_keys = src_h->num_elts / 2;

    int elts_moved = split_keys(src, dst, half_keys, 0);

    if (src_h->level > 0)
    {
        memmove(
            INTERNAL_CHILDREN(dst),
            INTERNAL_CHILDREN(src) + half_keys,
            elts_moved * sizeof(void *)
        );
    }
    else
    {
        memmove(
            LEAF_VALUES(dst),
            LEAF_VALUES(src) + half_keys,
            elts_moved * sizeof(bsl_val_t)
        );
    }

    return NODE_KEYS(dst)[0];
}

bsl_node_t *bsl_leaf_alloc(void);
bsl_node_t *bsl_internal_alloc(int level);
void bsl_node_free(bsl_node_t *n);

static inline void **
node_retire_link(bsl_node_t *n)
{
    return (void **)((char *)n + NODE_GC_NEXT_OFFSET);
}

static inline void
node_fill(bsl_node_t *n, size_t *used, size_t *limit)
{
    node_header_t *h = (node_header_t *)n;
    *used  = h->num_elts;
    *limit = h->level == 0 ? B_LEAF : B_INTERNAL;
}

#ifdef BSL_LAYOUT_IMPL
#include <stdlib.h>

/* The original src/node.c. */
static void *
baseline_node_alloc(void)
{
    void* ptr;
    if (posix_memalign(&ptr, CACHE_LINE_SIZE, NODE_SIZE) != 0)
        return NULL;
    memset(ptr, 0, NODE_SIZE);
    return ptr;
}

bsl_node_t *
bsl_leaf_alloc(void)
{
    return baseline_node_alloc();
}

bsl_node_t *
bsl_internal_alloc(int level)
{
    node_header_t *h = baseline_node_alloc();
    if (h) h->level = level;
    return (bsl_node_t *)h;
}

void
bsl_node_free(bsl_node_t *n)
{
    if (!n) return;
    free(n);
}
#endif /* BSL_LAYOUT_IMPL */

#endif /* BSL_LAYOUT_BASELINE_H */
