/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/* blocks.c -- ranged block cache of httpfs (fetch/read/release). */

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
#include "http.h"
/* ------------------------------------------------------------------
 * The resource engine (directory nodes)
 * ------------------------------------------------------------------ */

/*
 * ensure_metadata — run the HEAD probe of NN's resource, once.
 *
 * The result (status, size, filetime, raw headers) is sticky: it is
 * remembered for the whole life of the node.  Transport-level
 * failures are recorded in meta_err; HTTP statuses, whatever they
 * are, are information, not errors.
 */
static int ensure_metadata (struct netnode *nn)
{
    int err;

    pthread_mutex_lock (&nn->res_lock);

    if (! nn->meta_done)
    {
        err = http_head (nn->url, &nn->probe);
        nn->meta_err = err;
        nn->meta_done = true;
        if (err == 0)
        {
            nn->size = nn->probe.size_total;
            nn->state = RES_PROBED;   /* content mode now decidable */
        }
    }
    else
        err = nn->meta_err;

    pthread_mutex_unlock (&nn->res_lock);
    return err;
}

/*
 * install_block — put a fetched window into the block cache,
 * evicting the oldest blocks when the per-node budget is exceeded.
   NN's res_lock must be held; BLK ownership is transferred to the
   cache on success.  Returns 0, or ENOMEM after freeing BLK.
 */
static int install_block (struct netnode *nn, struct httpfs_block *blk)
{
    if (nn->blocks == nullptr
        && hurd_ihash_create (&nn->blocks, HURD_IHASH_NO_LOCP) != 0)
    {
        free (blk->data);
        free (blk);
        return ENOMEM;
    }

    hurd_ihash_add (nn->blocks, (hurd_ihash_key_t) blk->idx, blk);
    blk->older = nn->block_newest;
    blk->newer = nullptr;
    if (nn->block_newest != nullptr)
        nn->block_newest->newer = blk;
    else
        nn->block_oldest = blk;
    nn->block_newest = blk;
    nn->block_bytes += blk->len;

    /* Evict from the FIFO front while the budget is exceeded.  The
       block just installed is at the back, so it survives as long
       as the budget covers at least one block — which it does. */
    while (nn->block_bytes > HTTPFS_BLOCK_CACHE_BYTES
           && nn->block_oldest != nullptr)
    {
        struct httpfs_block *victim = nn->block_oldest;

        hurd_ihash_remove (nn->blocks, (hurd_ihash_key_t) victim->idx);
        nn->block_oldest = victim->newer;
        if (nn->block_oldest != nullptr)
            nn->block_oldest->older = nullptr;
        else
            nn->block_newest = nullptr;
        nn->block_bytes -= victim->len;

        free (victim->data);
        free (victim);
    }

    return 0;
}

/*
 * fetch_block — obtain the contents of block IDX of NN's resource.
   NN's res_lock must be held.  Returns 0 with *BLK pointing to the
   cached block (owned by the cache), or an errno.
 *
 * A 416 answer means the window lies beyond the resource: an empty
 * block is served, which read_blocks interprets as end of file.
 */
static int fetch_block (struct netnode *nn, uint64_t idx,
                        struct httpfs_block **blk)
{
    struct httpfs_block *b = hurd_ihash_find (nn->blocks,
                                              (hurd_ihash_key_t) idx);
    if (b != nullptr)
    {
        *blk = b;
        return 0;                   /* cache hit */
    }

    /* The window of block IDX: [idx*BS, min(size, (idx+1)*BS)).
       When the total size is unknown, ask for a full block and let
       a short answer mark the end of the resource.  A seek near the
       top of the address space would make idx*BS wrap: treat that as
       beyond the resource (empty block = EOF), never as block 0. */
    if (idx > (UINT64_MAX - HTTPFS_BLOCK_SIZE) / HTTPFS_BLOCK_SIZE)
      {
        int e;

        b = malloc (sizeof *b);
        if (b == nullptr)
            return ENOMEM;
        b->idx = idx;
        b->len = 0;
        b->data = nullptr;
        b->older = b->newer = nullptr;
        e = install_block (nn, b);
        if (e != 0)
            return e;
        *blk = b;
        return 0;
      }
    size_t start = (size_t) (idx * HTTPFS_BLOCK_SIZE);
    size_t end = start + HTTPFS_BLOCK_SIZE - 1;
    if (nn->size >= 0 && (long long) end >= nn->size)
        end = (size_t) nn->size - 1;

