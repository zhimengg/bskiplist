/*
 * layout/hints.h - the reference arrangement (sorted keys, then values or children) with a
 * hint array after the header (SIGMOD 2025, section 3.3): HINTS_N keys taken from evenly
 * spaced positions of the entries. A search scans the hints to pick a group, then searches
 * only that group. Capacity, split rule and node structure are the reference layout's; the
 * retirement link is a field of its own after the entries (rule 5).
 */

#ifndef BSL_LAYOUT_HINTS_H
#define BSL_LAYOUT_HINTS_H

#include <string.h>
#include <assert.h>
#include "params.h"
#include "util.h"
#include "hocc64.h"

#define LAYOUT_NAME "hints"

/* HINTS_N can be set at build time, for example -DHINTS_N=8. */
#ifdef BSL_LAYOUT_SMALL
#define HINTS_ENTRIES 8
#ifndef HINTS_N
#define HINTS_N 2        /* 8 or more hints leave no group among 8 entries */
#endif
#else
#define HINTS_ENTRIES 128
#ifndef HINTS_N
#define HINTS_N 8
#endif
#endif

typedef struct {
    hocc64_t ctrl;
    void *next;
    bsl_key_t next_header;
    uint32_t num_elts;
    uint32_t level;
    bsl_key_t hint[HINTS_N];
    bsl_key_t keys[HINTS_ENTRIES];
    union { bsl_val_t values[HINTS_ENTRIES]; void *children[HINTS_ENTRIES]; };
    void *retire_link;
} hints_node_t;

_Static_assert(HINTS_N >= 1, "hints: HINTS_N must be at least 1");
_Static_assert(sizeof(void *) == sizeof(bsl_val_t), "hints: values and children share one array");

#define HN(n) ((hints_node_t *)(n))

#define LEAF_MAX_ENTRIES     HINTS_ENTRIES
#define INTERNAL_MAX_ENTRIES HINTS_ENTRIES
#define LEAF_NODE_BYTES      sizeof(hints_node_t)
#define INTERNAL_NODE_BYTES  sizeof(hints_node_t)

/* leaf_scan returns views into the node. */
#define LEAF_SCAN_BUFFER_ENTRIES 0

/*
 * Rank of the largest key <= key, or 0, as the reference's find_rank_linear, searching
 * only the group that the hints pick. Hint g is keys[d * (g + 1)] with d = num / (HINTS_N + 1).
 * The group's bounds depend only on num, so a stale or torn hint can give a wrong rank but
 * never one outside the first num entries (rule 1).
 */
static inline uint32_t hints_rank(hints_node_t *a, uint32_t num, bsl_key_t key)
{
    uint32_t d = num / (HINTS_N + 1), g = 0, i;

    while (g < HINTS_N && LOAD_RELAXED(a->hint[g]) <= key)
        g++;
    uint32_t lo = g * d, hi = g < HINTS_N ? lo + d : num;
    for (i = lo; i < hi && a->keys[i] <= key; i++)
        ;
    return i > lo ? i - 1 : lo;
}

/*
 * Called under the lock after every change to the keys, with the count before the change and
 * the first slot that changed. While the spacing d stays the same, only the hints at or after
 * that slot are refreshed (the paper's partial update); r = 0 refreshes all.
 */
static inline void hints_refresh(hints_node_t *a, uint32_t old_num, uint32_t r)
{
    uint32_t d = a->num_elts / (HINTS_N + 1);
    if (d != old_num / (HINTS_N + 1))
        r = 0;
    for (uint32_t g = 0; g < HINTS_N; g++)
        if (d * (g + 1) >= r)
            STORE_RELAXED(a->hint[g], a->keys[d * (g + 1)]);
}

/* Slot helpers for both node kinds: the reference's, followed by a refresh. */

static inline void hints_insert_slot(hints_node_t *a, uint32_t rank, bsl_key_t key, bsl_val_t value)
{
    memmove(a->keys + rank + 1, a->keys + rank, (a->num_elts - rank) * sizeof(bsl_key_t));
    memmove(a->values + rank + 1, a->values + rank, (a->num_elts - rank) * sizeof(bsl_val_t));
    STORE_RELAXED(a->keys[rank], key);
    STORE_RELAXED(a->values[rank], value);
    __atomic_add_fetch(&a->num_elts, 1, __ATOMIC_RELAXED);
    hints_refresh(a, a->num_elts - 1, rank);
}

static inline void hints_delete_slot(hints_node_t *a, uint32_t rank)
{
    int num_to_move = a->num_elts - rank - 1;
    if (num_to_move > 0)
    {
        memmove(a->keys + rank, a->keys + rank + 1, num_to_move * sizeof(bsl_key_t));
        memmove(a->values + rank, a->values + rank + 1, num_to_move * sizeof(bsl_val_t));
    }
    __atomic_sub_fetch(&a->num_elts, 1, __ATOMIC_RELAXED);
    hints_refresh(a, a->num_elts + 1, rank);
}

