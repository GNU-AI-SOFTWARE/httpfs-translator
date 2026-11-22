/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * netfs_internal.h -- shared internals of the httpfs netfs modules
 * (netfs.c = libnetfs callbacks, util.c, blocks.c = ranged block cache,
 * cache.c = node tree + MRU cache).  Not part of the public httpfs.h.
 */
#ifndef HTTPFS_NETFS_INTERNAL_H
#define HTTPFS_NETFS_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <pthread.h>
#include <hurd/netfs.h>

#include "httpfs.h"

/* Directory entry construction (cf. ftpfs conventions). */
#define DIRENT_ALIGN     4
#define DIRENT_NAME_OFFS offsetof (struct dirent, d_name)
#define DIRENT_LEN(namelen)                                                \
    ((DIRENT_NAME_OFFS + (namelen) + 1 + (DIRENT_ALIGN - 1))               \
     & ~((size_t) (DIRENT_ALIGN - 1)))

/* Size of the memory region handed back to libnetfs: must be mmap'd
 * (VM memory), never malloc (data_dealloc = 1). */
#define DIRENTS_SIZE     (8 * 1024)

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

/* util.c */
uint64_t fnv1a (const char *s);
ino_t ino_of (const struct netnode *nn);
bool reserved_name (const char *name, enum httpfs_kind *kind);

/* blocks.c */
long long resource_size (struct netnode *nn);
int read_blocks (struct netnode *nn, uint64_t offset, size_t *len, char *buf);
void release_resource (struct netnode *nn);

/* cache.c */
void cache_node (struct node *np);
error_t create_child (struct node *dir, const char *name,
                      enum httpfs_kind kind, struct node **np);
struct netnode *find_child (struct netnode *dn, const char *name);
void unlink_child (struct netnode *nn);

#endif /* HTTPFS_NETFS_INTERNAL_H */
