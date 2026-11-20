/*
 * layout/aos.h - sorted array-of-structures: one array of (key, value) or (key, child)
 * entries per node, sorted by key. Header, capacity, split rule and searches are the
 * reference layout's; the retirement link is a field of its own after the entries (rule 5).
 */

#ifndef BSL_LAYOUT_AOS_H
#define BSL_LAYOUT_AOS_H

#include <string.h>
#include <assert.h>
#include "params.h"
#include "util.h"
#include "hocc64.h"

#define LAYOUT_NAME "aos"

#ifdef BSL_LAYOUT_SMALL
#define AOS_ENTRIES   8
#define AOS_NODE_SIZE 168     /* 32-byte header + 8 entries of 16 bytes + link */
#else
#define AOS_ENTRIES   128
#define AOS_NODE_SIZE 2088    /* 32-byte header + 128 entries of 16 bytes + link */
#endif

typedef struct {
    bsl_key_t key;
    union { bsl_val_t value; void *child; };
} aos_entry_t;

typedef struct {
    hocc64_t ctrl;
    void *next;
    bsl_key_t next_header;
    uint32_t num_elts;
    uint32_t level;
    aos_entry_t e[AOS_ENTRIES];
    void *retire_link;
} aos_node_t;

_Static_assert(sizeof(aos_entry_t) == 16, "aos: an entry is 16 bytes");
_Static_assert(sizeof(aos_node_t) == AOS_NODE_SIZE, "aos: node size");

#define AOS(n) ((aos_node_t *)(n))

#define LEAF_MAX_ENTRIES     AOS_ENTRIES
#define INTERNAL_MAX_ENTRIES AOS_ENTRIES
#define LEAF_NODE_BYTES      AOS_NODE_SIZE
#define INTERNAL_NODE_BYTES  AOS_NODE_SIZE

/* leaf_scan copies into the engine's buffers. */
#define LEAF_SCAN_BUFFER_ENTRIES LEAF_MAX_ENTRIES

/* In-node search: the reference's find_rank_linear, find_rank_binary and find_rank. */

static inline uint32_t aos_rank_linear(aos_entry_t *e, uint32_t num_elts, bsl_key_t key)
{
    aos_entry_t *aligned = __builtin_assume_aligned(e, 32);
    uint32_t i = 0;
    while (i < num_elts && aligned[i].key <= key)
        i++;
    return i > 0 ? i - 1 : 0;
}

static inline uint32_t aos_rank_binary(aos_entry_t *e, uint32_t num_elts, bsl_key_t key)
{
    aos_entry_t *base = e;
    uint32_t n = num_elts;
    while (n > 1)
    {
        uint32_t half = n >> 1;
        base = (LOAD_RELAXED(base[half].key) <= key) ? &base[half] : base;
        n -= half;
    }
    uint32_t rank = (uint32_t)(base - e);
    if (LOAD_RELAXED(e[rank].key) > key) return (rank > 0) ? rank - 1 : 0;
    return rank;
}

static inline uint32_t aos_rank(aos_entry_t *e, uint32_t num_elts, bsl_key_t key)
{
#if defined(__AVX2__)
    return aos_rank_linear(e, num_elts, key);
#else
    return aos_rank_binary(e, num_elts, key);
#endif
}

/* Slot helpers for both node kinds: the reference's, with one memmove per change. */

static inline void aos_insert_slot(aos_node_t *a, uint32_t rank, aos_entry_t x)
{
    memmove(a->e + rank + 1, a->e + rank, (a->num_elts - rank) * sizeof(aos_entry_t));
    STORE_RELAXED(a->e[rank].key, x.key);
    STORE_RELAXED(a->e[rank].value, x.value);
    __atomic_add_fetch(&a->num_elts, 1, __ATOMIC_RELAXED);
}

static inline void aos_delete_slot(aos_node_t *a, uint32_t rank)
{
    int num_to_move = a->num_elts - rank - 1;
    if (num_to_move > 0)
        memmove(a->e + rank, a->e + rank + 1, num_to_move * sizeof(aos_entry_t));
    __atomic_sub_fetch(&a->num_elts, 1, __ATOMIC_RELAXED);
}

/* The reference's split_keys, moving whole entries. */
static inline void aos_move(aos_node_t *src, aos_node_t *dst, uint32_t start_rank, int dst_offset)
{
    uint32_t move_cnt = src->num_elts - start_rank;
    assert(start_rank <= src->num_elts);
    memmove(dst->e + dst_offset, src->e + start_rank, move_cnt * sizeof(aos_entry_t));
    STORE_RELAXED(src->num_elts, start_rank);
    __atomic_add_fetch(&dst->num_elts, move_cnt, __ATOMIC_RELAXED);
}