/* The reference's split_keys, moving values or children with the keys. */
static inline void hints_move(hints_node_t *src, hints_node_t *dst, uint32_t start_rank, int dst_offset)
{
    uint32_t move_cnt = src->num_elts - start_rank;
    assert(start_rank <= src->num_elts);
    memmove(dst->keys + dst_offset, src->keys + start_rank, move_cnt * sizeof(bsl_key_t));
    memmove(dst->values + dst_offset, src->values + start_rank, move_cnt * sizeof(bsl_val_t));
    STORE_RELAXED(src->num_elts, start_rank);
    __atomic_add_fetch(&dst->num_elts, move_cnt, __ATOMIC_RELAXED);
    hints_refresh(src, 0, 0);
    hints_refresh(dst, 0, 0);
}

/* Contract functions, in the order of baseline.h */

static inline hocc64_t *node_version(bsl_node_t *n) { return &HN(n)->ctrl; }
static inline int node_level(bsl_node_t *n) { return LOAD_RELAXED(HN(n)->level); }
static inline bsl_node_t *node_next(bsl_node_t *n) { return LOAD_RELAXED(HN(n)->next); }
static inline bsl_node_t *node_next_acquire(bsl_node_t *n) { return LOAD_ACQUIRE(HN(n)->next); }
static inline void node_set_next(bsl_node_t *n, bsl_node_t *next) { STORE_RELAXED(HN(n)->next, (void *)next); }
static inline bsl_key_t node_next_header(bsl_node_t *n) { return LOAD_RELAXED(HN(n)->next_header); }
static inline void node_set_next_header(bsl_node_t *n, bsl_key_t key) { STORE_RELAXED(HN(n)->next_header, key); }
static inline uint32_t node_count(bsl_node_t *n) { return HN(n)->num_elts; }
static inline bsl_key_t node_first_key(bsl_node_t *n) { return HN(n)->keys[0]; }

static inline int leaf_lookup(bsl_node_t *n, bsl_key_t key, bsl_val_t *value)
{
    hints_node_t *a    = HN(n);
    uint32_t      rank = hints_rank(a, LOAD_RELAXED(a->num_elts), key);

    if (a->keys[rank] == key)
    {
        *value = a->values[rank];
        return 1;
    }
    return 0;
}

/* Entries with key >= start, as a view into the node. */
static inline size_t leaf_scan(bsl_node_t *n, bsl_key_t start, size_t max,
                               bsl_key_t *buf_keys, bsl_val_t *buf_vals,
                               const bsl_key_t **keys_out, const bsl_val_t **vals_out)
{
    hints_node_t *a    = HN(n);
    uint32_t      num  = LOAD_RELAXED(a->num_elts);
    uint32_t      rank = 0;
    (void)buf_keys;
    (void)buf_vals;

    if (LOAD_RELAXED(a->keys[0]) < start)
    {
        rank = hints_rank(a, num, start);
        if (LOAD_RELAXED(a->keys[rank]) != start)
            rank++;
    }
    if (rank >= num)
        return 0;

    size_t count = (size_t)(num - rank);
    if (max < count)
        count = max;
    *keys_out = &a->keys[rank];
    *vals_out = &a->values[rank];
    return count;
}

static inline int leaf_upsert(bsl_node_t *n, bsl_key_t key, bsl_val_t value)
{
    hints_node_t *a    = HN(n);
    uint32_t      rank = hints_rank(a, a->num_elts, key);

    if (a->keys[rank] == key)
    {
        STORE_RELAXED(a->values[rank], value);
        return BSL_UPDATED;
    }
    if (a->num_elts + 1 > HINTS_ENTRIES)
        return BSL_FULL;
    hints_insert_slot(a, rank + 1, key, value);
    return BSL_INSERTED;
}

static inline void leaf_insert(bsl_node_t *n, bsl_key_t key, bsl_val_t value)
{
    hints_node_t *a = HN(n);

    if (a->num_elts == 0)
        hints_insert_slot(a, 0, key, value);
    else
        hints_insert_slot(a, hints_rank(a, a->num_elts, key) + 1, key, value);
}

static inline int leaf_remove(bsl_node_t *n, bsl_key_t key)
{
    hints_node_t *a    = HN(n);
    uint32_t      rank = hints_rank(a, a->num_elts, key);

    if (a->keys[rank] != key)
        return 0;
    hints_delete_slot(a, rank);
    return 1;
}

