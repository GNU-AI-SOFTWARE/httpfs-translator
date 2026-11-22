/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/* util.c -- inode derivation and reserved-name helpers of httpfs. */

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
 * Utilities
 * ------------------------------------------------------------------ */

/* fnv1a — 64-bit FNV-1a hash of a string.  Used to derive inode
   numbers that are stable and reproducible from the URLs. */
uint64_t fnv1a (const char *s)
{
    uint64_t h = 14695981039346656037ULL;
    for (const unsigned char *p = (const unsigned char *) s; *p != '\0'; p++)
    {
        h ^= (uint64_t) *p;
        h *= 1099511628211ULL;
    }
    return h;
}

/* ino_of — inode number of a node, derived from its URL and its
   kind (the content/headers/status views of one directory thus get
   distinct inode numbers). */
ino_t ino_of (const struct netnode *nn)
{
    uint64_t h = fnv1a (nn->kind == HTTPFS_DIR ? nn->url : nn->parent->url);
    return (ino_t) (h ^ ((uint64_t) nn->kind << 56));
}

/* reserved_name — find the kind of a reserved name.
   Returns true when NAME is one of the virtual files. */
bool reserved_name (const char *name, enum httpfs_kind *kind)
{
    if (strcmp (name, HTTPFS_NAME_CONTENT) == 0)
        *kind = HTTPFS_CONTENT;
    else if (strcmp (name, HTTPFS_NAME_HEADERS) == 0)
        *kind = HTTPFS_HEADERS;
    else if (strcmp (name, HTTPFS_NAME_STATUS) == 0)
        *kind = HTTPFS_STATUS;
    else
        return false;
    return true;
}
