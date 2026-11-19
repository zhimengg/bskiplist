/*
 * test_layout.c - conformance test for the layout selected by the build
 * (make LAYOUT=...; ASAN=1 for the address sanitizer).
 *
 * Part 1: every contract function, single-threaded, against a sorted-array
 * model: empty, full and head nodes, splits at every position (the pending
 * key must fit in the half the engine picks), BSL_KEY_MAX, and the
 * retirement link (no R function may read it unless the layout defines
 * LAYOUT_RETIRE_LINK_SHARED).
 *
 * Part 2, reader safety: one writer changes one node under its lock while
 * readers call every R function on it. A validated read must match the
 * model; any read must return, with at most `max` entries and no memory
 * outside the node and the buffers.
 *
 * The test calls the layout only as the engine does: no key below the first
 * key of a node with entries, no present key to leaf_insert, internal_insert
 * or a promotion split, and on an empty node only those and the split
 * destinations.
 *
 * Usage: test_layout [milliseconds per concurrent phase, default 700]
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>
#include <time.h>

#include "node.h"

#define MAXE (LEAF_MAX_ENTRIES > INTERNAL_MAX_ENTRIES ? LEAF_MAX_ENTRIES : INTERNAL_MAX_ENTRIES)

/* Bytes of each scan buffer that leaf_scan may use (0 for a view-only layout). */
#define SCAN_BUF_KEY_BYTES (LEAF_SCAN_BUFFER_ENTRIES * sizeof(bsl_key_t))
#define SCAN_BUF_VAL_BYTES (LEAF_SCAN_BUFFER_ENTRIES * sizeof(bsl_val_t))

static long failures;
static long checks;

#define CHECK(cond, ...) do {                                              \
        __atomic_fetch_add(&checks, 1, __ATOMIC_RELAXED);                  \
        if (!(cond)) {                                                     \
            if (__atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED) < 40) { \
                fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);       \
                fprintf(stderr, __VA_ARGS__);                              \
                fprintf(stderr, "\n");                                     \
            }                                                              \
        }                                                                  \
    } while (0)

