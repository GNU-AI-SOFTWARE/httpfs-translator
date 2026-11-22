/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * netfs.c — The heart of the httpfs translator: libnetfs callbacks.
 *
 * NAVIGATION MODEL
 * ----------------
 *   POSIX directory            <->  (virtual, unbounded) URL path
 *   /.../content               <->  raw body of the response
 *   /.../headers               <->  raw HTTP headers (of the probe)
 *   /.../status                <->  HTTP code ("200\n")
 *
 * PHASE 2
 * -------
 *   1. Node cache.  Every child created by a lookup is remembered
 *      in its parent's child list and kept alive by a global MRU
 *      cache of HTTPFS_NODE_CACHE_MAX entries (the ftpfs pattern:
 *      the cache holds hard references, eviction releases them).
 *      Re-opening a path finds the same node again and reuses its
 *      downloaded resource.
 *
 *   2. Range streaming.  A directory's resource is a small state
 *      machine (see httpfs.h):
 *
 *        ensure_metadata : one HEAD -> status, size, Last-Modified
 *        ensure_content  : one ranged GET on first read
 *                            206 -> RES_BLOCKS: reads fetch only the
 *                                    64 KiB windows they touch
 *                            200 -> RES_FULLBODY: server ignores
 *                                    Range; whole body in memory
 *
 * LOCKS AND REFERENCES
 * --------------------
 *   np->lock         libnetfs node lock (callbacks see it locked)
 *   nn->res_lock     protects the resource state machine
 *   nn->child_lock   protects the parent's child list
 *   node_cache_lock  protects the global MRU cache
 *
 *   Locking order:  node np->lock  ->  child_lock / res_lock /
 *   node_cache_lock.  Nothing ever takes an np->lock while holding
 *   a child_lock, and the node cache is only touched while the
 *   *newly created* node is locked (never the LRU victim), so the
 *   eviction cascade can never deadlock on a lock we hold.
 *
 *   Every child holds one hard reference on its parent node (taken
 *   in netfs_attempt_lookup, returned in netfs_node_norefs); the
 *   traversal reference held by libnetfs guarantees that the
 *   eviction cascade can never free the directory being looked
 *   through.
 */

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
error_t httpfs_init (struct netnode *root, const char *base_url)
{
    CURLcode rc = curl_global_init (CURL_GLOBAL_DEFAULT);
    if (rc != CURLE_OK)
        return EIO;

    root->kind = HTTPFS_DIR;
    root->name = strdup ("/");
    root->url = strdup (base_url);
    root->parent = nullptr;
    root->node = nullptr;
    root->children = nullptr;
    root->sibling_prev = root->sibling_next = nullptr;
    root->cache_prev = root->cache_next = nullptr;

    root->meta_done = false;
    root->meta_err = 0;
    root->probe = (struct http_response) { nullptr, 0, nullptr, 0, -1, -1 };
    root->state = RES_UNKNOWN;
    root->content_err = 0;
    root->full = (struct http_response) { nullptr, 0, nullptr, 0, -1, -1 };
    root->size = -1;
    root->blocks = nullptr;
    root->block_oldest = root->block_newest = nullptr;
    root->block_bytes = 0;

    if (root->name == nullptr || root->url == nullptr)
    {
        free (root->name);
        free (root->url);
        curl_global_cleanup ();
        return ENOMEM;
    }

    if (pthread_mutex_init (&root->child_lock, nullptr) != 0
        || pthread_mutex_init (&root->res_lock, nullptr) != 0)
    {
        pthread_mutex_destroy (&root->child_lock);
        pthread_mutex_destroy (&root->res_lock);
        curl_global_cleanup ();
        free (root->name);
        free (root->url);
        return ENOMEM;
    }

    return 0;
}

error_t httpfs_destroy (struct netnode *root)
{
    release_resource (root);
    free (root->name);
    free (root->url);
    root->name = nullptr;
    root->url = nullptr;

    pthread_mutex_destroy (&root->res_lock);
    pthread_mutex_destroy (&root->child_lock);
    curl_global_cleanup ();
    return 0;
}

