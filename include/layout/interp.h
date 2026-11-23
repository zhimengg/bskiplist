/*
 * layout/interp.h - the reference arrangement (sorted keys, then values or children) searched by
 * interpolation: a search guesses the slot from the key's position in the node's key range, then
 * steps to the exact slot.
 *
 * Interpolation search is the classic method (Perl, Itai and Avni, "Interpolation search - a
 * log log N search", CACM 1978); for in-memory sorted arrays, see Van Sandt, Chronis and Patel,
 * "Efficiently Searching In-Memory Sorted Arrays: Revenge of the Interpolation Search?" (SIGMOD
 * 2019). The range used here is the one every node already keeps: keys[0], and next_header, the
 * first key of the next node at the same level. Nothing extra is stored, so unlike the hint array
 * of hints.h, inserts write nothing extra.
 *
 * Capacity, split rule and node structure are the reference layout's; the retirement link is a
 * field of its own after the entries (rule 5).
 */

#ifndef BSL_LAYOUT_INTERP_H
#define BSL_LAYOUT_INTERP_H

#include <string.h>
#include <assert.h>
#include "params.h"
#include "util.h"
#include "hocc64.h"

#define LAYOUT_NAME "interp"

#ifdef BSL_LAYOUT_SMALL
#define INTERP_ENTRIES 8
#else
#define INTERP_ENTRIES 128
#endif

typedef struct {
    hocc64_t ctrl;
    void *next;
    bsl_key_t next_header;
    uint32_t num_elts;
    uint32_t level;
    bsl_key_t keys[INTERP_ENTRIES];
    union { bsl_val_t values[INTERP_ENTRIES]; void *children[INTERP_ENTRIES]; };
    void *retire_link;
} interp_node_t;

_Static_assert(sizeof(void *) == sizeof(bsl_val_t), "interp: values and children share one array");

#define IN(n) ((interp_node_t *)(n))

#define LEAF_MAX_ENTRIES     INTERP_ENTRIES
#define INTERNAL_MAX_ENTRIES INTERP_ENTRIES
#define LEAF_NODE_BYTES      sizeof(interp_node_t)
#define INTERNAL_NODE_BYTES  sizeof(interp_node_t)

/* leaf_scan returns views into the node. */
#define LEAF_SCAN_BUFFER_ENTRIES 0

/*
 * Rank of the largest key <= key, or 0, as the reference's find_rank_linear. The guess is the
 * key's position in [keys[0], next_header) times num, clamped to [0, num - 1]; from there the
 * search steps right while the next key is <= key, or left while the key is > key. Every index
 * stays in [0, num - 1], so a stale or torn read can give a wrong rank but never one outside
 * the first num entries (rule 1).
 */
static inline uint32_t interp_rank(interp_node_t *a, uint32_t num, bsl_key_t key)
{
    if (num == 0)
        return 0;
    bsl_key_t lo = LOAD_RELAXED(a->keys[0]), hi = LOAD_RELAXED(a->next_header);
    uint32_t  i  = 0;

    if (hi > lo && key > lo)
    {
        double g = (double)(key - lo) / (double)(hi - lo) * num;
        i = g < num - 1 ? (uint32_t)g : num - 1;
    }
    if (a->keys[i] <= key)
        while (i + 1 < num && a->keys[i + 1] <= key)
            i++;
    else
        while (i > 0 && a->keys[i] > key)
            i--;
    return i;
}

/* Slot helpers for both node kinds: the reference's. */

static inline void interp_insert_slot(interp_node_t *a, uint32_t rank, bsl_key_t key, bsl_val_t value)
{
    memmove(a->keys + rank + 1, a->keys + rank, (a->num_elts - rank) * sizeof(bsl_key_t));
    memmove(a->values + rank + 1, a->values + rank, (a->num_elts - rank) * sizeof(bsl_val_t));
    STORE_RELAXED(a->keys[rank], key);
    STORE_RELAXED(a->values[rank], value);
    __atomic_add_fetch(&a->num_elts, 1, __ATOMIC_RELAXED);
}