/* splitmix64 */
static uint64_t
rnd(uint64_t *s)
{
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/* A child pointer that is never dereferenced: unique and non-NULL. */
static bsl_node_t *
fake_child(bsl_key_t k)
{
    return (bsl_node_t *)(uintptr_t)((k << 4) + 16);
}

/* Model: sorted (key, payload) array */

typedef struct {
    size_t    n;
    bsl_key_t k[MAXE + 2];
    uint64_t  v[MAXE + 2];   /* value, or child pointer for internal nodes */
} model_t;

static size_t
m_lb(const model_t *m, bsl_key_t key)
{
    size_t i = 0;
    while (i < m->n && m->k[i] < key) i++;
    return i;
}

static int
m_has(const model_t *m, bsl_key_t key)
{
    size_t i = m_lb(m, key);
    return i < m->n && m->k[i] == key;
}

static void
m_put(model_t *m, bsl_key_t key, uint64_t v)
{
    size_t i = m_lb(m, key);
    if (i < m->n && m->k[i] == key) { m->v[i] = v; return; }
    memmove(m->k + i + 1, m->k + i, (m->n - i) * sizeof m->k[0]);
    memmove(m->v + i + 1, m->v + i, (m->n - i) * sizeof m->v[0]);
    m->k[i] = key;
    m->v[i] = v;
    m->n++;
}

static int
m_del(model_t *m, bsl_key_t key)
{
    size_t i = m_lb(m, key);
    if (i >= m->n || m->k[i] != key) return 0;
    memmove(m->k + i, m->k + i + 1, (m->n - i - 1) * sizeof m->k[0]);
    memmove(m->v + i, m->v + i + 1, (m->n - i - 1) * sizeof m->v[0]);
    m->n--;
    return 1;
}

/* Child of the largest boundary key <= key; 0 if there is none. */
static uint64_t
m_route(const model_t *m, bsl_key_t key)
{
    uint64_t c = 0;
    for (size_t i = 0; i < m->n && m->k[i] <= key; i++) c = m->v[i];
    return c;
}

/* Does [p, p + bytes) lie inside the node or inside the buffer? */
static int
inside(const void *p, size_t bytes, const void *node, size_t node_bytes,
       const void *buf, size_t buf_bytes)
{
    uintptr_t c = (uintptr_t)p, n = (uintptr_t)node, b = (uintptr_t)buf;
    if (c >= n && c + bytes <= n + node_bytes) return 1;
    if (buf && c >= b && c + bytes <= b + buf_bytes) return 1;
    return 0;
}

/* Part 1 helpers */

static void
expect_scan(bsl_node_t *n, const model_t *m, bsl_key_t start, size_t max, const char *what)
{
    /* The buffers the engine passes, plus one guard entry each. */
    bsl_key_t        bk[BSL_SCAN_BUFFER_LEN + 1];
    bsl_val_t        bv[BSL_SCAN_BUFFER_LEN + 1];
    const bsl_key_t *kp = NULL;
    const bsl_val_t *vp = NULL;
    for (size_t i = 0; i <= BSL_SCAN_BUFFER_LEN; i++) { bk[i] = 0xdeadbeef; bv[i] = 0xdeadbeef; }

    size_t first = m_lb(m, start);
    size_t want  = m->n - first;
    if (max < want) want = max;

    size_t c = leaf_scan(n, start, max, bk, bv, &kp, &vp);
    CHECK(c == want, "%s: leaf_scan(start %" PRIu64 ", max %zu) returned %zu, expected %zu",
          what, start, max, c, want);
    for (size_t i = LEAF_SCAN_BUFFER_ENTRIES; i <= BSL_SCAN_BUFFER_LEN; i++)
        CHECK(bk[i] == 0xdeadbeef && bv[i] == 0xdeadbeef,
              "%s: leaf_scan wrote buffer entry %zu, LEAF_SCAN_BUFFER_ENTRIES is %d",
              what, i, (int)LEAF_SCAN_BUFFER_ENTRIES);
    if (c != want || c == 0) return;
    CHECK(inside(kp, c * sizeof *kp, n, LEAF_NODE_BYTES, bk, SCAN_BUF_KEY_BYTES) &&
          inside(vp, c * sizeof *vp, n, LEAF_NODE_BYTES, bv, SCAN_BUF_VAL_BYTES),
          "%s: leaf_scan handed out memory outside the node and the declared buffers", what);
    for (size_t i = 0; i < c; i++)
    {
        CHECK(kp[i] == m->k[first + i] && vp[i] == m->v[first + i],
              "%s: leaf_scan(start %" PRIu64 ") entry %zu is (%" PRIu64 ", %" PRIu64 "), expected (%" PRIu64 ", %" PRIu64 ")",
              what, start, i, kp[i], vp[i], m->k[first + i], m->v[first + i]);
    }
}

static void
expect_leaf(bsl_node_t *n, const model_t *m, const char *what)
{
    CHECK(node_level(n) == 0, "%s: leaf level %d", what, node_level(n));
    CHECK(node_count(n) == m->n, "%s: node_count %u, model %zu", what, node_count(n), m->n);
    if (m->n) CHECK(node_first_key(n) == m->k[0], "%s: node_first_key", what);

    expect_scan(n, m, 0, LEAF_MAX_ENTRIES, what);

    for (size_t i = 0; i < m->n; i++)
    {
        bsl_val_t v = 0;
        CHECK(leaf_lookup(n, m->k[i], &v) == 1 && v == m->v[i],
              "%s: leaf_lookup(%" PRIu64 ")", what, m->k[i]);
        if (!m_has(m, m->k[i] + 1))
            CHECK(leaf_lookup(n, m->k[i] + 1, &v) == 0, "%s: leaf_lookup of an absent key", what);
    }

    size_t used = 0, limit = 0;
    node_fill(n, &used, &limit);
    CHECK(limit > 0 && used <= limit, "%s: node_fill %zu of %zu", what, used, limit);
    if (m->n == 0) CHECK(used == 0, "%s: node_fill of an empty leaf is %zu", what, used);
}

static void
expect_internal(bsl_node_t *n, const model_t *m, const char *what)
{
    CHECK(node_count(n) == m->n, "%s: node_count %u, model %zu", what, node_count(n), m->n);
    if (m->n == 0) return;
    CHECK(node_first_key(n) == m->k[0], "%s: node_first_key", what);
    CHECK((uint64_t)(uintptr_t)internal_first_child(n) == m->v[0], "%s: internal_first_child", what);
    CHECK((uint64_t)(uintptr_t)internal_last_child(n) == m->v[m->n - 1], "%s: internal_last_child", what);

    bsl_key_t   ek[INTERNAL_MAX_ENTRIES];
    bsl_node_t *ec[INTERNAL_MAX_ENTRIES];
    size_t c = internal_entries(n, INTERNAL_MAX_ENTRIES, ek, ec);
    CHECK(c == m->n, "%s: internal_entries returned %zu, model %zu", what, c, m->n);
    for (size_t i = 0; i < c && i < m->n; i++)
        CHECK(ek[i] == m->k[i] && (uint64_t)(uintptr_t)ec[i] == m->v[i],
              "%s: internal_entries entry %zu", what, i);

    for (size_t i = 0; i < m->n; i++)
    {
        bsl_node_t *child = NULL, *left = NULL;
        CHECK(internal_find(n, m->k[i], &child, &left) == 1, "%s: internal_find(%" PRIu64 ")", what, m->k[i]);
        CHECK((uint64_t)(uintptr_t)child == m->v[i], "%s: internal_find child", what);
        CHECK((uint64_t)(uintptr_t)left == (i ? m->v[i - 1] : 0), "%s: internal_find left child", what);

        CHECK((uint64_t)(uintptr_t)internal_route(n, m->k[i]) == m->v[i], "%s: internal_route at a boundary", what);
        bsl_key_t between = m->k[i] + 1;
        if (!m_has(m, between))
        {
            CHECK(internal_find(n, between, &child, &left) == 0, "%s: internal_find of an absent key", what);
            CHECK((uint64_t)(uintptr_t)internal_route(n, between) == m_route(m, between),
                  "%s: internal_route between boundaries", what);
        }
    }
    CHECK((uint64_t)(uintptr_t)internal_route(n, BSL_KEY_MAX) == m->v[m->n - 1], "%s: internal_route(MAX)", what);
}

static bsl_node_t *
build_leaf(const model_t *m)
{
    bsl_node_t *n = bsl_leaf_alloc();
    for (size_t i = 0; i < m->n; i++)
    {
        if (i == 0)
            leaf_insert(n, m->k[i], m->v[i]);
        else
            CHECK(leaf_upsert(n, m->k[i], m->v[i]) == BSL_INSERTED, "build_leaf: insert %zu", i);
    }
    return n;
}

static bsl_node_t *
build_internal(const model_t *m, int level)
{
    bsl_node_t *n = bsl_internal_alloc(level);
    for (size_t i = 0; i < m->n; i++)
    {
        bsl_node_t *left;
        CHECK(internal_insert(n, m->k[i], (bsl_node_t *)(uintptr_t)m->v[i], &left) == BSL_INSERTED,
              "build_internal: insert %zu", i);
    }
    return n;
}

/* A full node's worth of keys spaced by 4, so that k + 1 and k + 2 are free. */
static void
fill_model(model_t *m, size_t n, uint64_t *seed, int internal)
{
    m->n = 0;
    while (m->n < n)
    {
        bsl_key_t k = 4 * (1 + rnd(seed) % (16 * MAXE));
        if (!m_has(m, k)) m_put(m, k, internal ? (uint64_t)(uintptr_t)fake_child(k) : rnd(seed));
    }
}

static void
fresh_checks(bsl_node_t *n, int level, const char *what)
{
    CHECK(*node_version(n) == HOCC_INIT, "%s: version of a new node", what);
    CHECK(node_level(n) == level, "%s: node_level %d, expected %d", what, node_level(n), level);
    CHECK(node_count(n) == 0, "%s: node_count of a new node", what);
    CHECK(node_next(n) == NULL && node_next_acquire(n) == NULL, "%s: next of a new node", what);
    CHECK(node_next_header(n) == 0, "%s: next_header of a new node", what);
}

/* R functions on an empty node must return and stay inside it; their answer
 * is unspecified (the engine retires a node when its last entry goes). */
static void
r_functions_on_empty(bsl_node_t *n, int leaf)
{
    bsl_key_t        bk[BSL_SCAN_BUFFER_LEN];
    bsl_val_t        bv[BSL_SCAN_BUFFER_LEN];
    const bsl_key_t *kp;
    const bsl_val_t *vp;
    bsl_val_t        v;
    bsl_key_t        probes[] = { 0, 1, 7, 1000, BSL_KEY_MAX - 1, BSL_KEY_MAX };
    for (size_t i = 0; i < sizeof probes / sizeof probes[0]; i++)
    {
        (void)node_next(n);
        (void)node_next_acquire(n);
        (void)node_next_header(n);
        (void)node_level(n);
        if (leaf)
        {
            (void)leaf_lookup(n, probes[i], &v);
            size_t max = i;
            size_t c   = leaf_scan(n, probes[i], max, bk, bv, &kp, &vp);
            CHECK(c <= max, "leaf_scan on an empty leaf handed out %zu entries, max %zu", c, max);
        }
        else
        {
            (void)internal_route(n, probes[i]);
            (void)internal_first_child(n);
        }
    }
}

/* Part 1 tests */

static void
test_leaf(uint64_t *seed)
{
    bsl_node_t *n = bsl_leaf_alloc();
    fresh_checks(n, 0, "new leaf");
    r_functions_on_empty(n, 1);

    model_t m = { 0 };
    expect_leaf(n, &m, "empty leaf");

    /* The first entry goes into the empty node with leaf_insert; it is the
     * smallest key of the universe, so no later key is below it. */
    leaf_insert(n, 4, 44);
    m_put(&m, 4, 44);
    expect_leaf(n, &m, "leaf_insert into an empty leaf");

    /* Fill until full. */
    for (;;)
    {
        bsl_key_t k;
        do k = 4 * (1 + rnd(seed) % (16 * MAXE)); while (m_has(&m, k));
        bsl_val_t v = rnd(seed);
        int r = leaf_upsert(n, k, v);
        if (r == BSL_FULL)
        {
            expect_leaf(n, &m, "leaf_upsert BSL_FULL leaves the node unchanged");
            break;
        }
        CHECK(r == BSL_INSERTED, "leaf_upsert of a new key returned %d", r);
        m_put(&m, k, v);
        expect_leaf(n, &m, "leaf_upsert insert");
        if (m.n > LEAF_MAX_ENTRIES) { CHECK(0, "leaf holds more than LEAF_MAX_ENTRIES"); break; }
    }
    CHECK(m.n >= 2, "a leaf must hold at least two entries (got %zu)", m.n);

    /* Overwrites succeed in a full node. */
    for (size_t i = 0; i < m.n; i++)
    {
        bsl_val_t v = rnd(seed);
        CHECK(leaf_upsert(n, m.k[i], v) == BSL_UPDATED, "leaf_upsert of a present key");
        m.v[i] = v;
    }
    expect_leaf(n, &m, "after overwrites");

    /* Scans from every kind of start, with every kind of limit. */
    size_t maxes[] = { 0, 1, 2, m.n / 2, m.n - 1, m.n, m.n + 3, LEAF_MAX_ENTRIES };
    for (size_t j = 0; j < sizeof maxes / sizeof maxes[0]; j++)
    {
        expect_scan(n, &m, 0, maxes[j], "scan from 0");
        expect_scan(n, &m, 1, maxes[j], "scan from 1");
        expect_scan(n, &m, m.k[0] - 1, maxes[j], "scan below the first key");
        expect_scan(n, &m, m.k[m.n - 1] + 1, maxes[j], "scan past the last key");
        expect_scan(n, &m, BSL_KEY_MAX - 1, maxes[j], "scan from MAX - 1");
        for (size_t i = 0; i < m.n; i++)
        {
            expect_scan(n, &m, m.k[i], maxes[j], "scan from a key");
            expect_scan(n, &m, m.k[i] + 1, maxes[j], "scan between keys");
        }
    }

    /* Removal in random order, down to empty. */
    CHECK(leaf_remove(n, 3) == 0, "leaf_remove of an absent key");
    while (m.n)
    {
        bsl_key_t k = m.k[rnd(seed) % m.n];
        CHECK(leaf_remove(n, k) == 1, "leaf_remove of a present key");
        m_del(&m, k);
        if (m.n)   /* never on an empty node */
            CHECK(leaf_remove(n, k) == 0, "leaf_remove of a removed key");
        expect_leaf(n, &m, "leaf_remove");
    }
    r_functions_on_empty(n, 1);

    /* Reuse after drain. */
    leaf_insert(n, 40, 4);
    m_put(&m, 40, 4);
    expect_leaf(n, &m, "reuse after drain");
    bsl_node_free(n);
}

static void
test_internal(uint64_t *seed)
{
    for (int level = 1; level < MAX_LEVEL; level++)
    {
        bsl_node_t *n = bsl_internal_alloc(level);
        fresh_checks(n, level, "new internal node");
        r_functions_on_empty(n, 0);
        bsl_node_free(n);
    }

    bsl_node_t *n = bsl_internal_alloc(1);
    model_t m = { 0 };
    bsl_node_t *first_left = (bsl_node_t *)1;
    CHECK(internal_insert(n, 4, fake_child(4), &first_left) == BSL_INSERTED && first_left == NULL,
          "internal_insert into an empty node");
    m_put(&m, 4, (uint64_t)(uintptr_t)fake_child(4));
    expect_internal(n, &m, "internal_insert into an empty node");
    for (;;)
    {
        bsl_key_t k;
        do k = 4 * (1 + rnd(seed) % (16 * MAXE)); while (m_has(&m, k));
        bsl_node_t *left = (bsl_node_t *)1;
        int r = internal_insert(n, k, fake_child(k), &left);
        if (r == BSL_FULL)
        {
            expect_internal(n, &m, "internal_insert BSL_FULL leaves the node unchanged");
            break;
        }
        CHECK(r == BSL_INSERTED, "internal_insert returned %d", r);
        size_t i = m_lb(&m, k);
        CHECK((uint64_t)(uintptr_t)left == (i ? m.v[i - 1] : 0), "internal_insert left_child");
        m_put(&m, k, (uint64_t)(uintptr_t)fake_child(k));
        expect_internal(n, &m, "internal_insert");
        if (m.n > INTERNAL_MAX_ENTRIES) { CHECK(0, "internal node holds more than INTERNAL_MAX_ENTRIES"); break; }
    }
    CHECK(m.n >= 2, "an internal node must hold at least two entries (got %zu)", m.n);

    while (m.n)
    {
        bsl_key_t k = m.k[rnd(seed) % m.n];
        internal_remove(n, k);
        m_del(&m, k);
        expect_internal(n, &m, "internal_remove");
    }
    r_functions_on_empty(n, 0);
    bsl_node_free(n);
}

static void
test_head_nodes(void)
{
    /* A head leaf: key 0 stored like any other key; scans start at 1. */
    bsl_node_t *leaf = bsl_leaf_alloc();
    model_t m = { 0 };
    leaf_insert(leaf, BSL_KEY_MIN, 0);
    m_put(&m, 0, 0);
    expect_leaf(leaf, &m, "head leaf");
    CHECK(leaf_upsert(leaf, 5, 50) == BSL_INSERTED, "insert into a head leaf");
    m_put(&m, 5, 50);
    expect_leaf(leaf, &m, "head leaf with a key");
    expect_scan(leaf, &m, 1, LEAF_MAX_ENTRIES, "head leaf scan from 1");
    expect_scan(leaf, &m, 0, LEAF_MAX_ENTRIES, "head leaf scan from 0");
    CHECK(leaf_upsert(leaf, BSL_KEY_MAX - 1, 9) == BSL_INSERTED, "largest user key");
    m_put(&m, BSL_KEY_MAX - 1, 9);
    expect_leaf(leaf, &m, "head leaf with the largest user key");
    expect_scan(leaf, &m, BSL_KEY_MAX - 1, 5, "scan from MAX - 1");
    bsl_node_free(leaf);

    /* BSL_KEY_MAX is storable and comparable like any other key. */
    leaf = bsl_leaf_alloc();
    m.n = 0;
    leaf_insert(leaf, 1, 2);
    CHECK(leaf_upsert(leaf, BSL_KEY_MAX, 1) == BSL_INSERTED, "store BSL_KEY_MAX");
    CHECK(leaf_upsert(leaf, BSL_KEY_MAX, 3) == BSL_UPDATED, "overwrite BSL_KEY_MAX");
    m_put(&m, 1, 2);
    m_put(&m, BSL_KEY_MAX, 3);
    expect_leaf(leaf, &m, "BSL_KEY_MAX entry");
    bsl_node_free(leaf);

    /* A head internal node routes every key >= 0. */
    bsl_node_t *in = bsl_internal_alloc(2);
    bsl_node_t *left = (bsl_node_t *)1;
    CHECK(internal_insert(in, BSL_KEY_MIN, fake_child(0), &left) == BSL_INSERTED && left == NULL,
          "key-0 entry of a head internal node");
    bsl_key_t probes[] = { 0, 1, 99, 12345, BSL_KEY_MAX - 1, BSL_KEY_MAX };
    for (size_t i = 0; i < sizeof probes / sizeof probes[0]; i++)
        CHECK(internal_route(in, probes[i]) == fake_child(0), "head internal_route(%" PRIu64 ")", probes[i]);
    CHECK(internal_insert(in, 100, fake_child(100), &left) == BSL_INSERTED && left == fake_child(0),
          "insert after the key-0 entry");
    CHECK(internal_route(in, 99) == fake_child(0) && internal_route(in, 100) == fake_child(100) &&
          internal_route(in, BSL_KEY_MAX) == fake_child(100), "head internal_route with two entries");
    bsl_node_t *child = NULL;
    CHECK(internal_find(in, 0, &child, &left) == 1 && child == fake_child(0) && left == NULL,
          "internal_find of the key-0 entry");
    bsl_node_free(in);
}

static void
test_header_fields(void)
{
    bsl_node_t *a = bsl_leaf_alloc(), *b = bsl_internal_alloc(3);
    node_set_next(a, b);
    node_set_next_header(a, 777);
    CHECK(node_next(a) == b && node_next_acquire(a) == b && node_next_header(a) == 777,
          "node_set_next / node_set_next_header");
    node_set_next(a, NULL);
    node_set_next_header(a, BSL_KEY_MAX);
    CHECK(node_next(a) == NULL && node_next_header(a) == BSL_KEY_MAX, "BSL_KEY_MAX next_header");
    CHECK(node_level(b) == 3, "bsl_internal_alloc(3) level");

    NODE_WRITE_LOCK(a);
    CHECK((*node_version(a) & HOCC_WRITER_BIT) != 0, "locked version has the writer bit");
    NODE_WRITE_UNLOCK(a);
    CHECK(*node_version(a) == HOCC_INIT + HOCC_VERSION_UNIT, "unlock advances the version");
    bsl_node_free(a);
    bsl_node_free(b);
}

static void
test_split_half(uint64_t *seed, int internal)
{
    size_t cap = internal ? INTERNAL_MAX_ENTRIES : LEAF_MAX_ENTRIES;
    size_t sizes[] = { 2, 3, cap / 2 + 1, cap - 1, cap };

    for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; si++)
    {
        if (sizes[si] < 2 || sizes[si] > cap) continue;
        model_t m;
        fill_model(&m, sizes[si], seed, internal);

        /* Every pending key that the engine could add after this split. */
        for (size_t pi = 0; pi <= m.n; pi++)
        {
            bsl_node_t *src = internal ? build_internal(&m, 1) : build_leaf(&m);
            bsl_node_t *dst = internal ? bsl_internal_alloc(1) : bsl_leaf_alloc();
            bsl_key_t split = node_split_half(src, dst);

            uint32_t a = node_count(src), b = node_count(dst);
            CHECK(a >= 1 && b >= 1 && a + b == m.n,
                  "node_split_half of %zu entries gave %u + %u", m.n, a, b);
            CHECK(node_first_key(dst) == split, "node_split_half returns dst's first key");

            model_t ms = m, md = m;
            ms.n = 0; md.n = 0;
            for (size_t i = 0; i < m.n; i++)
            {
                if (m.k[i] < split) m_put(&ms, m.k[i], m.v[i]);
                else m_put(&md, m.k[i], m.v[i]);
            }
            CHECK(ms.n == a && md.n == b, "node_split_half moved a non-suffix");
            if (internal) { expect_internal(src, &ms, "split src"); expect_internal(dst, &md, "split dst"); }
            else          { expect_leaf(src, &ms, "split src");     expect_leaf(dst, &md, "split dst"); }

            /* The pending key fits in the half the engine picks. */
            if (pi > 0)
            {
                bsl_key_t   k    = (pi < m.n) ? m.k[pi] - 1 : m.k[m.n - 1] + 1;
                bsl_node_t *half = (k < split) ? src : dst;
                model_t    *mh   = (k < split) ? &ms : &md;
                if (internal)
                {
                    bsl_node_t *left;
                    CHECK(internal_insert(half, k, fake_child(k), &left) == BSL_INSERTED,
                          "internal_insert into a split half");
                    m_put(mh, k, (uint64_t)(uintptr_t)fake_child(k));
                    expect_internal(half, mh, "insert into a split half");
                }
                else
                {
                    bsl_val_t v = rnd(seed);
                    leaf_insert(half, k, v);
                    m_put(mh, k, v);
                    expect_leaf(half, mh, "leaf_insert into a split half");
                }
            }
            bsl_node_free(src);
            bsl_node_free(dst);
        }
    }
}