    struct http_response resp = { nullptr, 0, nullptr, 0, -1, -1 };
    int err = http_fetch_range (nn->url, start, end, &resp);
    if (err != 0)
        return err;                 /* transport failure */

    if (resp.status == 416)
    {
        /* Beyond the resource: the caller learns the true size. */
        http_response_release (&resp);
        b = malloc (sizeof *b);
        if (b == nullptr)
            return ENOMEM;
        b->idx = idx;
        b->len = 0;
        b->data = nullptr;
        b->older = b->newer = nullptr;
        err = install_block (nn, b);
        if (err != 0)
            return err;
        *blk = b;
        return 0;
    }

    if (resp.status != 206)
    {
        /* The server changed its mind mid-stream (it answered 206
           for the very first probe, but not now).  This is not
           something a read can recover from: report an error. */
        http_response_release (&resp);
        return EIO;
    }

    b = malloc (sizeof *b);
    if (b == nullptr)
    {
        http_response_release (&resp);
        return ENOMEM;
    }
    b->idx = idx;
    b->len = resp.body_len;
    b->data = resp.body;           /* ownership moves to the cache  */
    b->older = b->newer = nullptr;
    resp.body = nullptr;
    http_response_release (&resp);  /* frees only the headers now */

    err = install_block (nn, b);
    if (err != 0)
        return err;

    *blk = b;
    return 0;
}

/*
 * ensure_content — determine how the resource must be read.
   NN's res_lock must be held.
 *
 *   RES_PROBED + first read:
 *     ranged GET of the first block:
 *       206 -> RES_BLOCKS (and the block is already cached);
 *       200 -> RES_FULLBODY: the server ignores Range, and the
 *              body just received IS the whole document;
 *       >=400 -> RES_FULLBODY with the error page as the body;
 *       transport error -> RES_ERROR (sticky).
 */
static int ensure_content (struct netnode *nn)
{
    int err;

    if (nn->state == RES_PROBED)
    {
        if (nn->size == 0)
        {
            /* Empty resource: nothing to fetch, serve zeroes... we
               simply complete with an empty full-body response. */
            nn->full = (struct http_response) { nullptr, 0, nullptr,
                                                nn->probe.status, 0, -1 };
            nn->state = RES_FULLBODY;
            return 0;
        }

        struct http_response resp = { nullptr, 0, nullptr, 0, -1, -1 };
        err = http_fetch_range (nn->url, 0, HTTPFS_BLOCK_SIZE - 1, &resp);
        if (err != 0)
        {
            nn->content_err = err;
            nn->state = RES_ERROR;
            return err;
        }

        if (resp.status == 206)
        {
            /* Windowed access from now on. */
            long long total = resp.size_total;
            struct httpfs_block *b = malloc (sizeof *b);
            if (b == nullptr)
            {
                http_response_release (&resp);
                nn->content_err = ENOMEM;
                nn->state = RES_ERROR;
                return ENOMEM;
            }
            b->idx = 0;
            b->len = resp.body_len;
            b->data = resp.body;
            b->older = b->newer = nullptr;
            resp.body = nullptr;
            http_response_release (&resp);

            nn->state = RES_BLOCKS;
            if (nn->size < 0 && total > 0)
                nn->size = total;
            install_block (nn, b);
            return 0;
        }

        /* 200 (server ignored the Range header) or any other
           status: keep the whole body and read from it forever. */
        nn->full = resp;
        if (nn->size < 0)
            nn->size = resp.size_total > 0 ? resp.size_total
                                           : (long long) resp.body_len;
        nn->state = RES_FULLBODY;
        return 0;
    }

    return nn->state == RES_ERROR ? nn->content_err : 0;
}