static inline bsl_node_t *internal_route(bsl_node_t *n, bsl_key_t key)
{
    hints_node_t *a = HN(n);
    return (bsl_node_t *)LOAD_RELAXED(a->children[hints_rank(a, LOAD_RELAXED(a->num_elts), key)]);
}

static inline bsl_node_t *internal_first_child(bsl_node_t *n) { return HN(n)->children[0]; }

static inline bsl_node_t *internal_last_child(bsl_node_t *n)
{
    return HN(n)->children[HN(n)->num_elts - 1];
}

static inline int internal_find(bsl_node_t *n, bsl_key_t key, bsl_node_t **child, bsl_node_t **left_child)
{
    hints_node_t *a    = HN(n);
    uint32_t      rank = hints_rank(a, a->num_elts, key);

    if (a->keys[rank] != key)
        return 0;
    *child      = a->children[rank];
    *left_child = rank != 0 ? a->children[rank - 1] : NULL;
    return 1;
}

static inline int internal_insert(bsl_node_t *n, bsl_key_t key, bsl_node_t *child, bsl_node_t **left_child)
{
    hints_node_t *a = HN(n);
    bsl_val_t     c = (bsl_val_t)(uintptr_t)child;

    if (a->num_elts == 0)
    {
        hints_insert_slot(a, 0, key, c);
        *left_child = NULL;
        return BSL_INSERTED;
    }
    if (a->num_elts + 1 > HINTS_ENTRIES)
        return BSL_FULL;

    uint32_t rank = hints_rank(a, a->num_elts, key);
    hints_insert_slot(a, rank + 1, key, c);
    *left_child = a->children[rank];
    return BSL_INSERTED;
}

static inline void internal_remove(bsl_node_t *n, bsl_key_t key)
{
    hints_delete_slot(HN(n), hints_rank(HN(n), HN(n)->num_elts, key));
}

static inline size_t internal_entries(bsl_node_t *n, size_t max, bsl_key_t *out_keys, bsl_node_t **out_children)
{
    size_t count = HN(n)->num_elts;
    if (max < count) count = max;
    for (size_t i = 0; i < count; i++)
    {
        out_keys[i]     = HN(n)->keys[i];
        out_children[i] = HN(n)->children[i];
    }
    return count;
}

/* Both promotion splits: dst gets the new entry, then every entry of src after its rank. */
static inline void hints_split_promote(bsl_node_t *src, bsl_key_t key, bsl_val_t value, bsl_node_t *dst)
{
    uint32_t rank = hints_rank(HN(src), HN(src)->num_elts, key);
    hints_insert_slot(HN(dst), 0, key, value);
    hints_move(HN(src), HN(dst), rank + 1, 1);
}

static inline void leaf_split_promote(bsl_node_t *src, bsl_key_t key, bsl_val_t value, bsl_node_t *dst)
{
    hints_split_promote(src, key, value, dst);
}

static inline void internal_split_promote(bsl_node_t *src, bsl_key_t key, bsl_node_t *child, bsl_node_t *dst)
{
    hints_split_promote(src, key, (bsl_val_t)(uintptr_t)child, dst);
}

static inline bsl_key_t node_split_half(bsl_node_t *src, bsl_node_t *dst)
{
    hints_move(HN(src), HN(dst), HN(src)->num_elts / 2, 0);
    return HN(dst)->keys[0];
}

bsl_node_t *bsl_leaf_alloc(void);
bsl_node_t *bsl_internal_alloc(int level);
void bsl_node_free(bsl_node_t *n);

static inline void **node_retire_link(bsl_node_t *n) { return &HN(n)->retire_link; }

static inline void node_fill(bsl_node_t *n, size_t *used, size_t *limit)
{
    *used  = HN(n)->num_elts;
    *limit = HINTS_ENTRIES;
}

#ifdef BSL_LAYOUT_IMPL
#include <stdlib.h>

/* As the reference: cache-line aligned and zero-filled. */
static void *hints_node_alloc(void)
{
    void *ptr;
    if (posix_memalign(&ptr, CACHE_LINE_SIZE, sizeof(hints_node_t)) != 0)
        return NULL;
    memset(ptr, 0, sizeof(hints_node_t));
    return ptr;
}

bsl_node_t *bsl_leaf_alloc(void) { return hints_node_alloc(); }

bsl_node_t *bsl_internal_alloc(int level)
{
    hints_node_t *a = hints_node_alloc();
    if (a) a->level = level;
    return (bsl_node_t *)a;
}

void bsl_node_free(bsl_node_t *n)
{
    if (!n) return;
    free(n);
}
#endif /* BSL_LAYOUT_IMPL */

#endif /* BSL_LAYOUT_HINTS_H */
