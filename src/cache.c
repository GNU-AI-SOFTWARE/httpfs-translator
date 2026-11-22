/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/* cache.c -- node tree and MRU cache of httpfs. */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include "mach-shim.h"
#include <hurd/netfs.h>
#include <hurd/iohelp.h>

#include "httpfs.h"
#include "netfs_internal.h"
/* ------------------------------------------------------------------
 * The global node cache
 * ------------------------------------------------------------------ */

/* One MRU list of nodes, each holding one hard reference.  Only
   cache_node takes node_cache_lock, always with the *new* node
   locked; the eviction path may therefore block on the LRU
   victim's locks without any risk of inversion. */
static pthread_mutex_t node_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static struct node *cache_mru = nullptr;
static struct node *cache_lru = nullptr;
static int cache_len = 0;

/* cache_node — remember NP (locked, referenced) in the MRU cache
   and evict the oldest entries beyond HTTPFS_NODE_CACHE_MAX. */
void cache_node (struct node *np)
{
    struct netnode *nn = np->nn;

    pthread_mutex_lock (&node_cache_lock);

    nn->cache_prev = nullptr;
    nn->cache_next = cache_mru;
    if (cache_mru != nullptr)
        cache_mru->nn->cache_prev = np;
    cache_mru = np;
    if (cache_lru == nullptr)
        cache_lru = np;
    netfs_nref (np);               /* the cache's own reference */
    cache_len++;

    /* Evict the oldest entries beyond the maximum. */
    while (cache_len > HTTPFS_NODE_CACHE_MAX)
    {
        struct node *victim = cache_lru;
        struct netnode *vn = victim->nn;

        if (vn->cache_prev != nullptr)
            vn->cache_prev->nn->cache_next = vn->cache_next;
        else
            cache_mru = vn->cache_next;
        if (vn->cache_next != nullptr)
            vn->cache_next->nn->cache_prev = vn->cache_prev;
        else
            cache_lru = vn->cache_prev;
        vn->cache_prev = vn->cache_next = nullptr;
        cache_len--;

        netfs_nrele (victim);     /* may free it, may not */
    }

    pthread_mutex_unlock (&node_cache_lock);
}

/* ------------------------------------------------------------------
 * Node creation and the child list
 * ------------------------------------------------------------------ */

/*
 * create_child — create inside DIR (locked) a node NAME of kind
   KIND.  On output: *NP designates the new node, LOCKED and holding
   one reference.  The child is entered in DIR's child list and in
   the global node cache.  Returns 0 or an errno code.
 */
error_t create_child (struct node *dir, const char *name,
                             enum httpfs_kind kind, struct node **np)
{
    struct netnode *nn = malloc (sizeof *nn);
    struct netnode *dn = dir->nn;

    *np = nullptr;
    if (nn == nullptr)
        return ENOMEM;

    /* Fields common to every kind. */
    nn->kind = kind;
    nn->name = strdup (name);
    nn->parent = dn;
    nn->node = nullptr;
    nn->children = nullptr;
    nn->sibling_prev = nn->sibling_next = nullptr;
    nn->cache_prev = nn->cache_next = nullptr;
    pthread_mutex_init (&nn->child_lock, nullptr);

    nn->meta_done = false;
    nn->meta_err = 0;
    nn->probe = (struct http_response) { nullptr, 0, nullptr, 0, -1, -1 };
    nn->state = RES_UNKNOWN;
    nn->content_err = 0;
    nn->full = (struct http_response) { nullptr, 0, nullptr, 0, -1, -1 };
    nn->size = -1;
    nn->blocks = nullptr;
    nn->block_oldest = nn->block_newest = nullptr;
    nn->block_bytes = 0;
    pthread_mutex_init (&nn->res_lock, nullptr);

    /* A URL is only defined for directories; the views refer to
       their parent's resource. */
    nn->url = (kind == HTTPFS_DIR) ? http_url_join (dn->url, name) : nullptr;

    if (nn->name == nullptr || (kind == HTTPFS_DIR && nn->url == nullptr))
        goto memerr;

    /* The child keeps its parent alive. */
    netfs_nref (dir);

    *np = netfs_make_node (nn);
    if (*np == nullptr)
    {
        netfs_nrele (dir);
        goto memerr;
    }
    nn->node = *np;

    /* libnetfs contract: the found node must be locked.  The node
       is brand new and unreachable, so this cannot block. */
    pthread_mutex_lock (&(*np)->lock);

    /* Enter the child in the parent's list. */
    pthread_mutex_lock (&dn->child_lock);
    nn->sibling_next = dn->children;
    if (dn->children != nullptr)
        dn->children->sibling_prev = nn;
    dn->children = nn;
    pthread_mutex_unlock (&dn->child_lock);

    /* Keep the node alive for future lookups. */
    cache_node (*np);

    return 0;

memerr:
    free (nn->name);
    free (nn->url);
    pthread_mutex_destroy (&nn->res_lock);
    pthread_mutex_destroy (&nn->child_lock);
    free (nn);
    return ENOMEM;
}

/*
 * find_child — return the live child of DN named NAME, or NULL.
   DN's child_lock must NOT be held; it is taken here.
 */
struct netnode *find_child (struct netnode *dn, const char *name)
{
    struct netnode *cn;

    pthread_mutex_lock (&dn->child_lock);
    for (cn = dn->children; cn != nullptr; cn = cn->sibling_next)
        if (strcmp (cn->name, name) == 0)
            break;
    pthread_mutex_unlock (&dn->child_lock);
    return cn;
}

/*
 * unlink_child — remove NN from its parent's child list.
   Called from netfs_node_norefs with the dying node's libnetfs lock
   held; taking the parent's child_lock is the only lock needed.
 */
void unlink_child (struct netnode *nn)
{
    struct netnode *parent = nn->parent;

    pthread_mutex_lock (&parent->child_lock);
    if (nn->sibling_prev != nullptr)
        nn->sibling_prev->sibling_next = nn->sibling_next;
    else
        parent->children = nn->sibling_next;
    if (nn->sibling_next != nullptr)
        nn->sibling_next->sibling_prev = nn->sibling_prev;
    nn->sibling_prev = nn->sibling_next = nullptr;
    pthread_mutex_unlock (&parent->child_lock);
}