/*
 * resource_size — the size of NN's readable content, in bytes.
   Triggers ensure_metadata and, when the size cannot be known
   without it, ensure_content.
 */
long long resource_size (struct netnode *nn)
{
    int err = ensure_metadata (nn);
    if (err != 0)
        return -1;

    pthread_mutex_lock (&nn->res_lock);

    long long size = nn->size;
    if (size < 0 || nn->probe.status >= 400)
    {
        /* Unknown size, or an error page whose length only a GET
           reveals: run the content engine once. */
        if (nn->state == RES_PROBED)
            err = ensure_content (nn);
        else if (nn->state == RES_ERROR)
            err = nn->content_err;

        if (nn->state == RES_FULLBODY)
            size = (long long) nn->full.body_len;
        else if (nn->state == RES_BLOCKS)
            size = nn->size;
        else
            size = err == 0 ? 0 : -1;
    }

    pthread_mutex_unlock (&nn->res_lock);
    return size;
}

/*
 * read_blocks — copy the window [OFFSET, OFFSET+LEN) of NN's
   resource into BUF, fetching only the blocks it touches.
   Returns 0 and sets *LEN to the number of bytes copied (0 = EOF).
 */
int read_blocks (struct netnode *nn, uint64_t offset, size_t *len,
                        char *buf)
{
    uint64_t remaining = *len;
    uint64_t done = 0;

    /* Clip to the end of the resource when its size is known. */
    if (nn->size >= 0)
    {
        uint64_t size = (uint64_t) nn->size;
        if (offset >= size)
        {
            *len = 0;
            return 0;               /* EOF */
        }
        if (offset + remaining > size)
            remaining = size - offset;
    }

    while (remaining > 0)
    {
        uint64_t idx = (offset + done) / HTTPFS_BLOCK_SIZE;
        size_t skip = (size_t) ((offset + done) % HTTPFS_BLOCK_SIZE);
        struct httpfs_block *blk = nullptr;
        int err = fetch_block (nn, idx, &blk);
        if (err != 0)
        {
            nn->content_err = err;
            nn->state = RES_ERROR;
            if (done == 0)
            {
                *len = 0;
                return err;
            }
            break;                  /* serve what we already have */
        }

        if (blk->len <= skip)
        {
            /* The server returned a short block: end of resource. */
            if (nn->size < 0)
                nn->size = (long long) (idx * HTTPFS_BLOCK_SIZE
                                        + blk->len);
            break;
        }

        size_t n = blk->len - skip;
        if (n > remaining)
            n = (size_t) remaining;
        memcpy (buf + done, blk->data + skip, n);
        done += n;
        remaining -= n;

        /* A short block is the last one: stop here.  (When the size
           was known the read has been clipped already, so this only
           saves a useless request when it was not.) */
        if (blk->len < HTTPFS_BLOCK_SIZE)
        {
            if (nn->size < 0)
                nn->size = (long long) (idx * HTTPFS_BLOCK_SIZE + blk->len);
            break;
        }
    }

    *len = (size_t) done;
    return 0;
}

/*
 * release_resource — free everything NN's resource holds.
   Called from netfs_node_norefs with no lock held.
 */
void release_resource (struct netnode *nn)
{
    pthread_mutex_lock (&nn->res_lock);

    if (nn->blocks != nullptr)
    {
        struct httpfs_block *b = nn->block_oldest;
        while (b != nullptr)
        {
            struct httpfs_block *next = b->newer;
            free (b->data);
            free (b);
            b = next;
        }
        hurd_ihash_destroy (nn->blocks);
        hurd_ihash_free (nn->blocks);
        nn->blocks = nullptr;
    }
    nn->block_oldest = nn->block_newest = nullptr;
    nn->block_bytes = 0;

    http_response_release (&nn->probe);
    http_response_release (&nn->full);

    pthread_mutex_unlock (&nn->res_lock);
}
