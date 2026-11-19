#include "bskiplist.h"
#include "epoch.h"
#include "node.h"
#include <stdlib.h>

bsl_t* bsl_new(void)
{
    int i, j;

    bsl_t *list = (bsl_t*)malloc(sizeof(bsl_t));
    if (!list) return NULL;

    /* level 0：leaf sentinel */
    bsl_node_t *leaf_sentinel = bsl_leaf_alloc();
    if (!leaf_sentinel)
    {
        free(list);
        return NULL;
    }

    node_set_next(leaf_sentinel, NULL);
    node_set_next_header(leaf_sentinel, BSL_KEY_MAX);
    leaf_insert(leaf_sentinel, BSL_KEY_MIN, 0);
    list->headers[0] = leaf_sentinel;

    for (i = 1; i < MAX_LEVEL; i++)
    {
        bsl_node_t *s = bsl_internal_alloc(i);
        if (!s)
        {
            for (j = 0; j < i; j++)
                bsl_node_free(list->headers[j]);
            free(list);
            return NULL;
        }

        bsl_node_t *left;
        node_set_next(s, NULL);
        node_set_next_header(s, BSL_KEY_MAX);
        internal_insert(s, BSL_KEY_MIN, list->headers[i - 1], &left);
        list->headers[i]      = s;
    }

    return list;
}


void bsl_destroy(bsl_t *list)
{
    if (!list) return;

    epoch_enter();
    epoch_exit();

    ebr_sync();

    for (int l = 0; l < MAX_LEVEL; l++)
    {
        bsl_node_t *curr = list->headers[l];
        while (curr)
        {
            bsl_node_t *next = node_next(curr);
            bsl_node_free(curr);
            curr = next;
        }
    }

    free(list);
}