static void
test_split_promote(uint64_t *seed, int internal)
{
    size_t cap = internal ? INTERNAL_MAX_ENTRIES : LEAF_MAX_ENTRIES;
    size_t sizes[] = { 1, 2, cap / 2, cap };

    for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; si++)
    {
        if (sizes[si] < 1 || sizes[si] > cap) continue;
        model_t m;
        fill_model(&m, sizes[si], seed, internal);

        /* A promoted key after every position: larger than the first key. */
        for (size_t p = 0; p < m.n; p++)
        {
            bsl_key_t   key = m.k[p] + 2;
            uint64_t    val = internal ? (uint64_t)(uintptr_t)fake_child(key) : rnd(seed);
            bsl_node_t *src = internal ? build_internal(&m, 2) : build_leaf(&m);
            bsl_node_t *dst = internal ? bsl_internal_alloc(2) : bsl_leaf_alloc();

            if (internal) internal_split_promote(src, key, (bsl_node_t *)(uintptr_t)val, dst);
            else          leaf_split_promote(src, key, val, dst);

            model_t ms = m, md = m;
            ms.n = p + 1;
            md.n = 0;
            m_put(&md, key, val);
            for (size_t i = p + 1; i < m.n; i++) m_put(&md, m.k[i], m.v[i]);

            if (internal) { expect_internal(src, &ms, "promote src"); expect_internal(dst, &md, "promote dst"); }
            else          { expect_leaf(src, &ms, "promote src");     expect_leaf(dst, &md, "promote dst"); }
            CHECK(node_first_key(dst) == key, "the promoted key is dst's first key");

            bsl_node_free(src);
            bsl_node_free(dst);
        }
    }
}