static inline void interp_delete_slot(interp_node_t *a, uint32_t rank)
{
    int num_to_move = a->num_elts - rank - 1;
    if (num_to_move > 0)
    {
        memmove(a->keys + rank, a->keys + rank + 1, num_to_move * sizeof(bsl_key_t));
        memmove(a->values + rank, a->values + rank + 1, num_to_move * sizeof(bsl_val_t));
    }
    __atomic_sub_fetch(&a->num_elts, 1, __ATOMIC_RELAXED);
}

/* The reference's split_keys, moving values or children with the keys. */
static inline void interp_move(interp_node_t *src, interp_node_t *dst, uint32_t start_rank, int dst_offset)
{
    uint32_t move_cnt = src->num_elts - start_rank;
    assert(start_rank <= src->num_elts);
    memmove(dst->keys + dst_offset, src->keys + start_rank, move_cnt * sizeof(bsl_key_t));
    memmove(dst->values + dst_offset, src->values + start_rank, move_cnt * sizeof(bsl_val_t));
    STORE_RELAXED(src->num_elts, start_rank);
    __atomic_add_fetch(&dst->num_elts, move_cnt, __ATOMIC_RELAXED);
}

/* Contract functions, in the order of baseline.h */

static inline hocc64_t *node_version(bsl_node_t *n) { return &IN(n)->ctrl; }
static inline int node_level(bsl_node_t *n) { return LOAD_RELAXED(IN(n)->level); }
static inline bsl_node_t *node_next(bsl_node_t *n) { return LOAD_RELAXED(IN(n)->next); }
static inline bsl_node_t *node_next_acquire(bsl_node_t *n) { return LOAD_ACQUIRE(IN(n)->next); }
static inline void node_set_next(bsl_node_t *n, bsl_node_t *next) { STORE_RELAXED(IN(n)->next, (void *)next); }
static inline bsl_key_t node_next_header(bsl_node_t *n) { return LOAD_RELAXED(IN(n)->next_header); }
static inline void node_set_next_header(bsl_node_t *n, bsl_key_t key) { STORE_RELAXED(IN(n)->next_header, key); }
static inline uint32_t node_count(bsl_node_t *n) { return IN(n)->num_elts; }
static inline bsl_key_t node_first_key(bsl_node_t *n) { return IN(n)->keys[0]; }

