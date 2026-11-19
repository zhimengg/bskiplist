#ifndef BSL_LAYOUT_CONTRACT_CHECK_H
#define BSL_LAYOUT_CONTRACT_CHECK_H

/*
 * layout_kit/contract_check.h - compile-time check that the layout defines
 * every contract function, as a function (not a macro), with this type.
 */

#define BSL_CONTRACT_FN(fn, type)                                          \
    _Static_assert(__builtin_types_compatible_p(__typeof__(&fn), type),    \
                   "layout contract: " #fn " must have type " #type)

/* Header fields. R: called by readers without the lock. */
BSL_CONTRACT_FN(node_version,          hocc64_t *(*)(bsl_node_t *));              /* R */
BSL_CONTRACT_FN(node_level,            int (*)(bsl_node_t *));                    /* R */
BSL_CONTRACT_FN(node_next,             bsl_node_t *(*)(bsl_node_t *));            /* R */
BSL_CONTRACT_FN(node_next_acquire,     bsl_node_t *(*)(bsl_node_t *));            /* R */
BSL_CONTRACT_FN(node_set_next,         void (*)(bsl_node_t *, bsl_node_t *));
BSL_CONTRACT_FN(node_next_header,      bsl_key_t (*)(bsl_node_t *));              /* R */
BSL_CONTRACT_FN(node_set_next_header,  void (*)(bsl_node_t *, bsl_key_t));
BSL_CONTRACT_FN(node_count,            uint32_t (*)(bsl_node_t *));
BSL_CONTRACT_FN(node_first_key,        bsl_key_t (*)(bsl_node_t *));

/* Leaf entries. */
BSL_CONTRACT_FN(leaf_lookup,           int (*)(bsl_node_t *, bsl_key_t, bsl_val_t *));   /* R */
BSL_CONTRACT_FN(leaf_scan,             size_t (*)(bsl_node_t *, bsl_key_t, size_t,
                                                  bsl_key_t *, bsl_val_t *,
                                                  const bsl_key_t **, const bsl_val_t **)); /* R */
BSL_CONTRACT_FN(leaf_upsert,           int (*)(bsl_node_t *, bsl_key_t, bsl_val_t));
BSL_CONTRACT_FN(leaf_insert,           void (*)(bsl_node_t *, bsl_key_t, bsl_val_t));
BSL_CONTRACT_FN(leaf_remove,           int (*)(bsl_node_t *, bsl_key_t));

/* Internal entries. */
BSL_CONTRACT_FN(internal_route,        bsl_node_t *(*)(bsl_node_t *, bsl_key_t));       /* R */
BSL_CONTRACT_FN(internal_first_child,  bsl_node_t *(*)(bsl_node_t *));                  /* R */
BSL_CONTRACT_FN(internal_last_child,   bsl_node_t *(*)(bsl_node_t *));
BSL_CONTRACT_FN(internal_find,         int (*)(bsl_node_t *, bsl_key_t,
                                               bsl_node_t **, bsl_node_t **));
BSL_CONTRACT_FN(internal_insert,       int (*)(bsl_node_t *, bsl_key_t, bsl_node_t *,
                                               bsl_node_t **));
BSL_CONTRACT_FN(internal_remove,       void (*)(bsl_node_t *, bsl_key_t));
BSL_CONTRACT_FN(internal_entries,      size_t (*)(bsl_node_t *, size_t, bsl_key_t *,
                                                  bsl_node_t **));

/* Splits. */
BSL_CONTRACT_FN(leaf_split_promote,     void (*)(bsl_node_t *, bsl_key_t, bsl_val_t, bsl_node_t *));
BSL_CONTRACT_FN(internal_split_promote, void (*)(bsl_node_t *, bsl_key_t, bsl_node_t *, bsl_node_t *));
BSL_CONTRACT_FN(node_split_half,        bsl_key_t (*)(bsl_node_t *, bsl_node_t *));

/* Lifecycle and reporting. */
BSL_CONTRACT_FN(bsl_leaf_alloc,        bsl_node_t *(*)(void));
BSL_CONTRACT_FN(bsl_internal_alloc,    bsl_node_t *(*)(int));
BSL_CONTRACT_FN(bsl_node_free,         void (*)(bsl_node_t *));
BSL_CONTRACT_FN(node_retire_link,      void **(*)(bsl_node_t *));
BSL_CONTRACT_FN(node_fill,             void (*)(bsl_node_t *, size_t *, size_t *));

/* Constants. */
#ifndef LAYOUT_NAME
#  error "layout contract: LAYOUT_NAME (a string literal) is not defined"
#endif
#ifndef LEAF_MAX_ENTRIES
#  error "layout contract: LEAF_MAX_ENTRIES is not defined"
#endif
#ifndef INTERNAL_MAX_ENTRIES
#  error "layout contract: INTERNAL_MAX_ENTRIES is not defined"
#endif
#ifndef LEAF_SCAN_BUFFER_ENTRIES
#  error "layout contract: LEAF_SCAN_BUFFER_ENTRIES is not defined (0 for a layout whose leaf_scan only hands out views)"
#endif
#ifndef LEAF_NODE_BYTES
#  error "layout contract: LEAF_NODE_BYTES (the bytes of a leaf's memory) is not defined"
#endif
#ifndef INTERNAL_NODE_BYTES
#  error "layout contract: INTERNAL_NODE_BYTES (the bytes of an internal node's memory) is not defined"
#endif

_Static_assert(sizeof(LAYOUT_NAME "") > 1, "layout contract: LAYOUT_NAME must be a non-empty string literal");
_Static_assert(LEAF_MAX_ENTRIES > 0, "layout contract: LEAF_MAX_ENTRIES must be positive");
_Static_assert(INTERNAL_MAX_ENTRIES > 0, "layout contract: INTERNAL_MAX_ENTRIES must be positive");
_Static_assert(LEAF_NODE_BYTES > 0 && INTERNAL_NODE_BYTES > 0, "layout contract: node sizes must be positive");
_Static_assert(LEAF_SCAN_BUFFER_ENTRIES == 0 || LEAF_SCAN_BUFFER_ENTRIES >= LEAF_MAX_ENTRIES,
               "layout contract: LEAF_SCAN_BUFFER_ENTRIES must be 0 or at least LEAF_MAX_ENTRIES");

#undef BSL_CONTRACT_FN

#endif /* BSL_LAYOUT_CONTRACT_CHECK_H */