/* Contract functions, in the order of baseline.h */

static inline hocc64_t *node_version(bsl_node_t *n) { return &AOS(n)->ctrl; }
static inline int node_level(bsl_node_t *n) { return LOAD_RELAXED(AOS(n)->level); }
static inline bsl_node_t *node_next(bsl_node_t *n) { return LOAD_RELAXED(AOS(n)->next); }
static inline bsl_node_t *node_next_acquire(bsl_node_t *n) { return LOAD_ACQUIRE(AOS(n)->next); }
static inline void node_set_next(bsl_node_t *n, bsl_node_t *next) { STORE_RELAXED(AOS(n)->next, (void *)next); }
static inline bsl_key_t node_next_header(bsl_node_t *n) { return LOAD_RELAXED(AOS(n)->next_header); }
static inline void node_set_next_header(bsl_node_t *n, bsl_key_t key) { STORE_RELAXED(AOS(n)->next_header, key); }
static inline uint32_t node_count(bsl_node_t *n) { return AOS(n)->num_elts; }
static inline bsl_key_t node_first_key(bsl_node_t *n) { return AOS(n)->e[0].key; }

static inline int leaf_lookup(bsl_node_t *n, bsl_key_t key, bsl_val_t *value)
{
    int          num  = LOAD_RELAXED(AOS(n)->num_elts);
    aos_entry_t *e    = AOS(n)->e;
    int          rank = aos_rank(e, num, key);

    if (e[rank].key == key)
    {
        *value = e[rank].value;
        return 1;
    }
    return 0;
}

/* Entries with key >= start, copied into the buffers; the count is clamped (rule 1). */
static inline size_t leaf_scan(bsl_node_t *n, bsl_key_t start, size_t max,
                               bsl_key_t *buf_keys, bsl_val_t *buf_vals,
                               const bsl_key_t **keys_out, const bsl_val_t **vals_out)
{
    aos_entry_t *e    = AOS(n)->e;
    int          num  = LOAD_RELAXED(AOS(n)->num_elts);
    int          rank = 0;

    if (num > AOS_ENTRIES)
        num = AOS_ENTRIES;
    if (LOAD_RELAXED(e[0].key) < start)
    {
        rank = aos_rank(e, num, start);
        if (LOAD_RELAXED(e[rank].key) != start)
            rank++;
    }
    if (rank >= num)
        return 0;

    size_t count = (size_t)(num - rank);
    if (max < count)
        count = max;
    for (size_t i = 0; i < count; i++)
    {
        buf_keys[i] = e[rank + i].key;
        buf_vals[i] = e[rank + i].value;
    }
    *keys_out = buf_keys;
    *vals_out = buf_vals;
    return count;
}

static inline int leaf_upsert(bsl_node_t *n, bsl_key_t key, bsl_val_t value)
{
    aos_node_t *a    = AOS(n);
    uint32_t    rank = aos_rank_linear(a->e, a->num_elts, key);

    if (a->e[rank].key == key)
    {
        STORE_RELAXED(a->e[rank].value, value);
        return BSL_UPDATED;
    }
    if (a->num_elts + 1 > AOS_ENTRIES)
        return BSL_FULL;
    aos_insert_slot(a, rank + 1, (aos_entry_t){ .key = key, .value = value });
    return BSL_INSERTED;
}

static inline void leaf_insert(bsl_node_t *n, bsl_key_t key, bsl_val_t value)
{
    aos_node_t *a = AOS(n);
    aos_entry_t x = { .key = key, .value = value };

    if (a->num_elts == 0)
        aos_insert_slot(a, 0, x);
    else
        aos_insert_slot(a, aos_rank_linear(a->e, a->num_elts, key) + 1, x);
}

static inline int leaf_remove(bsl_node_t *n, bsl_key_t key)
{
    aos_node_t *a    = AOS(n);
    uint32_t    rank = aos_rank_linear(a->e, a->num_elts, key);

    if (a->e[rank].key != key)
        return 0;
    aos_delete_slot(a, rank);
    return 1;
}

static inline bsl_node_t *internal_route(bsl_node_t *n, bsl_key_t key)
{
    aos_entry_t *e    = AOS(n)->e;
    int          rank = aos_rank(e, LOAD_RELAXED(AOS(n)->num_elts), key);

    return (bsl_node_t *)LOAD_RELAXED(e[rank].child);
}