/* ------------------------------------------------------------------
 * libnetfs callbacks — reading the tree
 * ------------------------------------------------------------------ */

/* netfs_attempt_lookup — resolve NAME inside DIR.
   Contract: on success *NP is locked and referenced; DIR is
   unlocked no matter what (unless *NP == DIR). */
error_t netfs_attempt_lookup (struct iouser *user, struct node *dir,
                              const char *name, struct node **np)
{
    struct netnode *dn = dir->nn;
    enum httpfs_kind kind;
    error_t err;

    (void) user;               /* no access control for now */
    *np = nullptr;

    if (dn->kind != HTTPFS_DIR)
    {
        pthread_mutex_unlock (&dir->lock);
        return ENOTDIR;
    }

    /* "." — the directory itself: return it as-is, still locked,
       with one extra reference. */
    if (strcmp (name, ".") == 0)
    {
        netfs_nref (dir);
        *np = dir;
        return 0;
    }

    /* ".." — the parent directory (the root case is normally
       intercepted by libnetfs itself; defensively, a ".." at the
       root returns the root). */
    if (strcmp (name, "..") == 0)
    {
        struct node *parent = dn->parent != nullptr ? dn->parent->node : dir;

        if (parent == dir)
          {
            /* ".." at the root is the root itself.  dir->lock is
               already held and must STAY held (the contract returns
               *np locked): re-locking it would self-deadlock on a
               default mutex. */
            netfs_nref (dir);
            *np = dir;
            return 0;
          }

        pthread_mutex_lock (&parent->lock);
        netfs_nref (parent);
        *np = parent;
        pthread_mutex_unlock (&dir->lock);
        return 0;
    }

    /* Is there already a live child under this name?  The node
       cache keeps it referenced, so taking one more reference
       right here is safe. */
    struct netnode *cn = find_child (dn, name);
    if (cn != nullptr)
    {
        struct node *hit = cn->node;

        netfs_nref (hit);
        pthread_mutex_unlock (&dir->lock);
        pthread_mutex_lock (&hit->lock);
        *np = hit;
        return 0;
    }

    /* Miss: create the child.  Reserved names become the views on
       this directory's resource; anything else extends the URL. */
    if (reserved_name (name, &kind))
        err = create_child (dir, name, kind, np);
    else
        err = create_child (dir, name, HTTPFS_DIR, np);

    pthread_mutex_unlock (&dir->lock);
    return err;
}

/* netfs_get_dirents — list the entries of a directory.
   The listing is identical at every level: the three views. */