static inline int leaf_lookup(bsl_node_t *n, bsl_key_t key, bsl_val_t *value)
{
    interp_node_t *a    = IN(n);
    uint32_t       rank = interp_rank(a, LOAD_RELAXED(a->num_elts), key);

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
    interp_node_t *a    = IN(n);
    uint32_t       num  = LOAD_RELAXED(a->num_elts);
    uint32_t       rank = 0;
    (void)buf_keys;
    (void)buf_vals;

    if (LOAD_RELAXED(a->keys[0]) < start)
    {
        rank = interp_rank(a, num, start);
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
    interp_node_t *a    = IN(n);
    uint32_t       rank = interp_rank(a, a->num_elts, key);

    if (a->keys[rank] == key)
    {
        STORE_RELAXED(a->values[rank], value);
        return BSL_UPDATED;
    }
    if (a->num_elts + 1 > INTERP_ENTRIES)
        return BSL_FULL;
    interp_insert_slot(a, rank + 1, key, value);
    return BSL_INSERTED;
}

static inline void leaf_insert(bsl_node_t *n, bsl_key_t key, bsl_val_t value)
{
    interp_node_t *a = IN(n);

    if (a->num_elts == 0)
        interp_insert_slot(a, 0, key, value);
    else
        interp_insert_slot(a, interp_rank(a, a->num_elts, key) + 1, key, value);
}

static inline int leaf_remove(bsl_node_t *n, bsl_key_t key)
{
    interp_node_t *a    = IN(n);
    uint32_t       rank = interp_rank(a, a->num_elts, key);

    if (a->keys[rank] != key)
        return 0;
    interp_delete_slot(a, rank);
    return 1;
}

static inline bsl_node_t *internal_route(bsl_node_t *n, bsl_key_t key)
{
    interp_node_t *a = IN(n);
    return (bsl_node_t *)LOAD_RELAXED(a->children[interp_rank(a, LOAD_RELAXED(a->num_elts), key)]);
}

static inline bsl_node_t *internal_first_child(bsl_node_t *n) { return IN(n)->children[0]; }

static inline bsl_node_t *internal_last_child(bsl_node_t *n)
{
    return IN(n)->children[IN(n)->num_elts - 1];
}

static inline int internal_find(bsl_node_t *n, bsl_key_t key, bsl_node_t **child, bsl_node_t **left_child)
{
    interp_node_t *a    = IN(n);
    uint32_t       rank = interp_rank(a, a->num_elts, key);

    if (a->keys[rank] != key)
        return 0;
    *child      = a->children[rank];
    *left_child = rank != 0 ? a->children[rank - 1] : NULL;
    return 1;
}

static inline int internal_insert(bsl_node_t *n, bsl_key_t key, bsl_node_t *child, bsl_node_t **left_child)
{
    interp_node_t *a = IN(n);
    bsl_val_t      c = (bsl_val_t)(uintptr_t)child;

    if (a->num_elts == 0)
    {
        interp_insert_slot(a, 0, key, c);
        *left_child = NULL;
        return BSL_INSERTED;
    }
    if (a->num_elts + 1 > INTERP_ENTRIES)
        return BSL_FULL;

    uint32_t rank = interp_rank(a, a->num_elts, key);
    interp_insert_slot(a, rank + 1, key, c);
    *left_child = a->children[rank];
    return BSL_INSERTED;
}

static inline void internal_remove(bsl_node_t *n, bsl_key_t key)
{
    interp_delete_slot(IN(n), interp_rank(IN(n), IN(n)->num_elts, key));
}

static inline size_t internal_entries(bsl_node_t *n, size_t max, bsl_key_t *out_keys, bsl_node_t **out_children)
{
    size_t count = IN(n)->num_elts;
    if (max < count) count = max;
    for (size_t i = 0; i < count; i++)
    {
        out_keys[i]     = IN(n)->keys[i];
        out_children[i] = IN(n)->children[i];
    }
    return count;
}

/* Both promotion splits: dst gets the new entry, then every entry of src after its rank. */
static inline void interp_split_promote(bsl_node_t *src, bsl_key_t key, bsl_val_t value, bsl_node_t *dst)
{
    uint32_t rank = interp_rank(IN(src), IN(src)->num_elts, key);
    interp_insert_slot(IN(dst), 0, key, value);
    interp_move(IN(src), IN(dst), rank + 1, 1);
}

static inline void leaf_split_promote(bsl_node_t *src, bsl_key_t key, bsl_val_t value, bsl_node_t *dst)
{
    interp_split_promote(src, key, value, dst);
}

static inline void internal_split_promote(bsl_node_t *src, bsl_key_t key, bsl_node_t *child, bsl_node_t *dst)
{
    interp_split_promote(src, key, (bsl_val_t)(uintptr_t)child, dst);
}

static inline bsl_key_t node_split_half(bsl_node_t *src, bsl_node_t *dst)
{
    interp_move(IN(src), IN(dst), IN(src)->num_elts / 2, 0);
    return IN(dst)->keys[0];
}

bsl_node_t *bsl_leaf_alloc(void);
bsl_node_t *bsl_internal_alloc(int level);
void bsl_node_free(bsl_node_t *n);

static inline void **node_retire_link(bsl_node_t *n) { return &IN(n)->retire_link; }

static inline void node_fill(bsl_node_t *n, size_t *used, size_t *limit)
{
    *used  = IN(n)->num_elts;
    *limit = INTERP_ENTRIES;
}

#ifdef BSL_LAYOUT_IMPL
#include <stdlib.h>

/* As the reference: cache-line aligned and zero-filled. */
static void *interp_node_alloc(void)
{
    void *ptr;
    if (posix_memalign(&ptr, CACHE_LINE_SIZE, sizeof(interp_node_t)) != 0)
        return NULL;
    memset(ptr, 0, sizeof(interp_node_t));
    return ptr;
}

bsl_node_t *bsl_leaf_alloc(void) { return interp_node_alloc(); }

bsl_node_t *bsl_internal_alloc(int level)
{
    interp_node_t *a = interp_node_alloc();
    if (a) a->level = level;
    return (bsl_node_t *)a;
}

void bsl_node_free(bsl_node_t *n)
{
    if (!n) return;
    free(n);
}
#endif /* BSL_LAYOUT_IMPL */

#endif /* BSL_LAYOUT_INTERP_H */
