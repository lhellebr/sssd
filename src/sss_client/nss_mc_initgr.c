/*
 * System Security Services Daemon. NSS client interface
 *
 * Authors:
 *     Lukas Slebodnik <lslebodn@redhat.com>
 *
 * Copyright (C) 2015 Red Hat
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* INITGROUPs database NSS interface using mmap cache */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <sys/mman.h>
#include <time.h>
#include "nss_mc.h"
#include "util/util_safealign.h"

struct sss_cli_mc_ctx initgr_mc_ctx = { UNINITIALIZED, -1, 0, NULL, 0, NULL, 0,
                                        NULL, 0, 0 };

static errno_t sss_nss_mc_parse_result(struct sss_mc_rec *rec,
                                       long int *start, long int *size,
                                       gid_t **groups, long int limit)
{
    struct sss_mc_initgr_data *data;
    time_t expire;
    long int i;
    uint32_t gid_count;
    long int max_ret;

    /* additional checks before filling result*/
    expire = rec->expire;
    if (expire < time(NULL)) {
        /* entry is now invalid */
        return EINVAL;
    }

    data = (struct sss_mc_initgr_data *)rec->data;
    gid_count = data->members;
    max_ret = gid_count;

    /* check we have enough space in the buffer */
    if ((*size - *start) < gid_count) {
        long int newsize;
        gid_t *newgroups;

        newsize = *size + gid_count;
        if ((limit > 0) && (newsize > limit)) {
            newsize = limit;
            max_ret = newsize - *start;
        }

        newgroups = (gid_t *)realloc((*groups), newsize * sizeof(**groups));
        if (!newgroups) {
            return ENOMEM;
        }
        *groups = newgroups;
        *size = newsize;
    }

    for (i = 0; i < max_ret; i++) {
        SAFEALIGN_COPY_UINT32(&(*groups)[*start], data->gids + i, NULL);
        *start += 1;
    }

    return 0;
}

errno_t sss_nss_mc_initgroups_dyn(const char *name, size_t name_len,
                                  gid_t group, long int *start, long int *size,
                                  gid_t **groups, long int limit)
{
    struct sss_mc_rec *rec = NULL;
    struct sss_mc_initgr_data *data;
    char *rec_name;
    uint32_t hash;
    uint32_t slot;
    int ret;
    uint8_t *max_addr;

    ret = sss_nss_mc_get_ctx("initgroups", &initgr_mc_ctx);
    if (ret) {
        return ret;
    }

    /* Get max address of data table. */
    max_addr = initgr_mc_ctx.data_table + initgr_mc_ctx.dt_size;

    /* hashes are calculated including the NULL terminator */
    hash = sss_nss_mc_hash(&initgr_mc_ctx, name, name_len + 1);
    slot = initgr_mc_ctx.hash_table[hash];

    /* If slot is not within the bounds of mmaped region and
     * it's value is not MC_INVALID_VAL, then the cache is
     * probbably corrupted. */
    while (MC_SLOT_WITHIN_BOUNDS(slot, initgr_mc_ctx.dt_size)) {
        /* free record from previous iteration */
        free(rec);
        rec = NULL;

        ret = sss_nss_mc_get_record(&initgr_mc_ctx, slot, &rec);
        if (ret) {
            goto done;
        }

        /* check record matches what we are searching for */
        if (hash != rec->hash1) {
            /* if name hash does not match we can skip this immediately */
            slot = sss_nss_mc_next_slot_with_hash(rec, hash);
            continue;
        }

        data = (struct sss_mc_initgr_data *)rec->data;
        /* Integrity check
         * - array with gids must be within data_table
         * - string must be within data_table */
        if ((uint8_t *)data->gids > max_addr
                || (uint8_t *)data + data->name + name_len > max_addr) {
            ret = ENOENT;
            goto done;
        }

        rec_name = (char *)data + data->name;
        if (strcmp(name, rec_name) == 0) {
            break;
        }

        slot = sss_nss_mc_next_slot_with_hash(rec, hash);
    }

    if (!MC_SLOT_WITHIN_BOUNDS(slot, initgr_mc_ctx.dt_size)) {
        ret = ENOENT;
        goto done;
    }

    ret = sss_nss_mc_parse_result(rec, start, size, groups, limit);

done:
    free(rec);
    __sync_sub_and_fetch(&initgr_mc_ctx.active_threads, 1);
    return ret;
}