error_t netfs_get_dirents (struct iouser *cred, struct node *dir, int entry,
                           int nentries, char **data,
                           mach_msg_type_number_t *datacnt, vm_size_t bufsize,
                           int *amt)
{
    static const char *const names[] =
        { HTTPFS_NAME_CONTENT, HTTPFS_NAME_HEADERS, HTTPFS_NAME_STATUS };
    constexpr int NUM_ENTRIES = 3;

    (void) cred;
    (void) bufsize;

    *amt = 0;
    *datacnt = 0;

    if (dir->nn->kind != HTTPFS_DIR)
    {
        pthread_mutex_unlock (&dir->lock);
        return ENOTDIR;
    }

    if (entry >= NUM_ENTRIES || nentries == 0)
    {
        pthread_mutex_unlock (&dir->lock);
        return 0;
    }

    int nb = NUM_ENTRIES - entry;
    if (nentries != -1 && nentries < nb)
        nb = nentries;

    /* Buffer in VM memory (see the DIRENTS_SIZE comment above).
       MAP_PRIVATE | MAP_ANONYMOUS: a private anonymous mapping,
       valid both under strict POSIX and on the Hurd. */
    char *start = mmap (nullptr, DIRENTS_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (start == MAP_FAILED)
    {
        pthread_mutex_unlock (&dir->lock);
        return ENOMEM;
    }

    char *p = start;
    for (int i = entry; i < entry + nb; i++)
    {
        size_t name_len = strlen (names[i]);
        size_t reclen = DIRENT_LEN (name_len);

        struct dirent hdr;
        memset (&hdr, 0, sizeof hdr);
        hdr.d_namlen = name_len;
        hdr.d_fileno = (ino_t) (fnv1a (dir->nn->url)
                                ^ ((uint64_t) (i + 1) << 56));
        hdr.d_reclen = reclen;
        hdr.d_type = DT_REG;

        memcpy (p, &hdr, DIRENT_NAME_OFFS);
        strcpy (p + DIRENT_NAME_OFFS, names[i]);
        p += reclen;
    }

    /* Return the unused part of the region to the VM, trimmed at
       the page boundary: the same discipline as ftpfs, required
       by the data_dealloc = 1 semantics of the RPC. */
    long page = sysconf (_SC_PAGESIZE);
    uintptr_t end = ((uintptr_t) p + (uintptr_t) page - 1)
                    & ~((uintptr_t) page - 1);
    if (end < (uintptr_t) (start + DIRENTS_SIZE))
        munmap ((void *) end,
                (size_t) ((uintptr_t) (start + DIRENTS_SIZE) - end));

    *data = start;
    *amt = nb;
    *datacnt = (mach_msg_type_number_t) (p - start);
    pthread_mutex_unlock (&dir->lock);
    return 0;
}

/* netfs_validate_stat — fill in np->nn_stat.
   Phase 2: stat'ing a view costs one HEAD (metadata only); the
   document itself is only downloaded when its size cannot be
   known otherwise. */
error_t netfs_validate_stat (struct node *np, struct iouser *cred)
{
    struct netnode *nn = np->nn;
    (void) cred;

    mode_t mode;
    long long size;

    if (nn->kind == HTTPFS_DIR)
    {
        mode = S_IFDIR | 0555;
        size = 4096;                  /* indicative value */
    }
    else
    {
        int err = ensure_metadata (nn->parent);
        if (err != 0)
            return err;

        mode = S_IFREG | 0444;

        switch (nn->kind)
        {
        case HTTPFS_CONTENT:
            size = resource_size (nn->parent);
            if (size < 0)
                size = 0;
            break;

        case HTTPFS_HEADERS:
            size = nn->parent->probe.headers != nullptr
                   ? (long long) strlen (nn->parent->probe.headers) : 0;
            break;

        case HTTPFS_STATUS:
            {
                char tmp[24];
                int n = snprintf (tmp, sizeof tmp, "%ld\n",
                                  nn->parent->probe.status);
                size = n > 0 ? n : 0;
            }
            break;

        default:
            size = 0;
            break;
        }
    }

    memset (&np->nn_stat, 0, sizeof np->nn_stat);
    np->nn_stat.st_ino = ino_of (nn);
    np->nn_stat.st_mode = mode;
    np->nn_stat.st_nlink = 1;
    np->nn_stat.st_uid = 0;
    np->nn_stat.st_gid = 0;
    np->nn_stat.st_size = (off_t) size;
    np->nn_stat.st_blksize = 4096;
    np->nn_stat.st_blocks = (size + 511) / 512;

    if (nn->kind != HTTPFS_DIR && nn->parent->probe.filetime > 0)
        np->nn_stat.st_mtim.tv_sec = nn->parent->probe.filetime;

    np->nn_translated = mode;
    return 0;
}

/* netfs_attempt_read — read up to *LEN bytes at OFFSET from NP. */
error_t netfs_attempt_read (struct iouser *cred, struct node *np,
                            loff_t offset, size_t *len, void *data)
{
    struct netnode *nn = np->nn;
    (void) cred;

    if (nn->kind == HTTPFS_DIR)
        return EISDIR;

    struct netnode *own = nn->parent;    /* the resource's owner */
    int err;

    if (nn->kind == HTTPFS_CONTENT)
    {
        err = ensure_metadata (own);
        if (err != 0)
            return err;

        pthread_mutex_lock (&own->res_lock);

        if (own->state == RES_PROBED)
            err = ensure_content (own);
        else if (own->state == RES_ERROR)
            err = own->content_err;

        if (err != 0)
        {
            pthread_mutex_unlock (&own->res_lock);
            return err;
        }

        if (own->state == RES_FULLBODY)
        {
            /* Serve from the whole-body cache (Phase 1 behavior). */
            const char *bytes = own->full.body != nullptr
                                ? own->full.body : "";
            uint64_t n_bytes = own->full.body_len;

            if (offset < 0 || (uint64_t) offset >= n_bytes)
            {
                pthread_mutex_unlock (&own->res_lock);
                *len = 0;
                return 0;
            }

            size_t n = *len;
            if (n > n_bytes - (uint64_t) offset)
                n = (size_t) (n_bytes - (uint64_t) offset);

            memcpy (data, bytes + offset, n);
            *len = n;
            pthread_mutex_unlock (&own->res_lock);
            return 0;
        }

        /* RES_BLOCKS: fetch only the windows this read touches. */
        if (offset < 0)
        {
            pthread_mutex_unlock (&own->res_lock);
            *len = 0;
            return 0;
        }

        size_t n = *len;
        err = read_blocks (own, (uint64_t) offset, &n, data);
        pthread_mutex_unlock (&own->res_lock);
        *len = n;
        return err;
    }

    /* The headers and status views come from the probe: no
       document download is ever needed. */
    err = ensure_metadata (own);
    if (err != 0)
        return err;

    const char *bytes;
    size_t n_bytes;
    char tmp[24];

    if (nn->kind == HTTPFS_HEADERS)
    {
        bytes = own->probe.headers != nullptr ? own->probe.headers : "";
        n_bytes = strlen (bytes);
    }
    else
    {
        int n = snprintf (tmp, sizeof tmp, "%ld\n", own->probe.status);
        bytes = tmp;
        n_bytes = (size_t) (n > 0 ? n : 0);
    }

    if (offset < 0 || (uint64_t) offset >= (uint64_t) n_bytes)
    {
        *len = 0;
        return 0;
    }

    size_t n = *len;
    if (n > n_bytes - (size_t) offset)
        n = n_bytes - (size_t) offset;

    memcpy (data, bytes + offset, n);
    *len = n;
    return 0;
}

/* ------------------------------------------------------------------
 * libnetfs callbacks — nodes and users
 * ------------------------------------------------------------------ */

/* netfs_node_norefs — the node NP just lost its last reference.
   Release its pinned parent, its resource, then itself. */
void netfs_node_norefs (struct node *np)
{
    struct netnode *nn = np->nn;

    if (nn != nullptr)
    {
        /* Leave the parent's child list first, while the parent is
           still pinned by our reference. */
        if (nn->parent != nullptr)
            unlink_child (nn);

        release_resource (nn);

        if (nn->parent != nullptr)
            netfs_nrele (nn->parent->node);

        pthread_mutex_destroy (&nn->res_lock);
        pthread_mutex_destroy (&nn->child_lock);
        free (nn->name);
        free (nn->url);
        free (nn);
    }

    free (np);
}

void netfs_try_dropping_softrefs (struct node *np)
{
    /* The node cache holds hard references only (the ftpfs
       pattern), so there are no soft references to drop. */
    (void) np;
}

error_t netfs_check_open_permissions (struct iouser *user, struct node *np,
                                      int flags, int newnode)
{
    /* Read-only filesystem, world-readable. */
    (void) user; (void) np; (void) flags; (void) newnode;
    return 0;
}

error_t netfs_report_access (struct iouser *cred, struct node *np, int *types)
{
    (void) cred; (void) np;
    *types = O_READ;
    return 0;
}

/* netfs_make_user — build a credential from the RPC's id arrays.
   libiohelp's current API takes an out-parameter and raw arrays
   (iohelp_create_complex_iouser), not idvecs built by the caller. */
struct iouser *netfs_make_user (uid_t *uids, int nuids, uid_t *gids, int ngids)
{
    struct iouser *user = nullptr;

    if (iohelp_create_complex_iouser (&user, uids, nuids, gids, ngids) != 0)
        return nullptr;
    return user;
}

/* ------------------------------------------------------------------
 * libnetfs callbacks — a read-only filesystem: every modification
 * attempt is refused.
 * ------------------------------------------------------------------ */

error_t netfs_attempt_write (struct iouser *cred, struct node *np,
                             loff_t offset, size_t *len, const void *data)
{ (void) cred; (void) np; (void) offset; (void) len; (void) data;
  return EROFS; }

error_t netfs_attempt_set_size (struct iouser *cred, struct node *np,
                                loff_t size)
{ (void) cred; (void) np; (void) size; return EROFS; }

error_t netfs_attempt_chown (struct iouser *cred, struct node *np,
                             uid_t uid, gid_t gid)
{ (void) cred; (void) np; (void) uid; (void) gid; return EROFS; }

error_t netfs_attempt_chauthor (struct iouser *cred, struct node *np,
                                uid_t author)
{ (void) cred; (void) np; (void) author; return EROFS; }

error_t netfs_attempt_chmod (struct iouser *cred, struct node *np, mode_t mode)
{ (void) cred; (void) np; (void) mode; return EROFS; }

error_t netfs_attempt_chflags (struct iouser *cred, struct node *np, int flags)
{ (void) cred; (void) np; (void) flags; return EROFS; }

error_t netfs_attempt_utimes (struct iouser *cred, struct node *np,
                              struct timespec *atime, struct timespec *mtime)
{ (void) cred; (void) np; (void) atime; (void) mtime; return EROFS; }

error_t netfs_attempt_unlink (struct iouser *user, struct node *dir,
                              const char *name)
{ (void) user; (void) dir; (void) name; return EROFS; }

error_t netfs_attempt_rmdir (struct iouser *user, struct node *dir,
                             const char *name)
{ (void) user; (void) dir; (void) name; return EROFS; }

error_t netfs_attempt_mkdir (struct iouser *user, struct node *dir,
                             const char *name, mode_t mode)
{ (void) user; (void) dir; (void) name; (void) mode; return EROFS; }

error_t netfs_attempt_rename (struct iouser *user, struct node *fromdir,
                              const char *fromname, struct node *todir,
                              const char *toname, int excl)
{ (void) user; (void) fromdir; (void) fromname; (void) todir;
  (void) toname; (void) excl; return EROFS; }

error_t netfs_attempt_link (struct iouser *user, struct node *dir,
                            struct node *file, const char *name, int excl)
{ (void) user; (void) dir; (void) file; (void) name; (void) excl;
  return EROFS; }

error_t netfs_attempt_mkfile (struct iouser *user, struct node *dir,
                              mode_t mode, struct node **np)
{ *np = nullptr; pthread_mutex_unlock (&dir->lock); (void) user; (void) mode;
  return EROFS; }

error_t netfs_attempt_create_file (struct iouser *user, struct node *dir,
                                   const char *name, mode_t mode,
                                   struct node **np)
{ *np = nullptr; pthread_mutex_unlock (&dir->lock);
  (void) user; (void) name; (void) mode; return EROFS; }

error_t netfs_attempt_mksymlink (struct iouser *cred, struct node *np,
                                 const char *name)
{ (void) cred; (void) np; (void) name; return EROFS; }

error_t netfs_attempt_mkdev (struct iouser *cred, struct node *np,
                             mode_t type, dev_t indexes)
{ (void) cred; (void) np; (void) type; (void) indexes; return EROFS; }

error_t netfs_attempt_readlink (struct iouser *user, struct node *np, char *buf)
{ (void) user; (void) np; (void) buf; return EINVAL; }

error_t netfs_attempt_statfs (struct iouser *cred, struct node *np,
                              fsys_statfsbuf_t *st)
{ (void) cred; (void) np; memset (st, 0, sizeof *st); return 0; }

error_t netfs_attempt_sync (struct iouser *cred, struct node *np, int wait)
{ (void) cred; (void) np; (void) wait; return 0; }

error_t netfs_attempt_syncfs (struct iouser *cred, int wait)
{ (void) cred; (void) wait; return 0; }