static inline bsl_node_t *internal_first_child(bsl_node_t *n) { return AOS(n)->e[0].child; }

static inline bsl_node_t *internal_last_child(bsl_node_t *n)
{
    return AOS(n)->e[AOS(n)->num_elts - 1].child;
}

static inline int internal_find(bsl_node_t *n, bsl_key_t key, bsl_node_t **child, bsl_node_t **left_child)
{
    aos_node_t *a    = AOS(n);
    uint32_t    rank = aos_rank_linear(a->e, a->num_elts, key);

    if (a->e[rank].key != key)
        return 0;
    *child      = a->e[rank].child;
    *left_child = rank != 0 ? a->e[rank - 1].child : NULL;
    return 1;
}

static inline int internal_insert(bsl_node_t *n, bsl_key_t key, bsl_node_t *child, bsl_node_t **left_child)
{
    aos_node_t *a = AOS(n);
    aos_entry_t x = { .key = key, .child = child };

    if (a->num_elts == 0)
    {
        aos_insert_slot(a, 0, x);
        *left_child = NULL;
        return BSL_INSERTED;
    }
    if (a->num_elts + 1 > AOS_ENTRIES)
        return BSL_FULL;

    uint32_t rank = aos_rank_linear(a->e, a->num_elts, key);
    aos_insert_slot(a, rank + 1, x);
    *left_child = a->e[rank].child;
    return BSL_INSERTED;
}

static inline void internal_remove(bsl_node_t *n, bsl_key_t key)
{
    aos_delete_slot(AOS(n), aos_rank_linear(AOS(n)->e, AOS(n)->num_elts, key));
}

static inline size_t internal_entries(bsl_node_t *n, size_t max, bsl_key_t *out_keys, bsl_node_t **out_children)
{
    size_t count = AOS(n)->num_elts;
    if (max < count) count = max;
    for (size_t i = 0; i < count; i++)
    {
        out_keys[i]     = AOS(n)->e[i].key;
        out_children[i] = AOS(n)->e[i].child;
    }
    return count;
}

/* Both promotion splits: dst gets x, then every entry of src after x's rank. */
static inline void aos_split_promote(bsl_node_t *src, aos_entry_t x, bsl_node_t *dst)
{
    uint32_t rank = aos_rank_linear(AOS(src)->e, AOS(src)->num_elts, x.key);
    aos_insert_slot(AOS(dst), 0, x);
    aos_move(AOS(src), AOS(dst), rank + 1, 1);
}

static inline void leaf_split_promote(bsl_node_t *src, bsl_key_t key, bsl_val_t value, bsl_node_t *dst)
{
    aos_split_promote(src, (aos_entry_t){ .key = key, .value = value }, dst);
}

static inline void internal_split_promote(bsl_node_t *src, bsl_key_t key, bsl_node_t *child, bsl_node_t *dst)
{
    aos_split_promote(src, (aos_entry_t){ .key = key, .child = child }, dst);
}

static inline bsl_key_t node_split_half(bsl_node_t *src, bsl_node_t *dst)
{
    aos_move(AOS(src), AOS(dst), AOS(src)->num_elts / 2, 0);
    return AOS(dst)->e[0].key;
}

bsl_node_t *bsl_leaf_alloc(void);
bsl_node_t *bsl_internal_alloc(int level);
void bsl_node_free(bsl_node_t *n);

static inline void **node_retire_link(bsl_node_t *n) { return &AOS(n)->retire_link; }

static inline void node_fill(bsl_node_t *n, size_t *used, size_t *limit)
{
    *used  = AOS(n)->num_elts;
    *limit = AOS_ENTRIES;
}

#ifdef BSL_LAYOUT_IMPL
#include <stdlib.h>

/* As the reference: cache-line aligned and zero-filled. */
static void *aos_node_alloc(void)
{
    void *ptr;
    if (posix_memalign(&ptr, CACHE_LINE_SIZE, sizeof(aos_node_t)) != 0)
        return NULL;
    memset(ptr, 0, sizeof(aos_node_t));
    return ptr;
}

bsl_node_t *bsl_leaf_alloc(void) { return aos_node_alloc(); }

bsl_node_t *bsl_internal_alloc(int level)
{
    aos_node_t *a = aos_node_alloc();
    if (a) a->level = level;
    return (bsl_node_t *)a;
}

void bsl_node_free(bsl_node_t *n)
{
    if (!n) return;
    free(n);
}
#endif /* BSL_LAYOUT_IMPL */

#endif /* BSL_LAYOUT_AOS_H */