static void
test_retire_link(uint64_t *seed)
{
    model_t m;
    fill_model(&m, LEAF_MAX_ENTRIES, seed, 0);
    bsl_node_t *n = build_leaf(&m);
    void **link = node_retire_link(n);
    CHECK(inside(link, sizeof *link, n, LEAF_NODE_BYTES, NULL, 0),
          "node_retire_link points outside the leaf");
    model_t mi;
    fill_model(&mi, INTERNAL_MAX_ENTRIES, seed, 1);
    bsl_node_t *in = build_internal(&mi, 1);
    void **ilink = node_retire_link(in);
    CHECK(inside(ilink, sizeof *ilink, in, INTERNAL_NODE_BYTES, NULL, 0),
          "node_retire_link points outside the internal node");

#ifdef LAYOUT_RETIRE_LINK_SHARED
    printf("  declared exception (LAYOUT_RETIRE_LINK_SHARED): the retirement link "
           "shares node memory that readers read\n");
#else
    /* No R function reads the retirement link. */
    *link = (void *)(uintptr_t)0x5a5a5a5a5a5a5a5aULL;
    expect_leaf(n, &m, "leaf after writing the retirement link");
    *ilink = (void *)(uintptr_t)0x5a5a5a5a5a5a5a5aULL;
    expect_internal(in, &mi, "internal node after writing the retirement link");
#endif
    bsl_node_free(in);
    bsl_node_free(n);
}

/* Part 2: one writer, several readers on one node */

#define RING    4096
#define READERS 3

typedef struct {
    uint64_t    tag;          /* version + 1 while valid, 0 while written */
    uint32_t    n;
    bsl_node_t *next;
    bsl_key_t   next_header;
    bsl_key_t   k[MAXE];
    uint64_t    v[MAXE];
} snap_t;

typedef struct {
    bsl_node_t *node;
    int         internal;
    snap_t     *ring;
    int         stop;
    uint64_t    seed;
    /* reader statistics */
    long        validated, invalid, locked, missed, mismatched, empty;
} rs_t;

static void
publish(rs_t *t, hocc64_t version, const model_t *m, bsl_node_t *next, bsl_key_t nh)
{
    snap_t *s = &t->ring[(version / HOCC_VERSION_UNIT) % RING];
    __atomic_store_n(&s->tag, 0, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    s->n = (uint32_t)m->n;
    s->next = next;
    s->next_header = nh;
    memcpy(s->k, m->k, m->n * sizeof m->k[0]);
    memcpy(s->v, m->v, m->n * sizeof m->v[0]);
    __atomic_store_n(&s->tag, version + 1, __ATOMIC_RELEASE);
}

/* Copy the snapshot of `version` into *out; 0 if it was overwritten. */
static int
fetch(rs_t *t, hocc64_t version, snap_t *out)
{
    snap_t *s = &t->ring[(version / HOCC_VERSION_UNIT) % RING];
    if (__atomic_load_n(&s->tag, __ATOMIC_ACQUIRE) != version + 1) return 0;
    memcpy(out, s, sizeof *out);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return __atomic_load_n(&s->tag, __ATOMIC_RELAXED) == version + 1 && out->tag == version + 1;
}

/*
 * Moves the entries of dst, split off n, back into n as the engine adds
 * entries to a node that has some: leaf_upsert or internal_insert, which may
 * answer BSL_FULL. An entry that does not fit leaves the model.
 */
static void
merge_back(bsl_node_t *n, bsl_node_t *dst, int internal, model_t *m)
{
    bsl_key_t ek[MAXE];
    if (internal)
    {
        bsl_node_t *ec[MAXE];
        size_t c = internal_entries(dst, INTERNAL_MAX_ENTRIES, ek, ec);
        for (size_t i = 0; i < c; i++)
        {
            bsl_node_t *left;
            if (internal_insert(n, ek[i], ec[i], &left) != BSL_INSERTED) m_del(m, ek[i]);
        }
    }
    else
    {
        bsl_key_t        bk[BSL_SCAN_BUFFER_LEN];
        bsl_val_t        bv[BSL_SCAN_BUFFER_LEN];
        bsl_val_t        ev[MAXE];
        const bsl_key_t *kp;
        const bsl_val_t *vp;
        size_t c = leaf_scan(dst, 0, LEAF_MAX_ENTRIES, bk, bv, &kp, &vp);
        memcpy(ek, kp, c * sizeof *kp);
        memcpy(ev, vp, c * sizeof *vp);
        for (size_t i = 0; i < c; i++)
            if (leaf_upsert(n, ek[i], ev[i]) == BSL_FULL) m_del(m, ek[i]);
    }
}

static void *
writer_main(void *arg)
{
    rs_t       *t   = arg;
    bsl_node_t *n   = t->node;
    uint64_t    s   = t->seed;
    size_t      cap = t->internal ? INTERNAL_MAX_ENTRIES : LEAF_MAX_ENTRIES;
    model_t     m   = { 0 };
    bsl_node_t *next = NULL;
    bsl_key_t   nh   = 0;

    while (!__atomic_load_n(&t->stop, __ATOMIC_RELAXED))
    {
        NODE_WRITE_LOCK(n);
        hocc64_t before = *node_version(n) & ~HOCC_WRITER_BIT;
        unsigned op = (unsigned)(rnd(&s) % 100);
        bsl_key_t k = 4 * (rnd(&s) % (3 * cap) + 1);

        if (m.n && k < m.k[0])
            k = m.k[0] + 4 * (rnd(&s) % (3 * cap));     /* never below the first key */

        if (op < 40 && m.n == 0)                        /* first entry of an empty node */
        {
            if (t->internal)
            {
                bsl_node_t *left;
                internal_insert(n, k, fake_child(k), &left);
                m_put(&m, k, (uint64_t)(uintptr_t)fake_child(k));
            }
            else
            {
                bsl_val_t v = rnd(&s);
                leaf_insert(n, k, v);
                m_put(&m, k, v);
            }
        }
        else if (op < 40)                               /* insert or overwrite */
        {
            if (t->internal)
            {
                bsl_node_t *left;
                if (!m_has(&m, k) && internal_insert(n, k, fake_child(k), &left) == BSL_INSERTED)
                    m_put(&m, k, (uint64_t)(uintptr_t)fake_child(k));
            }
            else
            {
                bsl_val_t v = rnd(&s);
                if (leaf_upsert(n, k, v) != BSL_FULL) m_put(&m, k, v);
            }
        }
        else if (op < 70 && m.n)                        /* remove */
        {
            if (op < 60) k = m.k[rnd(&s) % m.n];
            if (t->internal) { if (m_has(&m, k)) { internal_remove(n, k); m_del(&m, k); } }
            else if (leaf_remove(n, k)) m_del(&m, k);
        }
        else if (op < 77 && m.n >= 2)                   /* capacity split, then merge back */
        {
            bsl_node_t *dst = t->internal ? bsl_internal_alloc(1) : bsl_leaf_alloc();
            node_split_half(n, dst);
            merge_back(n, dst, t->internal, &m);
            bsl_node_free(dst);
        }
        else if (op < 84 && m.n >= 1 && m.n < cap)      /* promotion split, then merge back */
        {
            bsl_key_t key = m.k[rnd(&s) % m.n] + 2;
            if (!m_has(&m, key))
            {
                bsl_node_t *dst = t->internal ? bsl_internal_alloc(1) : bsl_leaf_alloc();
                if (t->internal)
                {
                    internal_split_promote(n, key, fake_child(key), dst);
                    m_put(&m, key, (uint64_t)(uintptr_t)fake_child(key));
                }
                else
                {
                    bsl_val_t v = rnd(&s);
                    leaf_split_promote(n, key, v, dst);
                    m_put(&m, key, v);
                }
                merge_back(n, dst, t->internal, &m);
                bsl_node_free(dst);
            }
        }
        else if (op < 88)                               /* drain */
        {
            while (m.n)
            {
                if (t->internal) internal_remove(n, m.k[0]);
                else leaf_remove(n, m.k[0]);
                m_del(&m, m.k[0]);
            }
        }
        else                                            /* header fields */
        {
            next = (rnd(&s) & 1) ? fake_child(k) : NULL;
            nh   = (rnd(&s) & 1) ? k : BSL_KEY_MAX;
            node_set_next(n, next);
            node_set_next_header(n, nh);
        }

        publish(t, before + HOCC_VERSION_UNIT, &m, next, nh);
        NODE_WRITE_UNLOCK(n);

        /* A short random pause, so that readers also see stable versions
         * (without it the writer holds the lock almost all the time). */
        for (unsigned i = (unsigned)(rnd(&s) % 512); i > 0; i--)
            cpu_relax();
    }
    return NULL;
}

static void *
reader_main(void *arg)
{
    rs_t       *t   = arg;
    bsl_node_t *n   = t->node;
    uint64_t    s   = t->seed ^ (uint64_t)(uintptr_t)&s;
    size_t      cap = t->internal ? INTERNAL_MAX_ENTRIES : LEAF_MAX_ENTRIES;
    snap_t     *snap = malloc(sizeof *snap);
    model_t    *m    = malloc(sizeof *m);
    long validated = 0, invalid = 0, locked = 0, missed = 0, mismatched = 0, empty = 0;

    while (!__atomic_load_n(&t->stop, __ATOMIC_RELAXED))
    {
        hocc64_t v  = NODE_LOAD_VERSION(n);
        unsigned fn = (unsigned)(rnd(&s) % 8);
        bsl_key_t k = (rnd(&s) % 4 == 0) ? rnd(&s) % (16 * cap) : 4 * (rnd(&s) % (3 * cap) + 1) + (rnd(&s) % 3);

        /* Results, captured before validation. */
        int              r_found = 0;
        bsl_val_t        r_val = 0;
        size_t           r_count = 0, r_max = 0;
        bsl_key_t        r_keys[LEAF_MAX_ENTRIES + 2];
        bsl_val_t        r_vals[LEAF_MAX_ENTRIES + 2];
        bsl_node_t      *r_ptr = NULL;
        bsl_key_t        r_key = 0;
        int              r_level = 0;

        if (fn == 0)      r_ptr = node_next(n);
        else if (fn == 1) r_ptr = node_next_acquire(n);
        else if (fn == 2) r_key = node_next_header(n);
        else if (fn == 3) r_level = node_level(n);
        else if (!t->internal && fn < 6)
            r_found = leaf_lookup(n, k, &r_val);
        else if (!t->internal)
        {
            bsl_key_t        bk[BSL_SCAN_BUFFER_LEN];   /* exact size: ASan sees overruns */
            bsl_val_t        bv[BSL_SCAN_BUFFER_LEN];
            const bsl_key_t *kp = NULL;
            const bsl_val_t *vp = NULL;
            r_max   = rnd(&s) % (cap + 3);
            r_count = leaf_scan(n, k, r_max, bk, bv, &kp, &vp);
            /* Checked whether or not the read validates. */
            CHECK(r_count <= r_max, "leaf_scan handed out %zu entries, max %zu", r_count, r_max);
            if (r_count > r_max) continue;
            if (r_count)
            {
                int ok = inside(kp, r_count * sizeof *kp, n, LEAF_NODE_BYTES, bk, SCAN_BUF_KEY_BYTES) &&
                         inside(vp, r_count * sizeof *vp, n, LEAF_NODE_BYTES, bv, SCAN_BUF_VAL_BYTES);
                CHECK(ok, "leaf_scan handed out memory outside the node and the declared buffers");
                if (!ok) continue;
                memcpy(r_keys, kp, r_count * sizeof *kp);
                memcpy(r_vals, vp, r_count * sizeof *vp);
            }
        }
        else if (fn < 6)
            r_ptr = internal_route(n, k);
        else
            r_ptr = internal_first_child(n);

        if (v & HOCC_WRITER_BIT) { locked++; continue; }

        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (!NODE_VALIDATE(n, v)) { invalid++; continue; }
        if (!fetch(t, v, snap)) { missed++; continue; }
        validated++;

        /* Entry functions on an empty node have no specified answer. */
        if (snap->n == 0 && fn >= 4) { empty++; continue; }

        m->n = snap->n;
        memcpy(m->k, snap->k, m->n * sizeof m->k[0]);
        memcpy(m->v, snap->v, m->n * sizeof m->v[0]);

        int bad = 0;
        if (fn == 0 || fn == 1)      bad = (r_ptr != snap->next);
        else if (fn == 2)            bad = (r_key != snap->next_header);
        else if (fn == 3)            bad = (r_level != (t->internal ? 1 : 0));
        else if (!t->internal && fn < 6)
        {
            size_t i = m_lb(m, k);
            int present = i < m->n && m->k[i] == k;
            bad = (r_found != present) || (present && r_val != m->v[i]);
        }
        else if (!t->internal)
        {
            size_t first = m_lb(m, k), want = m->n - first;
            if (r_max < want) want = r_max;
            bad = (r_count != want);
            for (size_t i = 0; !bad && i < r_count; i++)
                bad = (r_keys[i] != m->k[first + i] || r_vals[i] != m->v[first + i]);
        }
        else if (fn < 6)
        {
            /* Defined only when some boundary key is <= k. */
            if (m->n && m->k[0] <= k) bad = ((uint64_t)(uintptr_t)r_ptr != m_route(m, k));
        }
        else if (m->n)
            bad = ((uint64_t)(uintptr_t)r_ptr != m->v[0]);

        if (bad)
        {
            mismatched++;
            CHECK(0, "validated read of R function %u (key %" PRIu64 ") does not match the model of version %" PRIu64,
                  fn, k, (uint64_t)v);
        }
    }

    __atomic_fetch_add(&t->validated, validated, __ATOMIC_RELAXED);
    __atomic_fetch_add(&t->invalid, invalid, __ATOMIC_RELAXED);
    __atomic_fetch_add(&t->locked, locked, __ATOMIC_RELAXED);
    __atomic_fetch_add(&t->missed, missed, __ATOMIC_RELAXED);
    __atomic_fetch_add(&t->mismatched, mismatched, __ATOMIC_RELAXED);
    __atomic_fetch_add(&t->empty, empty, __ATOMIC_RELAXED);
    free(snap);
    free(m);
    return NULL;
}

static void
test_reader_safety(int internal, long ms)
{
    rs_t t;
    memset(&t, 0, sizeof t);
    t.internal = internal;
    t.node     = internal ? bsl_internal_alloc(1) : bsl_leaf_alloc();
    t.ring     = calloc(RING, sizeof(snap_t));
    t.seed     = internal ? 0x1234 : 0x5678;
    model_t empty = { 0 };
    publish(&t, HOCC_INIT, &empty, NULL, 0);

    pthread_t w, r[READERS];
    pthread_create(&w, NULL, writer_main, &t);
    for (int i = 0; i < READERS; i++) pthread_create(&r[i], NULL, reader_main, &t);

    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
    __atomic_store_n(&t.stop, 1, __ATOMIC_RELAXED);
    pthread_join(w, NULL);
    for (int i = 0; i < READERS; i++) pthread_join(r[i], NULL);

    printf("  %s node: %ld validated reads (%ld mismatched, %ld of an empty node not compared), "
           "%ld failed validation, %ld on a locked node, %ld without a snapshot\n",
           internal ? "internal" : "leaf", t.validated, t.mismatched, t.empty, t.invalid, t.locked, t.missed);
    CHECK(t.validated > 1000, "too few validated reads (%ld) to test anything", t.validated);
    CHECK(t.invalid + t.locked > 0, "no read overlapped a write");

    bsl_node_free(t.node);
    free(t.ring);
}

int
main(int argc, char **argv)
{
    long ms = argc > 1 ? atol(argv[1]) : 700;
    uint64_t seed = 20261117;

    printf("layout %s: leaf %zu bytes (max %zu entries), internal %zu bytes (max %zu entries)\n",
           LAYOUT_NAME, (size_t)LEAF_NODE_BYTES, (size_t)LEAF_MAX_ENTRIES,
           (size_t)INTERNAL_NODE_BYTES, (size_t)INTERNAL_MAX_ENTRIES);

    printf("part 1: function behaviour\n");
    test_header_fields();
    test_leaf(&seed);
    test_internal(&seed);
    test_head_nodes();
    test_split_half(&seed, 0);
    test_split_half(&seed, 1);
    test_split_promote(&seed, 0);
    test_split_promote(&seed, 1);
    test_retire_link(&seed);
    long part1 = failures;
    printf("  %ld checks, %ld failures\n", checks, part1);

    printf("part 2: reader safety, %ld ms per node kind\n", ms);
    test_reader_safety(0, ms);
    test_reader_safety(1, ms);

    printf("test_layout %s: %s (%ld failures)\n", LAYOUT_NAME, failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
