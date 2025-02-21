/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2013 by Delphix. All rights reserved.
 * Copyright (c) 2014, Joyent, Inc. All rights reserved.
 * Copyright (c) 2014 RackTop Systems.
 */

#include <sys/dmu_objset.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_dir.h>
#include <sys/dsl_prop.h>
#include <sys/dsl_synctask.h>
#include <sys/dmu_traverse.h>
#include <sys/dmu_impl.h>
#include <sys/dmu_tx.h>
#include <sys/arc.h>
#include <sys/zio.h>
#include <sys/zap.h>
#include <sys/zfeature.h>
#include <sys/unique.h>
#include <sys/zfs_context.h>
#include <sys/zfs_ioctl.h>
#include <sys/spa.h>
#include <sys/zfs_znode.h>
#include <sys/zfs_onexit.h>
#include <sys/zvol.h>
#include <sys/dsl_scan.h>
#include <sys/dsl_deadlist.h>
#include <sys/dsl_destroy.h>
#include <sys/dsl_userhold.h>
#include <sys/dsl_bookmark.h>

#define	SWITCH64(x, y) \
	{ \
		uint64_t __tmp = (x); \
		(x) = (y); \
		(y) = __tmp; \
	}

#define	DS_REF_MAX	(1ULL << 62)

#define	DSL_DEADLIST_BLOCKSIZE	SPA_MAXBLOCKSIZE

/*
 * Figure out how much of this delta should be propogated to the dsl_dir
 * layer.  If there's a refreservation, that space has already been
 * partially accounted for in our ancestors.
 */
static int64_t
parent_delta(dsl_dataset_t *ds, int64_t delta)
{
	uint64_t old_bytes, new_bytes;

	if (ds->ds_reserved == 0)
		return (delta);

	old_bytes = MAX(ds->ds_phys->ds_unique_bytes, ds->ds_reserved);
	new_bytes = MAX(ds->ds_phys->ds_unique_bytes + delta, ds->ds_reserved);

	ASSERT3U(ABS((int64_t)(new_bytes - old_bytes)), <=, ABS(delta));
	return (new_bytes - old_bytes);
}

void
dsl_dataset_block_born(dsl_dataset_t *ds, const blkptr_t *bp, dmu_tx_t *tx)
{
	int used = bp_get_dsize_sync(tx->tx_pool->dp_spa, bp);
	int compressed = BP_GET_PSIZE(bp);
	int uncompressed = BP_GET_UCSIZE(bp);
	int64_t delta;

	dprintf_bp(bp, "ds=%p", ds);

	ASSERT(dmu_tx_is_syncing(tx));
	/* It could have been compressed away to nothing */
	if (BP_IS_HOLE(bp))
		return;
	ASSERT(BP_GET_TYPE(bp) != DMU_OT_NONE);
	ASSERT(DMU_OT_IS_VALID(BP_GET_TYPE(bp)));
	if (ds == NULL) {
		dsl_pool_mos_diduse_space(tx->tx_pool,
		    used, compressed, uncompressed);
		return;
	}

	dmu_buf_will_dirty(ds->ds_dbuf, tx);
	mutex_enter(&ds->ds_lock);
	delta = parent_delta(ds, used);
	ds->ds_phys->ds_referenced_bytes += used;
	ds->ds_phys->ds_compressed_bytes += compressed;
	ds->ds_phys->ds_uncompressed_bytes += uncompressed;
	ds->ds_phys->ds_unique_bytes += used;
	mutex_exit(&ds->ds_lock);
	dsl_dir_diduse_space(ds->ds_dir, DD_USED_HEAD, delta,
	    compressed, uncompressed, tx);
	dsl_dir_transfer_space(ds->ds_dir, used - delta,
	    DD_USED_REFRSRV, DD_USED_HEAD, tx);
}

int
dsl_dataset_block_kill(dsl_dataset_t *ds, const blkptr_t *bp, dmu_tx_t *tx,
    boolean_t async)
{
	int used = bp_get_dsize_sync(tx->tx_pool->dp_spa, bp);
	int compressed = BP_GET_PSIZE(bp);
	int uncompressed = BP_GET_UCSIZE(bp);

	if (BP_IS_HOLE(bp))
		return (0);

	ASSERT(dmu_tx_is_syncing(tx));
	ASSERT(bp->blk_birth <= tx->tx_txg);

	if (ds == NULL) {
		dsl_free(tx->tx_pool, tx->tx_txg, bp);
		dsl_pool_mos_diduse_space(tx->tx_pool,
		    -used, -compressed, -uncompressed);
		return (used);
	}
	ASSERT3P(tx->tx_pool, ==, ds->ds_dir->dd_pool);

	ASSERT(!dsl_dataset_is_snapshot(ds));
	dmu_buf_will_dirty(ds->ds_dbuf, tx);

	if (bp->blk_birth > ds->ds_phys->ds_prev_snap_txg) {
		int64_t delta;

		dprintf_bp(bp, "freeing ds=%llu", ds->ds_object);
		dsl_free(tx->tx_pool, tx->tx_txg, bp);

		mutex_enter(&ds->ds_lock);
		ASSERT(ds->ds_phys->ds_unique_bytes >= used ||
		    !DS_UNIQUE_IS_ACCURATE(ds));
		delta = parent_delta(ds, -used);
		ds->ds_phys->ds_unique_bytes -= used;
		mutex_exit(&ds->ds_lock);
		dsl_dir_diduse_space(ds->ds_dir, DD_USED_HEAD,
		    delta, -compressed, -uncompressed, tx);
		dsl_dir_transfer_space(ds->ds_dir, -used - delta,
		    DD_USED_REFRSRV, DD_USED_HEAD, tx);
	} else {
		dprintf_bp(bp, "putting on dead list: %s", "");
		if (async) {
			/*
			 * We are here as part of zio's write done callback,
			 * which means we're a zio interrupt thread.  We can't
			 * call dsl_deadlist_insert() now because it may block
			 * waiting for I/O.  Instead, put bp on the deferred
			 * queue and let dsl_pool_sync() finish the job.
			 */
			bplist_append(&ds->ds_pending_deadlist, bp);
		} else {
			dsl_deadlist_insert(&ds->ds_deadlist, bp, tx);
		}
		ASSERT3U(ds->ds_prev->ds_object, ==,
		    ds->ds_phys->ds_prev_snap_obj);
		ASSERT(ds->ds_prev->ds_phys->ds_num_children > 0);
		/* if (bp->blk_birth > prev prev snap txg) prev unique += bs */
		if (ds->ds_prev->ds_phys->ds_next_snap_obj ==
		    ds->ds_object && bp->blk_birth >
		    ds->ds_prev->ds_phys->ds_prev_snap_txg) {
			dmu_buf_will_dirty(ds->ds_prev->ds_dbuf, tx);
			mutex_enter(&ds->ds_prev->ds_lock);
			ds->ds_prev->ds_phys->ds_unique_bytes += used;
			mutex_exit(&ds->ds_prev->ds_lock);
		}
		if (bp->blk_birth > ds->ds_dir->dd_origin_txg) {
			dsl_dir_transfer_space(ds->ds_dir, used,
			    DD_USED_HEAD, DD_USED_SNAP, tx);
		}
	}
	mutex_enter(&ds->ds_lock);
	ASSERT3U(ds->ds_phys->ds_referenced_bytes, >=, used);
	ds->ds_phys->ds_referenced_bytes -= used;
	ASSERT3U(ds->ds_phys->ds_compressed_bytes, >=, compressed);
	ds->ds_phys->ds_compressed_bytes -= compressed;
	ASSERT3U(ds->ds_phys->ds_uncompressed_bytes, >=, uncompressed);
	ds->ds_phys->ds_uncompressed_bytes -= uncompressed;
	mutex_exit(&ds->ds_lock);

	return (used);
}

uint64_t
dsl_dataset_prev_snap_txg(dsl_dataset_t *ds)
{
	uint64_t trysnap = 0;

	if (ds == NULL)
		return (0);
	/*
	 * The snapshot creation could fail, but that would cause an
	 * incorrect FALSE return, which would only result in an
	 * overestimation of the amount of space that an operation would
	 * consume, which is OK.
	 *
	 * There's also a small window where we could miss a pending
	 * snapshot, because we could set the sync task in the quiescing
	 * phase.  So this should only be used as a guess.
	 */
	if (ds->ds_trysnap_txg >
	    spa_last_synced_txg(ds->ds_dir->dd_pool->dp_spa))
		trysnap = ds->ds_trysnap_txg;
	return (MAX(ds->ds_phys->ds_prev_snap_txg, trysnap));
}

boolean_t
dsl_dataset_block_freeable(dsl_dataset_t *ds, const blkptr_t *bp,
    uint64_t blk_birth)
{
	if (blk_birth <= dsl_dataset_prev_snap_txg(ds) ||
	    (bp != NULL && BP_IS_HOLE(bp)))
		return (B_FALSE);

	ddt_prefetch(dsl_dataset_get_spa(ds), bp);

	return (B_TRUE);
}

/* ARGSUSED */
static void
dsl_dataset_evict(dmu_buf_t *db, void *dsv)
{
	dsl_dataset_t *ds = dsv;

	ASSERT(ds->ds_owner == NULL);

	unique_remove(ds->ds_fsid_guid);

	if (ds->ds_objset != NULL)
		dmu_objset_evict(ds->ds_objset);

	if (ds->ds_prev) {
		dsl_dataset_rele(ds->ds_prev, ds);
		ds->ds_prev = NULL;
	}

	bplist_destroy(&ds->ds_pending_deadlist);
	if (ds->ds_phys->ds_deadlist_obj != 0)
		dsl_deadlist_close(&ds->ds_deadlist);
	if (ds->ds_dir)
		dsl_dir_rele(ds->ds_dir, ds);

	ASSERT(!list_link_active(&ds->ds_synced_link));

	mutex_destroy(&ds->ds_lock);
	mutex_destroy(&ds->ds_opening_lock);
	refcount_destroy(&ds->ds_longholds);

	kmem_free(ds, sizeof (dsl_dataset_t));
}

int
dsl_dataset_get_snapname(dsl_dataset_t *ds)
{
	dsl_dataset_phys_t *headphys;
	int err;
	dmu_buf_t *headdbuf;
	dsl_pool_t *dp = ds->ds_dir->dd_pool;
	objset_t *mos = dp->dp_meta_objset;

	if (ds->ds_snapname[0])
		return (0);
	if (ds->ds_phys->ds_next_snap_obj == 0)
		return (0);

	err = dmu_bonus_hold(mos, ds->ds_dir->dd_phys->dd_head_dataset_obj,
	    FTAG, &headdbuf);
	if (err != 0)
		return (err);
	headphys = headdbuf->db_data;
	err = zap_value_search(dp->dp_meta_objset,
	    headphys->ds_snapnames_zapobj, ds->ds_object, 0, ds->ds_snapname);
	dmu_buf_rele(headdbuf, FTAG);
	return (err);
}

int
dsl_dataset_snap_lookup(dsl_dataset_t *ds, const char *name, uint64_t *value)
{
	objset_t *mos = ds->ds_dir->dd_pool->dp_meta_objset;
	uint64_t snapobj = ds->ds_phys->ds_snapnames_zapobj;
	matchtype_t mt;
	int err;

	if (ds->ds_phys->ds_flags & DS_FLAG_CI_DATASET)
		mt = MT_FIRST;
	else
		mt = MT_EXACT;

	err = zap_lookup_norm(mos, snapobj, name, 8, 1,
	    value, mt, NULL, 0, NULL);
	if (err == ENOTSUP && mt == MT_FIRST)
		err = zap_lookup(mos, snapobj, name, 8, 1, value);
	return (err);
}

int
dsl_dataset_snap_remove(dsl_dataset_t *ds, const char *name, dmu_tx_t *tx,
    boolean_t adj_cnt)
{
	objset_t *mos = ds->ds_dir->dd_pool->dp_meta_objset;
	uint64_t snapobj = ds->ds_phys->ds_snapnames_zapobj;
	matchtype_t mt;
	int err;

	dsl_dir_snap_cmtime_update(ds->ds_dir);

	if (ds->ds_phys->ds_flags & DS_FLAG_CI_DATASET)
		mt = MT_FIRST;
	else
		mt = MT_EXACT;

	err = zap_remove_norm(mos, snapobj, name, mt, tx);
	if (err == ENOTSUP && mt == MT_FIRST)
		err = zap_remove(mos, snapobj, name, tx);

	if (err == 0 && adj_cnt)
		dsl_fs_ss_count_adjust(ds->ds_dir, -1,
		    DD_FIELD_SNAPSHOT_COUNT, tx);

	return (err);
}

int
dsl_dataset_hold_obj(dsl_pool_t *dp, uint64_t dsobj, void *tag,
    dsl_dataset_t **dsp)
{
	objset_t *mos = dp->dp_meta_objset;
	dmu_buf_t *dbuf;
	dsl_dataset_t *ds;
	int err;
	dmu_object_info_t doi;

	ASSERT(dsl_pool_config_held(dp));

	err = dmu_bonus_hold(mos, dsobj, tag, &dbuf);
	if (err != 0)
		return (err);

	/* Make sure dsobj has the correct object type. */
	dmu_object_info_from_db(dbuf, &doi);
	if (doi.doi_bonus_type != DMU_OT_DSL_DATASET) {
		dmu_buf_rele(dbuf, tag);
		return (SET_ERROR(EINVAL));
	}

	ds = dmu_buf_get_user(dbuf);
	if (ds == NULL) {
		dsl_dataset_t *winner = NULL;

		ds = kmem_zalloc(sizeof (dsl_dataset_t), KM_SLEEP);
		ds->ds_dbuf = dbuf;
		ds->ds_object = dsobj;
		ds->ds_phys = dbuf->db_data;

		mutex_init(&ds->ds_lock, NULL, MUTEX_DEFAULT, NULL);
		mutex_init(&ds->ds_opening_lock, NULL, MUTEX_DEFAULT, NULL);
		mutex_init(&ds->ds_sendstream_lock, NULL, MUTEX_DEFAULT, NULL);
		refcount_create(&ds->ds_longholds);

		bplist_create(&ds->ds_pending_deadlist);
		dsl_deadlist_open(&ds->ds_deadlist,
		    mos, ds->ds_phys->ds_deadlist_obj);

		list_create(&ds->ds_sendstreams, sizeof (dmu_sendarg_t),
		    offsetof(dmu_sendarg_t, dsa_link));

		if (err == 0) {
			err = dsl_dir_hold_obj(dp,
			    ds->ds_phys->ds_dir_obj, NULL, ds, &ds->ds_dir);
		}
		if (err != 0) {
			mutex_destroy(&ds->ds_lock);
			mutex_destroy(&ds->ds_opening_lock);
			refcount_destroy(&ds->ds_longholds);
			bplist_destroy(&ds->ds_pending_deadlist);
			dsl_deadlist_close(&ds->ds_deadlist);
			kmem_free(ds, sizeof (dsl_dataset_t));
			dmu_buf_rele(dbuf, tag);
			return (err);
		}

		if (!dsl_dataset_is_snapshot(ds)) {
			ds->ds_snapname[0] = '\0';
			if (ds->ds_phys->ds_prev_snap_obj != 0) {
				err = dsl_dataset_hold_obj(dp,
				    ds->ds_phys->ds_prev_snap_obj,
				    ds, &ds->ds_prev);
			}
			if (doi.doi_type == DMU_OTN_ZAP_METADATA) {
				int zaperr = zap_lookup(mos, ds->ds_object,
				    DS_FIELD_BOOKMARK_NAMES,
				    sizeof (ds->ds_bookmarks), 1,
				    &ds->ds_bookmarks);
				if (zaperr != ENOENT)
					VERIFY0(zaperr);
			}
		} else {
			if (zfs_flags & ZFS_DEBUG_SNAPNAMES)
				err = dsl_dataset_get_snapname(ds);
			if (err == 0 && ds->ds_phys->ds_userrefs_obj != 0) {
				err = zap_count(
				    ds->ds_dir->dd_pool->dp_meta_objset,
				    ds->ds_phys->ds_userrefs_obj,
				    &ds->ds_userrefs);
			}
		}

		if (err == 0 && !dsl_dataset_is_snapshot(ds)) {
			err = dsl_prop_get_int_ds(ds,
			    zfs_prop_to_name(ZFS_PROP_REFRESERVATION),
			    &ds->ds_reserved);
			if (err == 0) {
				err = dsl_prop_get_int_ds(ds,
				    zfs_prop_to_name(ZFS_PROP_REFQUOTA),
				    &ds->ds_quota);
			}
		} else {
			ds->ds_reserved = ds->ds_quota = 0;
		}

		if (err != 0 || (winner = dmu_buf_set_user_ie(dbuf, ds,
		    &ds->ds_phys, dsl_dataset_evict)) != NULL) {
			bplist_destroy(&ds->ds_pending_deadlist);
			dsl_deadlist_close(&ds->ds_deadlist);
			if (ds->ds_prev)
				dsl_dataset_rele(ds->ds_prev, ds);
			dsl_dir_rele(ds->ds_dir, ds);
			mutex_destroy(&ds->ds_lock);
			mutex_destroy(&ds->ds_opening_lock);
			refcount_destroy(&ds->ds_longholds);
			kmem_free(ds, sizeof (dsl_dataset_t));
			if (err != 0) {
				dmu_buf_rele(dbuf, tag);
				return (err);
			}
			ds = winner;
		} else {
			ds->ds_fsid_guid =
			    unique_insert(ds->ds_phys->ds_fsid_guid);
		}
	}
	ASSERT3P(ds->ds_dbuf, ==, dbuf);
	ASSERT3P(ds->ds_phys, ==, dbuf->db_data);
	ASSERT(ds->ds_phys->ds_prev_snap_obj != 0 ||
	    spa_version(dp->dp_spa) < SPA_VERSION_ORIGIN ||
	    dp->dp_origin_snap == NULL || ds == dp->dp_origin_snap);
	*dsp = ds;
	return (0);
}

int
dsl_dataset_hold(dsl_pool_t *dp, const char *name,
    void *tag, dsl_dataset_t **dsp)
{
	dsl_dir_t *dd;
	const char *snapname;
	uint64_t obj;
	int err = 0;

	err = dsl_dir_hold(dp, name, FTAG, &dd, &snapname);
	if (err != 0)
		return (err);

	ASSERT(dsl_pool_config_held(dp));
	obj = dd->dd_phys->dd_head_dataset_obj;
	if (obj != 0)
		err = dsl_dataset_hold_obj(dp, obj, tag, dsp);
	else
		err = SET_ERROR(ENOENT);

	/* we may be looking for a snapshot */
	if (err == 0 && snapname != NULL) {
		dsl_dataset_t *ds;

		if (*snapname++ != '@') {
			dsl_dataset_rele(*dsp, tag);
			dsl_dir_rele(dd, FTAG);
			return (SET_ERROR(ENOENT));
		}

		dprintf("looking for snapshot '%s'\n", snapname);
		err = dsl_dataset_snap_lookup(*dsp, snapname, &obj);
		if (err == 0)
			err = dsl_dataset_hold_obj(dp, obj, tag, &ds);
		dsl_dataset_rele(*dsp, tag);

		if (err == 0) {
			mutex_enter(&ds->ds_lock);
			if (ds->ds_snapname[0] == 0)
				(void) strlcpy(ds->ds_snapname, snapname,
				    sizeof (ds->ds_snapname));
			mutex_exit(&ds->ds_lock);
			*dsp = ds;
		}
	}

	dsl_dir_rele(dd, FTAG);
	return (err);
}

int
dsl_dataset_own_obj(dsl_pool_t *dp, uint64_t dsobj,
    void *tag, dsl_dataset_t **dsp)
{
	int err = dsl_dataset_hold_obj(dp, dsobj, tag, dsp);
	if (err != 0)
		return (err);
	if (!dsl_dataset_tryown(*dsp, tag)) {
		dsl_dataset_rele(*dsp, tag);
		*dsp = NULL;
		return (SET_ERROR(EBUSY));
	}
	return (0);
}

int
dsl_dataset_own(dsl_pool_t *dp, const char *name,
    void *tag, dsl_dataset_t **dsp)
{
	int err = dsl_dataset_hold(dp, name, tag, dsp);
	if (err != 0)
		return (err);
	if (!dsl_dataset_tryown(*dsp, tag)) {
		dsl_dataset_rele(*dsp, tag);
		return (SET_ERROR(EBUSY));
	}
	return (0);
}

/*
 * See the comment above dsl_pool_hold() for details.  In summary, a long
 * hold is used to prevent destruction of a dataset while the pool hold
 * is dropped, allowing other concurrent operations (e.g. spa_sync()).
 *
 * The dataset and pool must be held when this function is called.  After it
 * is called, the pool hold may be released while the dataset is still held
 * and accessed.
 */
void
dsl_dataset_long_hold(dsl_dataset_t *ds, void *tag)
{
	ASSERT(dsl_pool_config_held(ds->ds_dir->dd_pool));
	(void) refcount_add(&ds->ds_longholds, tag);
}

void
dsl_dataset_long_rele(dsl_dataset_t *ds, void *tag)
{
	(void) refcount_remove(&ds->ds_longholds, tag);
}

/* Return B_TRUE if there are any long holds on this dataset. */
boolean_t
dsl_dataset_long_held(dsl_dataset_t *ds)
{
	return (!refcount_is_zero(&ds->ds_longholds));
}

void
dsl_dataset_name(dsl_dataset_t *ds, char *name)
{
	if (ds == NULL) {
		(void) strcpy(name, "mos");
	} else {
		dsl_dir_name(ds->ds_dir, name);
		VERIFY0(dsl_dataset_get_snapname(ds));
		if (ds->ds_snapname[0]) {
			(void) strcat(name, "@");
			/*
			 * We use a "recursive" mutex so that we
			 * can call dprintf_ds() with ds_lock held.
			 */
			if (!MUTEX_HELD(&ds->ds_lock)) {
				mutex_enter(&ds->ds_lock);
				(void) strcat(name, ds->ds_snapname);
				mutex_exit(&ds->ds_lock);
			} else {
				(void) strcat(name, ds->ds_snapname);
			}
		}
	}
}

void
dsl_dataset_rele(dsl_dataset_t *ds, void *tag)
{
	dmu_buf_rele(ds->ds_dbuf, tag);
}

void
dsl_dataset_disown(dsl_dataset_t *ds, void *tag)
{
	ASSERT(ds->ds_owner == tag && ds->ds_dbuf != NULL);

	mutex_enter(&ds->ds_lock);
	ds->ds_owner = NULL;
	mutex_exit(&ds->ds_lock);
	dsl_dataset_long_rele(ds, tag);
	if (ds->ds_dbuf != NULL)
		dsl_dataset_rele(ds, tag);
	else
		dsl_dataset_evict(NULL, ds);
}

boolean_t
dsl_dataset_tryown(dsl_dataset_t *ds, void *tag)
{
	boolean_t gotit = FALSE;

	mutex_enter(&ds->ds_lock);
	if (ds->ds_owner == NULL && !DS_IS_INCONSISTENT(ds)) {
		ds->ds_owner = tag;
		dsl_dataset_long_hold(ds, tag);
		gotit = TRUE;
	}
	mutex_exit(&ds->ds_lock);
	return (gotit);
}

uint64_t
dsl_dataset_create_sync_dd(dsl_dir_t *dd, dsl_dataset_t *origin,
    uint64_t flags, dmu_tx_t *tx)
{
	dsl_pool_t *dp = dd->dd_pool;
	dmu_buf_t *dbuf;
	dsl_dataset_phys_t *dsphys;
	uint64_t dsobj;
	objset_t *mos = dp->dp_meta_objset;

	if (origin == NULL)
		origin = dp->dp_origin_snap;

	ASSERT(origin == NULL || origin->ds_dir->dd_pool == dp);
	ASSERT(origin == NULL || origin->ds_phys->ds_num_children > 0);
	ASSERT(dmu_tx_is_syncing(tx));
	ASSERT(dd->dd_phys->dd_head_dataset_obj == 0);

	dsobj = dmu_object_alloc(mos, DMU_OT_DSL_DATASET, 0,
	    DMU_OT_DSL_DATASET, sizeof (dsl_dataset_phys_t), tx);
	VERIFY0(dmu_bonus_hold(mos, dsobj, FTAG, &dbuf));
	dmu_buf_will_dirty(dbuf, tx);
	dsphys = dbuf->db_data;
	bzero(dsphys, sizeof (dsl_dataset_phys_t));
	dsphys->ds_dir_obj = dd->dd_object;
	dsphys->ds_flags = flags;
	dsphys->ds_fsid_guid = unique_create();
	(void) random_get_pseudo_bytes((void*)&dsphys->ds_guid,
	    sizeof (dsphys->ds_guid));
	dsphys->ds_snapnames_zapobj =
	    zap_create_norm(mos, U8_TEXTPREP_TOUPPER, DMU_OT_DSL_DS_SNAP_MAP,
	    DMU_OT_NONE, 0, tx);
	dsphys->ds_creation_time = gethrestime_sec();
	dsphys->ds_creation_txg = tx->tx_txg == TXG_INITIAL ? 1 : tx->tx_txg;

	if (origin == NULL) {
		dsphys->ds_deadlist_obj = dsl_deadlist_alloc(mos, tx);
	} else {
		dsl_dataset_t *ohds; /* head of the origin snapshot */

		dsphys->ds_prev_snap_obj = origin->ds_object;
		dsphys->ds_prev_snap_txg =
		    origin->ds_phys->ds_creation_txg;
		dsphys->ds_referenced_bytes =
		    origin->ds_phys->ds_referenced_bytes;
		dsphys->ds_compressed_bytes =
		    origin->ds_phys->ds_compressed_bytes;
		dsphys->ds_uncompressed_bytes =
		    origin->ds_phys->ds_uncompressed_bytes;
		dsphys->ds_bp = origin->ds_phys->ds_bp;
		dsphys->ds_flags |= origin->ds_phys->ds_flags;

		dmu_buf_will_dirty(origin->ds_dbuf, tx);
		origin->ds_phys->ds_num_children++;

		VERIFY0(dsl_dataset_hold_obj(dp,
		    origin->ds_dir->dd_phys->dd_head_dataset_obj, FTAG, &ohds));
		dsphys->ds_deadlist_obj = dsl_deadlist_clone(&ohds->ds_deadlist,
		    dsphys->ds_prev_snap_txg, dsphys->ds_prev_snap_obj, tx);
		dsl_dataset_rele(ohds, FTAG);

		if (spa_version(dp->dp_spa) >= SPA_VERSION_NEXT_CLONES) {
			if (origin->ds_phys->ds_next_clones_obj == 0) {
				origin->ds_phys->ds_next_clones_obj =
				    zap_create(mos,
				    DMU_OT_NEXT_CLONES, DMU_OT_NONE, 0, tx);
			}
			VERIFY0(zap_add_int(mos,
			    origin->ds_phys->ds_next_clones_obj, dsobj, tx));
		}

		dmu_buf_will_dirty(dd->dd_dbuf, tx);
		dd->dd_phys->dd_origin_obj = origin->ds_object;
		if (spa_version(dp->dp_spa) >= SPA_VERSION_DIR_CLONES) {
			if (origin->ds_dir->dd_phys->dd_clones == 0) {
				dmu_buf_will_dirty(origin->ds_dir->dd_dbuf, tx);
				origin->ds_dir->dd_phys->dd_clones =
				    zap_create(mos,
				    DMU_OT_DSL_CLONES, DMU_OT_NONE, 0, tx);
			}
			VERIFY0(zap_add_int(mos,
			    origin->ds_dir->dd_phys->dd_clones, dsobj, tx));
		}
	}

	if (spa_version(dp->dp_spa) >= SPA_VERSION_UNIQUE_ACCURATE)
		dsphys->ds_flags |= DS_FLAG_UNIQUE_ACCURATE;

	dmu_buf_rele(dbuf, FTAG);

	dmu_buf_will_dirty(dd->dd_dbuf, tx);
	dd->dd_phys->dd_head_dataset_obj = dsobj;

	return (dsobj);
}

static void
dsl_dataset_zero_zil(dsl_dataset_t *ds, dmu_tx_t *tx)
{
	objset_t *os;

	VERIFY0(dmu_objset_from_ds(ds, &os));
	bzero(&os->os_zil_header, sizeof (os->os_zil_header));
	dsl_dataset_dirty(ds, tx);
}

uint64_t
dsl_dataset_create_sync(dsl_dir_t *pdd, const char *lastname,
    dsl_dataset_t *origin, uint64_t flags, cred_t *cr, dmu_tx_t *tx)
{
	dsl_pool_t *dp = pdd->dd_pool;
	uint64_t dsobj, ddobj;
	dsl_dir_t *dd;

	ASSERT(dmu_tx_is_syncing(tx));
	ASSERT(lastname[0] != '@');

	ddobj = dsl_dir_create_sync(dp, pdd, lastname, tx);
	VERIFY0(dsl_dir_hold_obj(dp, ddobj, lastname, FTAG, &dd));

	dsobj = dsl_dataset_create_sync_dd(dd, origin,
	    flags & ~DS_CREATE_FLAG_NODIRTY, tx);

	dsl_deleg_set_create_perms(dd, tx, cr);

	/*
	 * Since we're creating a new node we know it's a leaf, so we can
	 * initialize the counts if the limit feature is active.
	 */
	if (spa_feature_is_active(dp->dp_spa, SPA_FEATURE_FS_SS_LIMIT)) {
		uint64_t cnt = 0;
		objset_t *os = dd->dd_pool->dp_meta_objset;

		dsl_dir_zapify(dd, tx);
		VERIFY0(zap_add(os, dd->dd_object, DD_FIELD_FILESYSTEM_COUNT,
		    sizeof (cnt), 1, &cnt, tx));
		VERIFY0(zap_add(os, dd->dd_object, DD_FIELD_SNAPSHOT_COUNT,
		    sizeof (cnt), 1, &cnt, tx));
	}

	dsl_dir_rele(dd, FTAG);

	/*
	 * If we are creating a clone, make sure we zero out any stale
	 * data from the origin snapshots zil header.
	 */
	if (origin != NULL && !(flags & DS_CREATE_FLAG_NODIRTY)) {
		dsl_dataset_t *ds;

		VERIFY0(dsl_dataset_hold_obj(dp, dsobj, FTAG, &ds));
		dsl_dataset_zero_zil(ds, tx);
		dsl_dataset_rele(ds, FTAG);
	}

	return (dsobj);
}

/*
 * The unique space in the head dataset can be calculated by subtracting
 * the space used in the most recent snapshot, that is still being used
 * in this file system, from the space currently in use.  To figure out
 * the space in the most recent snapshot still in use, we need to take
 * the total space used in the snapshot and subtract out the space that
 * has been freed up since the snapshot was taken.
 */
void
dsl_dataset_recalc_head_uniq(dsl_dataset_t *ds)
{
	uint64_t mrs_used;
	uint64_t dlused, dlcomp, dluncomp;

	ASSERT(!dsl_dataset_is_snapshot(ds));

	if (ds->ds_phys->ds_prev_snap_obj != 0)
		mrs_used = ds->ds_prev->ds_phys->ds_referenced_bytes;
	else
		mrs_used = 0;

	dsl_deadlist_space(&ds->ds_deadlist, &dlused, &dlcomp, &dluncomp);

	ASSERT3U(dlused, <=, mrs_used);
	ds->ds_phys->ds_unique_bytes =
	    ds->ds_phys->ds_referenced_bytes - (mrs_used - dlused);

	if (spa_version(ds->ds_dir->dd_pool->dp_spa) >=
	    SPA_VERSION_UNIQUE_ACCURATE)
		ds->ds_phys->ds_flags |= DS_FLAG_UNIQUE_ACCURATE;
}

void
dsl_dataset_remove_from_next_clones(dsl_dataset_t *ds, uint64_t obj,
    dmu_tx_t *tx)
{
	objset_t *mos = ds->ds_dir->dd_pool->dp_meta_objset;
	uint64_t count;
	int err;

	ASSERT(ds->ds_phys->ds_num_children >= 2);
	err = zap_remove_int(mos, ds->ds_phys->ds_next_clones_obj, obj, tx);
	/*
	 * The err should not be ENOENT, but a bug in a previous version
	 * of the code could cause upgrade_clones_cb() to not set
	 * ds_next_snap_obj when it should, leading to a missing entry.
	 * If we knew that the pool was created after
	 * SPA_VERSION_NEXT_CLONES, we could assert that it isn't
	 * ENOENT.  However, at least we can check that we don't have
	 * too many entries in the next_clones_obj even after failing to
	 * remove this one.
	 */
	if (err != ENOENT)
		VERIFY0(err);
	ASSERT0(zap_count(mos, ds->ds_phys->ds_next_clones_obj,
	    &count));
	ASSERT3U(count, <=, ds->ds_phys->ds_num_children - 2);
}


blkptr_t *
dsl_dataset_get_blkptr(dsl_dataset_t *ds)
{
	return (&ds->ds_phys->ds_bp);
}

void
dsl_dataset_set_blkptr(dsl_dataset_t *ds, blkptr_t *bp, dmu_tx_t *tx)
{
	ASSERT(dmu_tx_is_syncing(tx));
	/* If it's the meta-objset, set dp_meta_rootbp */
	if (ds == NULL) {
		tx->tx_pool->dp_meta_rootbp = *bp;
	} else {
		dmu_buf_will_dirty(ds->ds_dbuf, tx);
		ds->ds_phys->ds_bp = *bp;
	}
}

spa_t *
dsl_dataset_get_spa(dsl_dataset_t *ds)
{
	return (ds->ds_dir->dd_pool->dp_spa);
}

void
dsl_dataset_dirty(dsl_dataset_t *ds, dmu_tx_t *tx)
{
	dsl_pool_t *dp;

	if (ds == NULL) /* this is the meta-objset */
		return;

	ASSERT(ds->ds_objset != NULL);

	if (ds->ds_phys->ds_next_snap_obj != 0)
		panic("dirtying snapshot!");

	dp = ds->ds_dir->dd_pool;

	if (txg_list_add(&dp->dp_dirty_datasets, ds, tx->tx_txg)) {
		/* up the hold count until we can be written out */
		dmu_buf_add_ref(ds->ds_dbuf, ds);
	}
}

boolean_t
dsl_dataset_is_dirty(dsl_dataset_t *ds)
{
	for (int t = 0; t < TXG_SIZE; t++) {
		if (txg_list_member(&ds->ds_dir->dd_pool->dp_dirty_datasets,
		    ds, t))
			return (B_TRUE);
	}
	return (B_FALSE);
}

static int
dsl_dataset_snapshot_reserve_space(dsl_dataset_t *ds, dmu_tx_t *tx)
{
	uint64_t asize;

	if (!dmu_tx_is_syncing(tx))
		return (0);

	/*
	 * If there's an fs-only reservation, any blocks that might become
	 * owned by the snapshot dataset must be accommodated by space
	 * outside of the reservation.
	 */
	ASSERT(ds->ds_reserved == 0 || DS_UNIQUE_IS_ACCURATE(ds));
	asize = MIN(ds->ds_phys->ds_unique_bytes, ds->ds_reserved);
	if (asize > dsl_dir_space_available(ds->ds_dir, NULL, 0, TRUE))
		return (SET_ERROR(ENOSPC));

	/*
	 * Propagate any reserved space for this snapshot to other
	 * snapshot checks in this sync group.
	 */
	if (asize > 0)
		dsl_dir_willuse_space(ds->ds_dir, asize, tx);

	return (0);
}

typedef struct dsl_dataset_snapshot_arg {
	nvlist_t *ddsa_snaps;
	nvlist_t *ddsa_props;
	nvlist_t *ddsa_errors;
	cred_t *ddsa_cr;
} dsl_dataset_snapshot_arg_t;

int
dsl_dataset_snapshot_check_impl(dsl_dataset_t *ds, const char *snapname,
    dmu_tx_t *tx, boolean_t recv, uint64_t cnt, cred_t *cr)
{
	int error;
	uint64_t value;

	ds->ds_trysnap_txg = tx->tx_txg;

	if (!dmu_tx_is_syncing(tx))
		return (0);

	/*
	 * We don't allow multiple snapshots of the same txg.  If there
	 * is already one, try again.
	 */
	if (ds->ds_phys->ds_prev_snap_txg >= tx->tx_txg)
		return (SET_ERROR(EAGAIN));

	/*
	 * Check for conflicting snapshot name.
	 */
	error = dsl_dataset_snap_lookup(ds, snapname, &value);
	if (error == 0)
		return (SET_ERROR(EEXIST));
	if (error != ENOENT)
		return (error);

	/*
	 * We don't allow taking snapshots of inconsistent datasets, such as
	 * those into which we are currently receiving.  However, if we are
	 * creating this snapshot as part of a receive, this check will be
	 * executed atomically with respect to the completion of the receive
	 * itself but prior to the clearing of DS_FLAG_INCONSISTENT; in this
	 * case we ignore this, knowing it will be fixed up for us shortly in
	 * dmu_recv_end_sync().
	 */
	if (!recv && DS_IS_INCONSISTENT(ds))
		return (SET_ERROR(EBUSY));

	/*
	 * Skip the check for temporary snapshots or if we have already checked
	 * the counts in dsl_dataset_snapshot_check. This means we really only
	 * check the count here when we're receiving a stream.
	 */
	if (cnt != 0 && cr != NULL) {
		error = dsl_fs_ss_limit_check(ds->ds_dir, cnt,
		    ZFS_PROP_SNAPSHOT_LIMIT, NULL, cr);
		if (error != 0)
			return (error);
	}

	error = dsl_dataset_snapshot_reserve_space(ds, tx);
	if (error != 0)
		return (error);

	return (0);
}

static int
dsl_dataset_snapshot_check(void *arg, dmu_tx_t *tx)
{
	dsl_dataset_snapshot_arg_t *ddsa = arg;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	nvpair_t *pair;
	int rv = 0;

	/*
	 * Pre-compute how many total new snapshots will be created for each
	 * level in the tree and below. This is needed for validating the
	 * snapshot limit when either taking a recursive snapshot or when
	 * taking multiple snapshots.
	 *
	 * The problem is that the counts are not actually adjusted when
	 * we are checking, only when we finally sync. For a single snapshot,
	 * this is easy, the count will increase by 1 at each node up the tree,
	 * but its more complicated for the recursive/multiple snapshot case.
	 *
	 * The dsl_fs_ss_limit_check function does recursively check the count
	 * at each level up the tree but since it is validating each snapshot
	 * independently we need to be sure that we are validating the complete
	 * count for the entire set of snapshots. We do this by rolling up the
	 * counts for each component of the name into an nvlist and then
	 * checking each of those cases with the aggregated count.
	 *
	 * This approach properly handles not only the recursive snapshot
	 * case (where we get all of those on the ddsa_snaps list) but also
	 * the sibling case (e.g. snapshot a/b and a/c so that we will also
	 * validate the limit on 'a' using a count of 2).
	 *
	 * We validate the snapshot names in the third loop and only report
	 * name errors once.
	 */
	if (dmu_tx_is_syncing(tx)) {
		nvlist_t *cnt_track = NULL;
		cnt_track = fnvlist_alloc();

		/* Rollup aggregated counts into the cnt_track list */
		for (pair = nvlist_next_nvpair(ddsa->ddsa_snaps, NULL);
		    pair != NULL;
		    pair = nvlist_next_nvpair(ddsa->ddsa_snaps, pair)) {
			char *pdelim;
			uint64_t val;
			char nm[MAXPATHLEN];

			(void) strlcpy(nm, nvpair_name(pair), sizeof (nm));
			pdelim = strchr(nm, '@');
			if (pdelim == NULL)
				continue;
			*pdelim = '\0';

			do {
				if (nvlist_lookup_uint64(cnt_track, nm,
				    &val) == 0) {
					/* update existing entry */
					fnvlist_add_uint64(cnt_track, nm,
					    val + 1);
				} else {
					/* add to list */
					fnvlist_add_uint64(cnt_track, nm, 1);
				}

				pdelim = strrchr(nm, '/');
				if (pdelim != NULL)
					*pdelim = '\0';
			} while (pdelim != NULL);
		}

		/* Check aggregated counts at each level */
		for (pair = nvlist_next_nvpair(cnt_track, NULL);
		    pair != NULL; pair = nvlist_next_nvpair(cnt_track, pair)) {
			int error = 0;
			char *name;
			uint64_t cnt = 0;
			dsl_dataset_t *ds;

			name = nvpair_name(pair);
			cnt = fnvpair_value_uint64(pair);
			ASSERT(cnt > 0);

			error = dsl_dataset_hold(dp, name, FTAG, &ds);
			if (error == 0) {
				error = dsl_fs_ss_limit_check(ds->ds_dir, cnt,
				    ZFS_PROP_SNAPSHOT_LIMIT, NULL,
				    ddsa->ddsa_cr);
				dsl_dataset_rele(ds, FTAG);
			}

			if (error != 0) {
				if (ddsa->ddsa_errors != NULL)
					fnvlist_add_int32(ddsa->ddsa_errors,
					    name, error);
				rv = error;
				/* only report one error for this check */
				break;
			}
		}
		nvlist_free(cnt_track);
	}

	for (pair = nvlist_next_nvpair(ddsa->ddsa_snaps, NULL);
	    pair != NULL; pair = nvlist_next_nvpair(ddsa->ddsa_snaps, pair)) {
		int error = 0;
		dsl_dataset_t *ds;
		char *name, *atp;
		char dsname[MAXNAMELEN];

		name = nvpair_name(pair);
		if (strlen(name) >= MAXNAMELEN)
			error = SET_ERROR(ENAMETOOLONG);
		if (error == 0) {
			atp = strchr(name, '@');
			if (atp == NULL)
				error = SET_ERROR(EINVAL);
			if (error == 0)
				(void) strlcpy(dsname, name, atp - name + 1);
		}
		if (error == 0)
			error = dsl_dataset_hold(dp, dsname, FTAG, &ds);
		if (error == 0) {
			/* passing 0/NULL skips dsl_fs_ss_limit_check */
			error = dsl_dataset_snapshot_check_impl(ds,
			    atp + 1, tx, B_FALSE, 0, NULL);
			dsl_dataset_rele(ds, FTAG);
		}

		if (error != 0) {
			if (ddsa->ddsa_errors != NULL) {
				fnvlist_add_int32(ddsa->ddsa_errors,
				    name, error);
			}
			rv = error;
		}
	}

	return (rv);
}

void
dsl_dataset_snapshot_sync_impl(dsl_dataset_t *ds, const char *snapname,
    dmu_tx_t *tx)
{
	static zil_header_t zero_zil;

	dsl_pool_t *dp = ds->ds_dir->dd_pool;
	dmu_buf_t *dbuf;
	dsl_dataset_phys_t *dsphys;
	uint64_t dsobj, crtxg;
	objset_t *mos = dp->dp_meta_objset;
	objset_t *os;

	ASSERT(RRW_WRITE_HELD(&dp->dp_config_rwlock));

	/*
	 * If we are on an old pool, the zil must not be active, in which
	 * case it will be zeroed.  Usually zil_suspend() accomplishes this.
	 */
	ASSERT(spa_version(dmu_tx_pool(tx)->dp_spa) >= SPA_VERSION_FAST_SNAP ||
	    dmu_objset_from_ds(ds, &os) != 0 ||
	    bcmp(&os->os_phys->os_zil_header, &zero_zil,
	    sizeof (zero_zil)) == 0);

	dsl_fs_ss_count_adjust(ds->ds_dir, 1, DD_FIELD_SNAPSHOT_COUNT, tx);

	/*
	 * The origin's ds_creation_txg has to be < TXG_INITIAL
	 */
	if (strcmp(snapname, ORIGIN_DIR_NAME) == 0)
		crtxg = 1;
	else
		crtxg = tx->tx_txg;

	dsobj = dmu_object_alloc(mos, DMU_OT_DSL_DATASET, 0,
	    DMU_OT_DSL_DATASET, sizeof (dsl_dataset_phys_t), tx);
	VERIFY0(dmu_bonus_hold(mos, dsobj, FTAG, &dbuf));
	dmu_buf_will_dirty(dbuf, tx);
	dsphys = dbuf->db_data;
	bzero(dsphys, sizeof (dsl_dataset_phys_t));
	dsphys->ds_dir_obj = ds->ds_dir->dd_object;
	dsphys->ds_fsid_guid = unique_create();
	(void) random_get_pseudo_bytes((void*)&dsphys->ds_guid,
	    sizeof (dsphys->ds_guid));
	dsphys->ds_prev_snap_obj = ds->ds_phys->ds_prev_snap_obj;
	dsphys->ds_prev_snap_txg = ds->ds_phys->ds_prev_snap_txg;
	dsphys->ds_next_snap_obj = ds->ds_object;
	dsphys->ds_num_children = 1;
	dsphys->ds_creation_time = gethrestime_sec();
	dsphys->ds_creation_txg = crtxg;
	dsphys->ds_deadlist_obj = ds->ds_phys->ds_deadlist_obj;
	dsphys->ds_referenced_bytes = ds->ds_phys->ds_referenced_bytes;
	dsphys->ds_compressed_bytes = ds->ds_phys->ds_compressed_bytes;
	dsphys->ds_uncompressed_bytes = ds->ds_phys->ds_uncompressed_bytes;
	dsphys->ds_flags = ds->ds_phys->ds_flags;
	dsphys->ds_bp = ds->ds_phys->ds_bp;
	dmu_buf_rele(dbuf, FTAG);

	ASSERT3U(ds->ds_prev != 0, ==, ds->ds_phys->ds_prev_snap_obj != 0);
	if (ds->ds_prev) {
		uint64_t next_clones_obj =
		    ds->ds_prev->ds_phys->ds_next_clones_obj;
		ASSERT(ds->ds_prev->ds_phys->ds_next_snap_obj ==
		    ds->ds_object ||
		    ds->ds_prev->ds_phys->ds_num_children > 1);
		if (ds->ds_prev->ds_phys->ds_next_snap_obj == ds->ds_object) {
			dmu_buf_will_dirty(ds->ds_prev->ds_dbuf, tx);
			ASSERT3U(ds->ds_phys->ds_prev_snap_txg, ==,
			    ds->ds_prev->ds_phys->ds_creation_txg);
			ds->ds_prev->ds_phys->ds_next_snap_obj = dsobj;
		} else if (next_clones_obj != 0) {
			dsl_dataset_remove_from_next_clones(ds->ds_prev,
			    dsphys->ds_next_snap_obj, tx);
			VERIFY0(zap_add_int(mos,
			    next_clones_obj, dsobj, tx));
		}
	}

	/*
	 * If we have a reference-reservation on this dataset, we will
	 * need to increase the amount of refreservation being charged
	 * since our unique space is going to zero.
	 */
	if (ds->ds_reserved) {
		int64_t delta;
		ASSERT(DS_UNIQUE_IS_ACCURATE(ds));
		delta = MIN(ds->ds_phys->ds_unique_bytes, ds->ds_reserved);
		dsl_dir_diduse_space(ds->ds_dir, DD_USED_REFRSRV,
		    delta, 0, 0, tx);
	}

	dmu_buf_will_dirty(ds->ds_dbuf, tx);
	ds->ds_phys->ds_deadlist_obj = dsl_deadlist_clone(&ds->ds_deadlist,
	    UINT64_MAX, ds->ds_phys->ds_prev_snap_obj, tx);
	dsl_deadlist_close(&ds->ds_deadlist);
	dsl_deadlist_open(&ds->ds_deadlist, mos, ds->ds_phys->ds_deadlist_obj);
	dsl_deadlist_add_key(&ds->ds_deadlist,
	    ds->ds_phys->ds_prev_snap_txg, tx);

	ASSERT3U(ds->ds_phys->ds_prev_snap_txg, <, tx->tx_txg);
	ds->ds_phys->ds_prev_snap_obj = dsobj;
	ds->ds_phys->ds_prev_snap_txg = crtxg;
	ds->ds_phys->ds_unique_bytes = 0;
	if (spa_version(dp->dp_spa) >= SPA_VERSION_UNIQUE_ACCURATE)
		ds->ds_phys->ds_flags |= DS_FLAG_UNIQUE_ACCURATE;

	VERIFY0(zap_add(mos, ds->ds_phys->ds_snapnames_zapobj,
	    snapname, 8, 1, &dsobj, tx));

	if (ds->ds_prev)
		dsl_dataset_rele(ds->ds_prev, ds);
	VERIFY0(dsl_dataset_hold_obj(dp,
	    ds->ds_phys->ds_prev_snap_obj, ds, &ds->ds_prev));

	dsl_scan_ds_snapshotted(ds, tx);

	dsl_dir_snap_cmtime_update(ds->ds_dir);

	spa_history_log_internal_ds(ds->ds_prev, "snapshot", tx, "");
}

static void
dsl_dataset_snapshot_sync(void *arg, dmu_tx_t *tx)
{
	dsl_dataset_snapshot_arg_t *ddsa = arg;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	nvpair_t *pair;

	for (pair = nvlist_next_nvpair(ddsa->ddsa_snaps, NULL);
	    pair != NULL; pair = nvlist_next_nvpair(ddsa->ddsa_snaps, pair)) {
		dsl_dataset_t *ds;
		char *name, *atp;
		char dsname[MAXNAMELEN];

		name = nvpair_name(pair);
		atp = strchr(name, '@');
		(void) strlcpy(dsname, name, atp - name + 1);
		VERIFY0(dsl_dataset_hold(dp, dsname, FTAG, &ds));

		dsl_dataset_snapshot_sync_impl(ds, atp + 1, tx);
		if (ddsa->ddsa_props != NULL) {
			dsl_props_set_sync_impl(ds->ds_prev,
			    ZPROP_SRC_LOCAL, ddsa->ddsa_props, tx);
		}
		dsl_dataset_rele(ds, FTAG);
	}
}

/*
 * The snapshots must all be in the same pool.
 * All-or-nothing: if there are any failures, nothing will be modified.
 */
int
dsl_dataset_snapshot(nvlist_t *snaps, nvlist_t *props, nvlist_t *errors)
{
	dsl_dataset_snapshot_arg_t ddsa;
	nvpair_t *pair;
	boolean_t needsuspend;
	int error;
	spa_t *spa;
	char *firstname;
	nvlist_t *suspended = NULL;

	pair = nvlist_next_nvpair(snaps, NULL);
	if (pair == NULL)
		return (0);
	firstname = nvpair_name(pair);

	error = spa_open(firstname, &spa, FTAG);
	if (error != 0)
		return (error);
	needsuspend = (spa_version(spa) < SPA_VERSION_FAST_SNAP);
	spa_close(spa, FTAG);

	if (needsuspend) {
		suspended = fnvlist_alloc();
		for (pair = nvlist_next_nvpair(snaps, NULL); pair != NULL;
		    pair = nvlist_next_nvpair(snaps, pair)) {
			char fsname[MAXNAMELEN];
			char *snapname = nvpair_name(pair);
			char *atp;
			void *cookie;

			atp = strchr(snapname, '@');
			if (atp == NULL) {
				error = SET_ERROR(EINVAL);
				break;
			}
			(void) strlcpy(fsname, snapname, atp - snapname + 1);

			error = zil_suspend(fsname, &cookie);
			if (error != 0)
				break;
			fnvlist_add_uint64(suspended, fsname,
			    (uintptr_t)cookie);
		}
	}

	ddsa.ddsa_snaps = snaps;
	ddsa.ddsa_props = props;
	ddsa.ddsa_errors = errors;
	ddsa.ddsa_cr = CRED();

	if (error == 0) {
		error = dsl_sync_task(firstname, dsl_dataset_snapshot_check,
		    dsl_dataset_snapshot_sync, &ddsa,
		    fnvlist_num_pairs(snaps) * 3);
	}

	if (suspended != NULL) {
		for (pair = nvlist_next_nvpair(suspended, NULL); pair != NULL;
		    pair = nvlist_next_nvpair(suspended, pair)) {
			zil_resume((void *)(uintptr_t)
			    fnvpair_value_uint64(pair));
		}
		fnvlist_free(suspended);
	}

	return (error);
}

typedef struct dsl_dataset_snapshot_tmp_arg {
	const char *ddsta_fsname;
	const char *ddsta_snapname;
	minor_t ddsta_cleanup_minor;
	const char *ddsta_htag;
} dsl_dataset_snapshot_tmp_arg_t;

static int
dsl_dataset_snapshot_tmp_check(void *arg, dmu_tx_t *tx)
{
	dsl_dataset_snapshot_tmp_arg_t *ddsta = arg;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	dsl_dataset_t *ds;
	int error;

	error = dsl_dataset_hold(dp, ddsta->ddsta_fsname, FTAG, &ds);
	if (error != 0)
		return (error);

	/* NULL cred means no limit check for tmp snapshot */
	error = dsl_dataset_snapshot_check_impl(ds, ddsta->ddsta_snapname,
	    tx, B_FALSE, 0, NULL);
	if (error != 0) {
		dsl_dataset_rele(ds, FTAG);
		return (error);
	}

	if (spa_version(dp->dp_spa) < SPA_VERSION_USERREFS) {
		dsl_dataset_rele(ds, FTAG);
		return (SET_ERROR(ENOTSUP));
	}
	error = dsl_dataset_user_hold_check_one(NULL, ddsta->ddsta_htag,
	    B_TRUE, tx);
	if (error != 0) {
		dsl_dataset_rele(ds, FTAG);
		return (error);
	}

	dsl_dataset_rele(ds, FTAG);
	return (0);
}

static void
dsl_dataset_snapshot_tmp_sync(void *arg, dmu_tx_t *tx)
{
	dsl_dataset_snapshot_tmp_arg_t *ddsta = arg;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	dsl_dataset_t *ds;

	VERIFY0(dsl_dataset_hold(dp, ddsta->ddsta_fsname, FTAG, &ds));

	dsl_dataset_snapshot_sync_impl(ds, ddsta->ddsta_snapname, tx);
	dsl_dataset_user_hold_sync_one(ds->ds_prev, ddsta->ddsta_htag,
	    ddsta->ddsta_cleanup_minor, gethrestime_sec(), tx);
	dsl_destroy_snapshot_sync_impl(ds->ds_prev, B_TRUE, tx);

	dsl_dataset_rele(ds, FTAG);
}

int
dsl_dataset_snapshot_tmp(const char *fsname, const char *snapname,
    minor_t cleanup_minor, const char *htag)
{
	dsl_dataset_snapshot_tmp_arg_t ddsta;
	int error;
	spa_t *spa;
	boolean_t needsuspend;
	void *cookie;

	ddsta.ddsta_fsname = fsname;
	ddsta.ddsta_snapname = snapname;
	ddsta.ddsta_cleanup_minor = cleanup_minor;
	ddsta.ddsta_htag = htag;

	error = spa_open(fsname, &spa, FTAG);
	if (error != 0)
		return (error);
	needsuspend = (spa_version(spa) < SPA_VERSION_FAST_SNAP);
	spa_close(spa, FTAG);

	if (needsuspend) {
		error = zil_suspend(fsname, &cookie);
		if (error != 0)
			return (error);
	}

	error = dsl_sync_task(fsname, dsl_dataset_snapshot_tmp_check,
	    dsl_dataset_snapshot_tmp_sync, &ddsta, 3);

	if (needsuspend)
		zil_resume(cookie);
	return (error);
}


void
dsl_dataset_sync(dsl_dataset_t *ds, zio_t *zio, dmu_tx_t *tx)
{
	ASSERT(dmu_tx_is_syncing(tx));
	ASSERT(ds->ds_objset != NULL);
	ASSERT(ds->ds_phys->ds_next_snap_obj == 0);

	/*
	 * in case we had to change ds_fsid_guid when we opened it,
	 * sync it out now.
	 */
	dmu_buf_will_dirty(ds->ds_dbuf, tx);
	ds->ds_phys->ds_fsid_guid = ds->ds_fsid_guid;

	dmu_objset_sync(ds->ds_objset, zio, tx);
}

static void
get_clones_stat(dsl_dataset_t *ds, nvlist_t *nv)
{
	uint64_t count = 0;
	objset_t *mos = ds->ds_dir->dd_pool->dp_meta_objset;
	zap_cursor_t zc;
	zap_attribute_t za;
	nvlist_t *propval = fnvlist_alloc();
	nvlist_t *val = fnvlist_alloc();

	ASSERT(dsl_pool_config_held(ds->ds_dir->dd_pool));

	/*
	 * There may be missing entries in ds_next_clones_obj
	 * due to a bug in a previous version of the code.
	 * Only trust it if it has the right number of entries.
	 */
	if (ds->ds_phys->ds_next_clones_obj != 0) {
		VERIFY0(zap_count(mos, ds->ds_phys->ds_next_clones_obj,
		    &count));
	}
	if (count != ds->ds_phys->ds_num_children - 1)
		goto fail;
	for (zap_cursor_init(&zc, mos, ds->ds_phys->ds_next_clones_obj);
	    zap_cursor_retrieve(&zc, &za) == 0;
	    zap_cursor_advance(&zc)) {
		dsl_dataset_t *clone;
		char buf[ZFS_MAXNAMELEN];
		VERIFY0(dsl_dataset_hold_obj(ds->ds_dir->dd_pool,
		    za.za_first_integer, FTAG, &clone));
		dsl_dir_name(clone->ds_dir, buf);
		fnvlist_add_boolean(val, buf);
		dsl_dataset_rele(clone, FTAG);
	}
	zap_cursor_fini(&zc);
	fnvlist_add_nvlist(propval, ZPROP_VALUE, val);
	fnvlist_add_nvlist(nv, zfs_prop_to_name(ZFS_PROP_CLONES), propval);
fail:
	nvlist_free(val);
	nvlist_free(propval);
}

void
dsl_dataset_stats(dsl_dataset_t *ds, nvlist_t *nv)
{
	dsl_pool_t *dp = ds->ds_dir->dd_pool;
	uint64_t refd, avail, uobjs, aobjs, ratio;

	ASSERT(dsl_pool_config_held(dp));

	ratio = ds->ds_phys->ds_compressed_bytes == 0 ? 100 :
	    (ds->ds_phys->ds_uncompressed_bytes * 100 /
	    ds->ds_phys->ds_compressed_bytes);

	dsl_prop_nvlist_add_uint64(nv, ZFS_PROP_REFRATIO, ratio);
	dsl_prop_nvlist_add_uint64(nv, ZFS_PROP_LOGICALREFERENCED,
	    ds->ds_phys->ds_uncompressed_bytes);

	if (dsl_dataset_is_snapshot(ds)) {
		dsl_prop_nvlist_add_uint64(nv, ZFS_PROP_COMPRESSRATIO, ratio);
		dsl_prop_nvlist_add_uint64(nv, ZFS_PROP_USED,
		    ds->ds_phys->ds_unique_bytes);
		get_clones_stat(ds, nv);
	} else {
		dsl_dir_stats(ds->ds_dir, nv);
	}

	dsl_dataset_space(ds, &refd, &avail, &uobjs, &aobjs);
	dsl_prop_nvlist_add_uint64(nv, ZFS_PROP_AVAILABLE, avail);
	dsl_prop_nvlist_add_uint64(nv, ZFS_PROP_REFERENCED, refd);

	dsl_prop_nvlist_add_uint64(nv, ZFS_PROP_CREATION,
	    ds->ds_phys->ds_creation_time);
	dsl_prop_nvlist_add_uint64(nv, ZFS_PROP_CREATETXG,
	    ds->ds_phys->ds_creation_txg);
	dsl_prop_nvlist_add_uint64(nv, ZFS_PROP_REFQUOTA,
	    ds->ds_quota);
	dsl_prop_nvlist_add_uint64(nv, ZFS_PROP_REFRESERVATION,
	    ds->ds_reserved);
	dsl_prop_nvlist_add_uint64(nv, ZFS_PROP_GUID,
	    ds->ds_phys->ds_guid);
	dsl_prop_nvlist_add_uint64(nv, ZFS_PROP_UNIQUE,
	    ds->ds_phys->ds_unique_bytes);
	dsl_prop_nvlist_add_uint64(nv, ZFS_PROP_OBJSETID,
	    ds->ds_object);
	dsl_prop_nvlist_add_uint64(nv, ZFS_PROP_USERREFS,
	    ds->ds_userrefs);
	dsl_prop_nvlist_add_uint64(nv, ZFS_PROP_DEFER_DESTROY,
	    DS_IS_DEFER_DESTROY(ds) ? 1 : 0);

	if (ds->ds_phys->ds_prev_snap_obj != 0) {
		uint64_t written, comp, uncomp;
		dsl_pool_t *dp = ds->ds_dir->dd_pool;
		dsl_dataset_t *prev;

		int err = dsl_dataset_hold_obj(dp,
		    ds->ds_phys->ds_prev_snap_obj, FTAG, &prev);
		if (err == 0) {
			err = dsl_dataset_space_written(prev, ds, &written,
			    &comp, &uncomp);
			dsl_dataset_rele(prev, FTAG);
			if (err == 0) {
				dsl_prop_nvlist_add_uint64(nv, ZFS_PROP_WRITTEN,
				    written);
			}
		}
	}
}

void
dsl_dataset_fast_stat(dsl_dataset_t *ds, dmu_objset_stats_t *stat)
{
	dsl_pool_t *dp = ds->ds_dir->dd_pool;
	ASSERT(dsl_pool_config_held(dp));

	stat->dds_creation_txg = ds->ds_phys->ds_creation_txg;
	stat->dds_inconsistent = ds->ds_phys->ds_flags & DS_FLAG_INCONSISTENT;
	stat->dds_guid = ds->ds_phys->ds_guid;
	stat->dds_origin[0] = '\0';
	if (dsl_dataset_is_snapshot(ds)) {
		stat->dds_is_snapshot = B_TRUE;
		stat->dds_num_clones = ds->ds_phys->ds_num_children - 1;
	} else {
		stat->dds_is_snapshot = B_FALSE;
		stat->dds_num_clones = 0;

		if (dsl_dir_is_clone(ds->ds_dir)) {
			dsl_dataset_t *ods;

			VERIFY0(dsl_dataset_hold_obj(dp,
			    ds->ds_dir->dd_phys->dd_origin_obj, FTAG, &ods));
			dsl_dataset_name(ods, stat->dds_origin);
			dsl_dataset_rele(ods, FTAG);
		}
	}
}

uint64_t
dsl_dataset_fsid_guid(dsl_dataset_t *ds)
{
	return (ds->ds_fsid_guid);
}

void
dsl_dataset_space(dsl_dataset_t *ds,
    uint64_t *refdbytesp, uint64_t *availbytesp,
    uint64_t *usedobjsp, uint64_t *availobjsp)
{
	*refdbytesp = ds->ds_phys->ds_referenced_bytes;
	*availbytesp = dsl_dir_space_available(ds->ds_dir, NULL, 0, TRUE);
	if (ds->ds_reserved > ds->ds_phys->ds_unique_bytes)
		*availbytesp += ds->ds_reserved - ds->ds_phys->ds_unique_bytes;
	if (ds->ds_quota != 0) {
		/*
		 * Adjust available bytes according to refquota
		 */
		if (*refdbytesp < ds->ds_quota)
			*availbytesp = MIN(*availbytesp,
			    ds->ds_quota - *refdbytesp);
		else
			*availbytesp = 0;
	}
	*usedobjsp = ds->ds_phys->ds_bp.blk_fill;
	*availobjsp = DN_MAX_OBJECT - *usedobjsp;
}

boolean_t
dsl_dataset_modified_since_snap(dsl_dataset_t *ds, dsl_dataset_t *snap)
{
	dsl_pool_t *dp = ds->ds_dir->dd_pool;

	ASSERT(dsl_pool_config_held(dp));
	if (snap == NULL)
		return (B_FALSE);
	if (ds->ds_phys->ds_bp.blk_birth >
	    snap->ds_phys->ds_creation_txg) {
		objset_t *os, *os_snap;
		/*
		 * It may be that only the ZIL differs, because it was
		 * reset in the head.  Don't count that as being
		 * modified.
		 */
		if (dmu_objset_from_ds(ds, &os) != 0)
			return (B_TRUE);
		if (dmu_objset_from_ds(snap, &os_snap) != 0)
			return (B_TRUE);
		return (bcmp(&os->os_phys->os_meta_dnode,
		    &os_snap->os_phys->os_meta_dnode,
		    sizeof (os->os_phys->os_meta_dnode)) != 0);
	}
	return (B_FALSE);
}

typedef struct dsl_dataset_rename_snapshot_arg {
	const char *ddrsa_fsname;
	const char *ddrsa_oldsnapname;
	const char *ddrsa_newsnapname;
	boolean_t ddrsa_recursive;
	dmu_tx_t *ddrsa_tx;
} dsl_dataset_rename_snapshot_arg_t;

/* ARGSUSED */
static int
dsl_dataset_rename_snapshot_check_impl(dsl_pool_t *dp,
    dsl_dataset_t *hds, void *arg)
{
	dsl_dataset_rename_snapshot_arg_t *ddrsa = arg;
	int error;
	uint64_t val;

	error = dsl_dataset_snap_lookup(hds, ddrsa->ddrsa_oldsnapname, &val);
	if (error != 0) {
		/* ignore nonexistent snapshots */
		return (error == ENOENT ? 0 : error);
	}

	/* new name should not exist */
	error = dsl_dataset_snap_lookup(hds, ddrsa->ddrsa_newsnapname, &val);
	if (error == 0)
		error = SET_ERROR(EEXIST);
	else if (error == ENOENT)
		error = 0;

	/* dataset name + 1 for the "@" + the new snapshot name must fit */
	if (dsl_dir_namelen(hds->ds_dir) + 1 +
	    strlen(ddrsa->ddrsa_newsnapname) >= MAXNAMELEN)
		error = SET_ERROR(ENAMETOOLONG);

	return (error);
}

static int
dsl_dataset_rename_snapshot_check(void *arg, dmu_tx_t *tx)
{
	dsl_dataset_rename_snapshot_arg_t *ddrsa = arg;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	dsl_dataset_t *hds;
	int error;

	error = dsl_dataset_hold(dp, ddrsa->ddrsa_fsname, FTAG, &hds);
	if (error != 0)
		return (error);

	if (ddrsa->ddrsa_recursive) {
		error = dmu_objset_find_dp(dp, hds->ds_dir->dd_object,
		    dsl_dataset_rename_snapshot_check_impl, ddrsa,
		    DS_FIND_CHILDREN);
	} else {
		error = dsl_dataset_rename_snapshot_check_impl(dp, hds, ddrsa);
	}
	dsl_dataset_rele(hds, FTAG);
	return (error);
}

static int
dsl_dataset_rename_snapshot_sync_impl(dsl_pool_t *dp,
    dsl_dataset_t *hds, void *arg)
{
	dsl_dataset_rename_snapshot_arg_t *ddrsa = arg;
	dsl_dataset_t *ds;
	uint64_t val;
	dmu_tx_t *tx = ddrsa->ddrsa_tx;
	int error;

	error = dsl_dataset_snap_lookup(hds, ddrsa->ddrsa_oldsnapname, &val);
	ASSERT(error == 0 || error == ENOENT);
	if (error == ENOENT) {
		/* ignore nonexistent snapshots */
		return (0);
	}

	VERIFY0(dsl_dataset_hold_obj(dp, val, FTAG, &ds));

	/* log before we change the name */
	spa_history_log_internal_ds(ds, "rename", tx,
	    "-> @%s", ddrsa->ddrsa_newsnapname);

	VERIFY0(dsl_dataset_snap_remove(hds, ddrsa->ddrsa_oldsnapname, tx,
	    B_FALSE));
	mutex_enter(&ds->ds_lock);
	(void) strcpy(ds->ds_snapname, ddrsa->ddrsa_newsnapname);
	mutex_exit(&ds->ds_lock);
	VERIFY0(zap_add(dp->dp_meta_objset, hds->ds_phys->ds_snapnames_zapobj,
	    ds->ds_snapname, 8, 1, &ds->ds_object, tx));

	dsl_dataset_rele(ds, FTAG);
	return (0);
}

static void
dsl_dataset_rename_snapshot_sync(void *arg, dmu_tx_t *tx)
{
	dsl_dataset_rename_snapshot_arg_t *ddrsa = arg;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	dsl_dataset_t *hds;

	VERIFY0(dsl_dataset_hold(dp, ddrsa->ddrsa_fsname, FTAG, &hds));
	ddrsa->ddrsa_tx = tx;
	if (ddrsa->ddrsa_recursive) {
		VERIFY0(dmu_objset_find_dp(dp, hds->ds_dir->dd_object,
		    dsl_dataset_rename_snapshot_sync_impl, ddrsa,
		    DS_FIND_CHILDREN));
	} else {
		VERIFY0(dsl_dataset_rename_snapshot_sync_impl(dp, hds, ddrsa));
	}
	dsl_dataset_rele(hds, FTAG);
}

int
dsl_dataset_rename_snapshot(const char *fsname,
    const char *oldsnapname, const char *newsnapname, boolean_t recursive)
{
	dsl_dataset_rename_snapshot_arg_t ddrsa;

	ddrsa.ddrsa_fsname = fsname;
	ddrsa.ddrsa_oldsnapname = oldsnapname;
	ddrsa.ddrsa_newsnapname = newsnapname;
	ddrsa.ddrsa_recursive = recursive;

	return (dsl_sync_task(fsname, dsl_dataset_rename_snapshot_check,
	    dsl_dataset_rename_snapshot_sync, &ddrsa, 1));
}

/*
 * If we're doing an ownership handoff, we need to make sure that there is
 * only one long hold on the dataset.  We're not allowed to change anything here
 * so we don't permanently release the long hold or regular hold here.  We want
 * to do this only when syncing to avoid the dataset unexpectedly going away
 * when we release the long hold.
 */
static int
dsl_dataset_handoff_check(dsl_dataset_t *ds, void *owner, dmu_tx_t *tx)
{
	boolean_t held;

	if (!dmu_tx_is_syncing(tx))
		return (0);

	if (owner != NULL) {
		VERIFY3P(ds->ds_owner, ==, owner);
		dsl_dataset_long_rele(ds, owner);
	}

	held = dsl_dataset_long_held(ds);

	if (owner != NULL)
		dsl_dataset_long_hold(ds, owner);

	if (held)
		return (SET_ERROR(EBUSY));

	return (0);
}

typedef struct dsl_dataset_rollback_arg {
	const char *ddra_fsname;
	void *ddra_owner;
	nvlist_t *ddra_result;
} dsl_dataset_rollback_arg_t;

static int
dsl_dataset_rollback_check(void *arg, dmu_tx_t *tx)
{
	dsl_dataset_rollback_arg_t *ddra = arg;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	dsl_dataset_t *ds;
	int64_t unused_refres_delta;
	int error;

	error = dsl_dataset_hold(dp, ddra->ddra_fsname, FTAG, &ds);
	if (error != 0)
		return (error);

	/* must not be a snapshot */
	if (dsl_dataset_is_snapshot(ds)) {
		dsl_dataset_rele(ds, FTAG);
		return (SET_ERROR(EINVAL));
	}

	/* must have a most recent snapshot */
	if (ds->ds_phys->ds_prev_snap_txg < TXG_INITIAL) {
		dsl_dataset_rele(ds, FTAG);
		return (SET_ERROR(EINVAL));
	}

	/* must not have any bookmarks after the most recent snapshot */
	nvlist_t *proprequest = fnvlist_alloc();
	fnvlist_add_boolean(proprequest, zfs_prop_to_name(ZFS_PROP_CREATETXG));
	nvlist_t *bookmarks = fnvlist_alloc();
	error = dsl_get_bookmarks_impl(ds, proprequest, bookmarks);
	fnvlist_free(proprequest);
	if (error != 0)
		return (error);
	for (nvpair_t *pair = nvlist_next_nvpair(bookmarks, NULL);
	    pair != NULL; pair = nvlist_next_nvpair(bookmarks, pair)) {
		nvlist_t *valuenv =
		    fnvlist_lookup_nvlist(fnvpair_value_nvlist(pair),
		    zfs_prop_to_name(ZFS_PROP_CREATETXG));
		uint64_t createtxg = fnvlist_lookup_uint64(valuenv, "value");
		if (createtxg > ds->ds_phys->ds_prev_snap_txg) {
			fnvlist_free(bookmarks);
			dsl_dataset_rele(ds, FTAG);
			return (SET_ERROR(EEXIST));
		}
	}
	fnvlist_free(bookmarks);

	error = dsl_dataset_handoff_check(ds, ddra->ddra_owner, tx);
	if (error != 0) {
		dsl_dataset_rele(ds, FTAG);
		return (error);
	}

	/*
	 * Check if the snap we are rolling back to uses more than
	 * the refquota.
	 */
	if (ds->ds_quota != 0 &&
	    ds->ds_prev->ds_phys->ds_referenced_bytes > ds->ds_quota) {
		dsl_dataset_rele(ds, FTAG);
		return (SET_ERROR(EDQUOT));
	}

	/*
	 * When we do the clone swap, we will temporarily use more space
	 * due to the refreservation (the head will no longer have any
	 * unique space, so the entire amount of the refreservation will need
	 * to be free).  We will immediately destroy the clone, freeing
	 * this space, but the freeing happens over many txg's.
	 */
	unused_refres_delta = (int64_t)MIN(ds->ds_reserved,
	    ds->ds_phys->ds_unique_bytes);

	if (unused_refres_delta > 0 &&
	    unused_refres_delta >
	    dsl_dir_space_available(ds->ds_dir, NULL, 0, TRUE)) {
		dsl_dataset_rele(ds, FTAG);
		return (SET_ERROR(ENOSPC));
	}

	dsl_dataset_rele(ds, FTAG);
	return (0);
}

static void
dsl_dataset_rollback_sync(void *arg, dmu_tx_t *tx)
{
	dsl_dataset_rollback_arg_t *ddra = arg;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	dsl_dataset_t *ds, *clone;
	uint64_t cloneobj;
	char namebuf[ZFS_MAXNAMELEN];

	VERIFY0(dsl_dataset_hold(dp, ddra->ddra_fsname, FTAG, &ds));

	dsl_dataset_name(ds->ds_prev, namebuf);
	fnvlist_add_string(ddra->ddra_result, "target", namebuf);

	cloneobj = dsl_dataset_create_sync(ds->ds_dir, "%rollback",
	    ds->ds_prev, DS_CREATE_FLAG_NODIRTY, kcred, tx);

	VERIFY0(dsl_dataset_hold_obj(dp, cloneobj, FTAG, &clone));

	dsl_dataset_clone_swap_sync_impl(clone, ds, tx);
	dsl_dataset_zero_zil(ds, tx);

	dsl_destroy_head_sync_impl(clone, tx);

	dsl_dataset_rele(clone, FTAG);
	dsl_dataset_rele(ds, FTAG);
}

/*
 * Rolls back the given filesystem or volume to the most recent snapshot.
 * The name of the most recent snapshot will be returned under key "target"
 * in the result nvlist.
 *
 * If owner != NULL:
 * - The existing dataset MUST be owned by the specified owner at entry
 * - Upon return, dataset will still be held by the same owner, whether we
 *   succeed or not.
 *
 * This mode is required any time the existing filesystem is mounted.  See
 * notes above zfs_suspend_fs() for further details.
 */
int
dsl_dataset_rollback(const char *fsname, void *owner, nvlist_t *result)
{
	dsl_dataset_rollback_arg_t ddra;

	ddra.ddra_fsname = fsname;
	ddra.ddra_owner = owner;
	ddra.ddra_result = result;

	return (dsl_sync_task(fsname, dsl_dataset_rollback_check,
	    dsl_dataset_rollback_sync, &ddra, 1));
}

struct promotenode {
	list_node_t link;
	dsl_dataset_t *ds;
};

typedef struct dsl_dataset_promote_arg {
	const char *ddpa_clonename;
	dsl_dataset_t *ddpa_clone;
	list_t shared_snaps, origin_snaps, clone_snaps;
	dsl_dataset_t *origin_origin; /* origin of the origin */
	uint64_t used, comp, uncomp, unique, cloneusedsnap, originusedsnap;
	char *err_ds;
	cred_t *cr;
} dsl_dataset_promote_arg_t;

static int snaplist_space(list_t *l, uint64_t mintxg, uint64_t *spacep);
static int promote_hold(dsl_dataset_promote_arg_t *ddpa, dsl_pool_t *dp,
    void *tag);
static void promote_rele(dsl_dataset_promote_arg_t *ddpa, void *tag);

static int
dsl_dataset_promote_check(void *arg, dmu_tx_t *tx)
{
	dsl_dataset_promote_arg_t *ddpa = arg;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	dsl_dataset_t *hds;
	struct promotenode *snap;
	dsl_dataset_t *origin_ds;
	int err;
	uint64_t unused;
	uint64_t ss_mv_cnt;

	err = promote_hold(ddpa, dp, FTAG);
	if (err != 0)
		return (err);

	hds = ddpa->ddpa_clone;

	if (hds->ds_phys->ds_flags & DS_FLAG_NOPROMOTE) {
		promote_rele(ddpa, FTAG);
		return (SET_ERROR(EXDEV));
	}

	/*
	 * Compute and check the amount of space to transfer.  Since this is
	 * so expensive, don't do the preliminary check.
	 */
	if (!dmu_tx_is_syncing(tx)) {
		promote_rele(ddpa, FTAG);
		return (0);
	}

	snap = list_head(&ddpa->shared_snaps);
	origin_ds = snap->ds;

	/* compute origin's new unique space */
	snap = list_tail(&ddpa->clone_snaps);
	ASSERT3U(snap->ds->ds_phys->ds_prev_snap_obj, ==, origin_ds->ds_object);
	dsl_deadlist_space_range(&snap->ds->ds_deadlist,
	    origin_ds->ds_phys->ds_prev_snap_txg, UINT64_MAX,
	    &ddpa->unique, &unused, &unused);

	/*
	 * Walk the snapshots that we are moving
	 *
	 * Compute space to transfer.  Consider the incremental changes
	 * to used by each snapshot:
	 * (my used) = (prev's used) + (blocks born) - (blocks killed)
	 * So each snapshot gave birth to:
	 * (blocks born) = (my used) - (prev's used) + (blocks killed)
	 * So a sequence would look like:
	 * (uN - u(N-1) + kN) + ... + (u1 - u0 + k1) + (u0 - 0 + k0)
	 * Which simplifies to:
	 * uN + kN + kN-1 + ... + k1 + k0
	 * Note however, if we stop before we reach the ORIGIN we get:
	 * uN + kN + kN-1 + ... + kM - uM-1
	 */
	ss_mv_cnt = 0;
	ddpa->used = origin_ds->ds_phys->ds_referenced_bytes;
	ddpa->comp = origin_ds->ds_phys->ds_compressed_bytes;
	ddpa->uncomp = origin_ds->ds_phys->ds_uncompressed_bytes;
	for (snap = list_head(&ddpa->shared_snaps); snap;
	    snap = list_next(&ddpa->shared_snaps, snap)) {
		uint64_t val, dlused, dlcomp, dluncomp;
		dsl_dataset_t *ds = snap->ds;

		ss_mv_cnt++;

		/*
		 * If there are long holds, we won't be able to evict
		 * the objset.
		 */
		if (dsl_dataset_long_held(ds)) {
			err = SET_ERROR(EBUSY);
			goto out;
		}

		/* Check that the snapshot name does not conflict */
		VERIFY0(dsl_dataset_get_snapname(ds));
		err = dsl_dataset_snap_lookup(hds, ds->ds_snapname, &val);
		if (err == 0) {
			(void) strcpy(ddpa->err_ds, snap->ds->ds_snapname);
			err = SET_ERROR(EEXIST);
			goto out;
		}
		if (err != ENOENT)
			goto out;

		/* The very first snapshot does not have a deadlist */
		if (ds->ds_phys->ds_prev_snap_obj == 0)
			continue;

		dsl_deadlist_space(&ds->ds_deadlist,
		    &dlused, &dlcomp, &dluncomp);
		ddpa->used += dlused;
		ddpa->comp += dlcomp;
		ddpa->uncomp += dluncomp;
	}

	/*
	 * If we are a clone of a clone then we never reached ORIGIN,
	 * so we need to subtract out the clone origin's used space.
	 */
	if (ddpa->origin_origin) {
		ddpa->used -= ddpa->origin_origin->ds_phys->ds_referenced_bytes;
		ddpa->comp -= ddpa->origin_origin->ds_phys->ds_compressed_bytes;
		ddpa->uncomp -=
		    ddpa->origin_origin->ds_phys->ds_uncompressed_bytes;
	}

	/* Check that there is enough space and limit headroom here */
	err = dsl_dir_transfer_possible(origin_ds->ds_dir, hds->ds_dir,
	    0, ss_mv_cnt, ddpa->used, ddpa->cr);
	if (err != 0)
		goto out;

	/*
	 * Compute the amounts of space that will be used by snapshots
	 * after the promotion (for both origin and clone).  For each,
	 * it is the amount of space that will be on all of their
	 * deadlists (that was not born before their new origin).
	 */
	if (hds->ds_dir->dd_phys->dd_flags & DD_FLAG_USED_BREAKDOWN) {
		uint64_t space;

		/*
		 * Note, typically this will not be a clone of a clone,
		 * so dd_origin_txg will be < TXG_INITIAL, so
		 * these snaplist_space() -> dsl_deadlist_space_range()
		 * calls will be fast because they do not have to
		 * iterate over all bps.
		 */
		snap = list_head(&ddpa->origin_snaps);
		err = snaplist_space(&ddpa->shared_snaps,
		    snap->ds->ds_dir->dd_origin_txg, &ddpa->cloneusedsnap);
		if (err != 0)
			goto out;

		err = snaplist_space(&ddpa->clone_snaps,
		    snap->ds->ds_dir->dd_origin_txg, &space);
		if (err != 0)
			goto out;
		ddpa->cloneusedsnap += space;
	}
	if (origin_ds->ds_dir->dd_phys->dd_flags & DD_FLAG_USED_BREAKDOWN) {
		err = snaplist_space(&ddpa->origin_snaps,
		    origin_ds->ds_phys->ds_creation_txg, &ddpa->originusedsnap);
		if (err != 0)
			goto out;
	}

out:
	promote_rele(ddpa, FTAG);
	return (err);
}

static void
dsl_dataset_promote_sync(void *arg, dmu_tx_t *tx)
{
	dsl_dataset_promote_arg_t *ddpa = arg;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	dsl_dataset_t *hds;
	struct promotenode *snap;
	dsl_dataset_t *origin_ds;
	dsl_dataset_t *origin_head;
	dsl_dir_t *dd;
	dsl_dir_t *odd = NULL;
	uint64_t oldnext_obj;
	int64_t delta;

	VERIFY0(promote_hold(ddpa, dp, FTAG));
	hds = ddpa->ddpa_clone;

	ASSERT0(hds->ds_phys->ds_flags & DS_FLAG_NOPROMOTE);

	snap = list_head(&ddpa->shared_snaps);
	origin_ds = snap->ds;
	dd = hds->ds_dir;

	snap = list_head(&ddpa->origin_snaps);
	origin_head = snap->ds;

	/*
	 * We need to explicitly open odd, since origin_ds's dd will be
	 * changing.
	 */
	VERIFY0(dsl_dir_hold_obj(dp, origin_ds->ds_dir->dd_object,
	    NULL, FTAG, &odd));

	/* change origin's next snap */
	dmu_buf_will_dirty(origin_ds->ds_dbuf, tx);
	oldnext_obj = origin_ds->ds_phys->ds_next_snap_obj;
	snap = list_tail(&ddpa->clone_snaps);
	ASSERT3U(snap->ds->ds_phys->ds_prev_snap_obj, ==, origin_ds->ds_object);
	origin_ds->ds_phys->ds_next_snap_obj = snap->ds->ds_object;

	/* change the origin's next clone */
	if (origin_ds->ds_phys->ds_next_clones_obj) {
		dsl_dataset_remove_from_next_clones(origin_ds,
		    snap->ds->ds_object, tx);
		VERIFY0(zap_add_int(dp->dp_meta_objset,
		    origin_ds->ds_phys->ds_next_clones_obj,
		    oldnext_obj, tx));
	}

	/* change origin */
	dmu_buf_will_dirty(dd->dd_dbuf, tx);
	ASSERT3U(dd->dd_phys->dd_origin_obj, ==, origin_ds->ds_object);
	dd->dd_phys->dd_origin_obj = odd->dd_phys->dd_origin_obj;
	dd->dd_origin_txg = origin_head->ds_dir->dd_origin_txg;
	dmu_buf_will_dirty(odd->dd_dbuf, tx);
	odd->dd_phys->dd_origin_obj = origin_ds->ds_object;
	origin_head->ds_dir->dd_origin_txg =
	    origin_ds->ds_phys->ds_creation_txg;

	/* change dd_clone entries */
	if (spa_version(dp->dp_spa) >= SPA_VERSION_DIR_CLONES) {
		VERIFY0(zap_remove_int(dp->dp_meta_objset,
		    odd->dd_phys->dd_clones, hds->ds_object, tx));
		VERIFY0(zap_add_int(dp->dp_meta_objset,
		    ddpa->origin_origin->ds_dir->dd_phys->dd_clones,
		    hds->ds_object, tx));

		VERIFY0(zap_remove_int(dp->dp_meta_objset,
		    ddpa->origin_origin->ds_dir->dd_phys->dd_clones,
		    origin_head->ds_object, tx));
		if (dd->dd_phys->dd_clones == 0) {
			dd->dd_phys->dd_clones = zap_create(dp->dp_meta_objset,
			    DMU_OT_DSL_CLONES, DMU_OT_NONE, 0, tx);
		}
		VERIFY0(zap_add_int(dp->dp_meta_objset,
		    dd->dd_phys->dd_clones, origin_head->ds_object, tx));
	}

	/* move snapshots to this dir */
	for (snap = list_head(&ddpa->shared_snaps); snap;
	    snap = list_next(&ddpa->shared_snaps, snap)) {
		dsl_dataset_t *ds = snap->ds;

		/*
		 * Property callbacks are registered to a particular
		 * dsl_dir.  Since ours is changing, evict the objset
		 * so that they will be unregistered from the old dsl_dir.
		 */
		if (ds->ds_objset) {
			dmu_objset_evict(ds->ds_objset);
			ds->ds_objset = NULL;
		}

		/* move snap name entry */
		VERIFY0(dsl_dataset_get_snapname(ds));
		VERIFY0(dsl_dataset_snap_remove(origin_head,
		    ds->ds_snapname, tx, B_TRUE));
		VERIFY0(zap_add(dp->dp_meta_objset,
		    hds->ds_phys->ds_snapnames_zapobj, ds->ds_snapname,
		    8, 1, &ds->ds_object, tx));
		dsl_fs_ss_count_adjust(hds->ds_dir, 1,
		    DD_FIELD_SNAPSHOT_COUNT, tx);

		/* change containing dsl_dir */
		dmu_buf_will_dirty(ds->ds_dbuf, tx);
		ASSERT3U(ds->ds_phys->ds_dir_obj, ==, odd->dd_object);
		ds->ds_phys->ds_dir_obj = dd->dd_object;
		ASSERT3P(ds->ds_dir, ==, odd);
		dsl_dir_rele(ds->ds_dir, ds);
		VERIFY0(dsl_dir_hold_obj(dp, dd->dd_object,
		    NULL, ds, &ds->ds_dir));

		/* move any clone references */
		if (ds->ds_phys->ds_next_clones_obj &&
		    spa_version(dp->dp_spa) >= SPA_VERSION_DIR_CLONES) {
			zap_cursor_t zc;
			zap_attribute_t za;

			for (zap_cursor_init(&zc, dp->dp_meta_objset,
			    ds->ds_phys->ds_next_clones_obj);
			    zap_cursor_retrieve(&zc, &za) == 0;
			    zap_cursor_advance(&zc)) {
				dsl_dataset_t *cnds;
				uint64_t o;

				if (za.za_first_integer == oldnext_obj) {
					/*
					 * We've already moved the
					 * origin's reference.
					 */
					continue;
				}

				VERIFY0(dsl_dataset_hold_obj(dp,
				    za.za_first_integer, FTAG, &cnds));
				o = cnds->ds_dir->dd_phys->dd_head_dataset_obj;

				VERIFY0(zap_remove_int(dp->dp_meta_objset,
				    odd->dd_phys->dd_clones, o, tx));
				VERIFY0(zap_add_int(dp->dp_meta_objset,
				    dd->dd_phys->dd_clones, o, tx));
				dsl_dataset_rele(cnds, FTAG);
			}
			zap_cursor_fini(&zc);
		}

		ASSERT(!dsl_prop_hascb(ds));
	}

	/*
	 * Change space accounting.
	 * Note, pa->*usedsnap and dd_used_breakdown[SNAP] will either
	 * both be valid, or both be 0 (resulting in delta == 0).  This
	 * is true for each of {clone,origin} independently.
	 */

	delta = ddpa->cloneusedsnap -
	    dd->dd_phys->dd_used_breakdown[DD_USED_SNAP];
	ASSERT3S(delta, >=, 0);
	ASSERT3U(ddpa->used, >=, delta);
	dsl_dir_diduse_space(dd, DD_USED_SNAP, delta, 0, 0, tx);
	dsl_dir_diduse_space(dd, DD_USED_HEAD,
	    ddpa->used - delta, ddpa->comp, ddpa->uncomp, tx);

	delta = ddpa->originusedsnap -
	    odd->dd_phys->dd_used_breakdown[DD_USED_SNAP];
	ASSERT3S(delta, <=, 0);
	ASSERT3U(ddpa->used, >=, -delta);
	dsl_dir_diduse_space(odd, DD_USED_SNAP, delta, 0, 0, tx);
	dsl_dir_diduse_space(odd, DD_USED_HEAD,
	    -ddpa->used - delta, -ddpa->comp, -ddpa->uncomp, tx);

	origin_ds->ds_phys->ds_unique_bytes = ddpa->unique;

	/* log history record */
	spa_history_log_internal_ds(hds, "promote", tx, "");

	dsl_dir_rele(odd, FTAG);
	promote_rele(ddpa, FTAG);
}

/*
 * Make a list of dsl_dataset_t's for the snapshots between first_obj
 * (exclusive) and last_obj (inclusive).  The list will be in reverse
 * order (last_obj will be the list_head()).  If first_obj == 0, do all
 * snapshots back to this dataset's origin.
 */
static int
snaplist_make(dsl_pool_t *dp,
    uint64_t first_obj, uint64_t last_obj, list_t *l, void *tag)
{
	uint64_t obj = last_obj;

	list_create(l, sizeof (struct promotenode),
	    offsetof(struct promotenode, link));

	while (obj != first_obj) {
		dsl_dataset_t *ds;
		struct promotenode *snap;
		int err;

		err = dsl_dataset_hold_obj(dp, obj, tag, &ds);
		ASSERT(err != ENOENT);
		if (err != 0)
			return (err);

		if (first_obj == 0)
			first_obj = ds->ds_dir->dd_phys->dd_origin_obj;

		snap = kmem_alloc(sizeof (*snap), KM_SLEEP);
		snap->ds = ds;
		list_insert_tail(l, snap);
		obj = ds->ds_phys->ds_prev_snap_obj;
	}

	return (0);
}

static int
snaplist_space(list_t *l, uint64_t mintxg, uint64_t *spacep)
{
	struct promotenode *snap;

	*spacep = 0;
	for (snap = list_head(l); snap; snap = list_next(l, snap)) {
		uint64_t used, comp, uncomp;
		dsl_deadlist_space_range(&snap->ds->ds_deadlist,
		    mintxg, UINT64_MAX, &used, &comp, &uncomp);
		*spacep += used;
	}
	return (0);
}

static void
snaplist_destroy(list_t *l, void *tag)
{
	struct promotenode *snap;

	if (l == NULL || !list_link_active(&l->list_head))
		return;

	while ((snap = list_tail(l)) != NULL) {
		list_remove(l, snap);
		dsl_dataset_rele(snap->ds, tag);
		kmem_free(snap, sizeof (*snap));
	}
	list_destroy(l);
}

static int
promote_hold(dsl_dataset_promote_arg_t *ddpa, dsl_pool_t *dp, void *tag)
{
	int error;
	dsl_dir_t *dd;
	struct promotenode *snap;

	error = dsl_dataset_hold(dp, ddpa->ddpa_clonename, tag,
	    &ddpa->ddpa_clone);
	if (error != 0)
		return (error);
	dd = ddpa->ddpa_clone->ds_dir;

	if (dsl_dataset_is_snapshot(ddpa->ddpa_clone) ||
	    !dsl_dir_is_clone(dd)) {
		dsl_dataset_rele(ddpa->ddpa_clone, tag);
		return (SET_ERROR(EINVAL));
	}

	error = snaplist_make(dp, 0, dd->dd_phys->dd_origin_obj,
	    &ddpa->shared_snaps, tag);
	if (error != 0)
		goto out;

	error = snaplist_make(dp, 0, ddpa->ddpa_clone->ds_object,
	    &ddpa->clone_snaps, tag);
	if (error != 0)
		goto out;

	snap = list_head(&ddpa->shared_snaps);
	ASSERT3U(snap->ds->ds_object, ==, dd->dd_phys->dd_origin_obj);
	error = snaplist_make(dp, dd->dd_phys->dd_origin_obj,
	    snap->ds->ds_dir->dd_phys->dd_head_dataset_obj,
	    &ddpa->origin_snaps, tag);
	if (error != 0)
		goto out;

	if (snap->ds->ds_dir->dd_phys->dd_origin_obj != 0) {
		error = dsl_dataset_hold_obj(dp,
		    snap->ds->ds_dir->dd_phys->dd_origin_obj,
		    tag, &ddpa->origin_origin);
		if (error != 0)
			goto out;
	}
out:
	if (error != 0)
		promote_rele(ddpa, tag);
	return (error);
}

static void
promote_rele(dsl_dataset_promote_arg_t *ddpa, void *tag)
{
	snaplist_destroy(&ddpa->shared_snaps, tag);
	snaplist_destroy(&ddpa->clone_snaps, tag);
	snaplist_destroy(&ddpa->origin_snaps, tag);
	if (ddpa->origin_origin != NULL)
		dsl_dataset_rele(ddpa->origin_origin, tag);
	dsl_dataset_rele(ddpa->ddpa_clone, tag);
}

/*
 * Promote a clone.
 *
 * If it fails due to a conflicting snapshot name, "conflsnap" will be filled
 * in with the name.  (It must be at least MAXNAMELEN bytes long.)
 */
int
dsl_dataset_promote(const char *name, char *conflsnap)
{
	dsl_dataset_promote_arg_t ddpa = { 0 };
	uint64_t numsnaps;
	int error;
	objset_t *os;

	/*
	 * We will modify space proportional to the number of
	 * snapshots.  Compute numsnaps.
	 */
	error = dmu_objset_hold(name, FTAG, &os);
	if (error != 0)
		return (error);
	error = zap_count(dmu_objset_pool(os)->dp_meta_objset,
	    dmu_objset_ds(os)->ds_phys->ds_snapnames_zapobj, &numsnaps);
	dmu_objset_rele(os, FTAG);
	if (error != 0)
		return (error);

	ddpa.ddpa_clonename = name;
	ddpa.err_ds = conflsnap;
	ddpa.cr = CRED();

	return (dsl_sync_task(name, dsl_dataset_promote_check,
	    dsl_dataset_promote_sync, &ddpa, 2 + numsnaps));
}

int
dsl_dataset_clone_swap_check_impl(dsl_dataset_t *clone,
    dsl_dataset_t *origin_head, boolean_t force, void *owner, dmu_tx_t *tx)
{
	int64_t unused_refres_delta;

	/* they should both be heads */
	if (dsl_dataset_is_snapshot(clone) ||
	    dsl_dataset_is_snapshot(origin_head))
		return (SET_ERROR(EINVAL));

	/* if we are not forcing, the branch point should be just before them */
	if (!force && clone->ds_prev != origin_head->ds_prev)
		return (SET_ERROR(EINVAL));

	/* clone should be the clone (unless they are unrelated) */
	if (clone->ds_prev != NULL &&
	    clone->ds_prev != clone->ds_dir->dd_pool->dp_origin_snap &&
	    origin_head->ds_dir != clone->ds_prev->ds_dir)
		return (SET_ERROR(EINVAL));

	/* the clone should be a child of the origin */
	if (clone->ds_dir->dd_parent != origin_head->ds_dir)
		return (SET_ERROR(EINVAL));

	/* origin_head shouldn't be modified unless 'force' */
	if (!force &&
	    dsl_dataset_modified_since_snap(origin_head, origin_head->ds_prev))
		return (SET_ERROR(ETXTBSY));

	/* origin_head should have no long holds (e.g. is not mounted) */
	if (dsl_dataset_handoff_check(origin_head, owner, tx))
		return (SET_ERROR(EBUSY));

	/* check amount of any unconsumed refreservation */
	unused_refres_delta =
	    (int64_t)MIN(origin_head->ds_reserved,
	    origin_head->ds_phys->ds_unique_bytes) -
	    (int64_t)MIN(origin_head->ds_reserved,
	    clone->ds_phys->ds_unique_bytes);

	if (unused_refres_delta > 0 &&
	    unused_refres_delta >
	    dsl_dir_space_available(origin_head->ds_dir, NULL, 0, TRUE))
		return (SET_ERROR(ENOSPC));

	/* clone can't be over the head's refquota */
	if (origin_head->ds_quota != 0 &&
	    clone->ds_phys->ds_referenced_bytes > origin_head->ds_quota)
		return (SET_ERROR(EDQUOT));

	return (0);
}

void
dsl_dataset_clone_swap_sync_impl(dsl_dataset_t *clone,
    dsl_dataset_t *origin_head, dmu_tx_t *tx)
{
	dsl_pool_t *dp = dmu_tx_pool(tx);
	int64_t unused_refres_delta;

	ASSERT(clone->ds_reserved == 0);
	ASSERT(origin_head->ds_quota == 0 ||
	    clone->ds_phys->ds_unique_bytes <= origin_head->ds_quota);
	ASSERT3P(clone->ds_prev, ==, origin_head->ds_prev);

	dmu_buf_will_dirty(clone->ds_dbuf, tx);
	dmu_buf_will_dirty(origin_head->ds_dbuf, tx);

	if (clone->ds_objset != NULL) {
		dmu_objset_evict(clone->ds_objset);
		clone->ds_objset = NULL;
	}

	if (origin_head->ds_objset != NULL) {
		dmu_objset_evict(origin_head->ds_objset);
		origin_head->ds_objset = NULL;
	}

	unused_refres_delta =
	    (int64_t)MIN(origin_head->ds_reserved,
	    origin_head->ds_phys->ds_unique_bytes) -
	    (int64_t)MIN(origin_head->ds_reserved,
	    clone->ds_phys->ds_unique_bytes);

	/*
	 * Reset origin's unique bytes, if it exists.
	 */
	if (clone->ds_prev) {
		dsl_dataset_t *origin = clone->ds_prev;
		uint64_t comp, uncomp;

		dmu_buf_will_dirty(origin->ds_dbuf, tx);
		dsl_deadlist_space_range(&clone->ds_deadlist,
		    origin->ds_phys->ds_prev_snap_txg, UINT64_MAX,
		    &origin->ds_phys->ds_unique_bytes, &comp, &uncomp);
	}

	/* swap blkptrs */
	{
		blkptr_t tmp;
		tmp = origin_head->ds_phys->ds_bp;
		origin_head->ds_phys->ds_bp = clone->ds_phys->ds_bp;
		clone->ds_phys->ds_bp = tmp;
	}

	/* set dd_*_bytes */
	{
		int64_t dused, dcomp, duncomp;
		uint64_t cdl_used, cdl_comp, cdl_uncomp;
		uint64_t odl_used, odl_comp, odl_uncomp;

		ASSERT3U(clone->ds_dir->dd_phys->
		    dd_used_breakdown[DD_USED_SNAP], ==, 0);

		dsl_deadlist_space(&clone->ds_deadlist,
		    &cdl_used, &cdl_comp, &cdl_uncomp);
		dsl_deadlist_space(&origin_head->ds_deadlist,
		    &odl_used, &odl_comp, &odl_uncomp);

		dused = clone->ds_phys->ds_referenced_bytes + cdl_used -
		    (origin_head->ds_phys->ds_referenced_bytes + odl_used);
		dcomp = clone->ds_phys->ds_compressed_bytes + cdl_comp -
		    (origin_head->ds_phys->ds_compressed_bytes + odl_comp);
		duncomp = clone->ds_phys->ds_uncompressed_bytes +
		    cdl_uncomp -
		    (origin_head->ds_phys->ds_uncompressed_bytes + odl_uncomp);

		dsl_dir_diduse_space(origin_head->ds_dir, DD_USED_HEAD,
		    dused, dcomp, duncomp, tx);
		dsl_dir_diduse_space(clone->ds_dir, DD_USED_HEAD,
		    -dused, -dcomp, -duncomp, tx);

		/*
		 * The difference in the space used by snapshots is the
		 * difference in snapshot space due to the head's
		 * deadlist (since that's the only thing that's
		 * changing that affects the snapused).
		 */
		dsl_deadlist_space_range(&clone->ds_deadlist,
		    origin_head->ds_dir->dd_origin_txg, UINT64_MAX,
		    &cdl_used, &cdl_comp, &cdl_uncomp);
		dsl_deadlist_space_range(&origin_head->ds_deadlist,
		    origin_head->ds_dir->dd_origin_txg, UINT64_MAX,
		    &odl_used, &odl_comp, &odl_uncomp);
		dsl_dir_transfer_space(origin_head->ds_dir, cdl_used - odl_used,
		    DD_USED_HEAD, DD_USED_SNAP, tx);
	}

	/* swap ds_*_bytes */
	SWITCH64(origin_head->ds_phys->ds_referenced_bytes,
	    clone->ds_phys->ds_referenced_bytes);
	SWITCH64(origin_head->ds_phys->ds_compressed_bytes,
	    clone->ds_phys->ds_compressed_bytes);
	SWITCH64(origin_head->ds_phys->ds_uncompressed_bytes,
	    clone->ds_phys->ds_uncompressed_bytes);
	SWITCH64(origin_head->ds_phys->ds_unique_bytes,
	    clone->ds_phys->ds_unique_bytes);

	/* apply any parent delta for change in unconsumed refreservation */
	dsl_dir_diduse_space(origin_head->ds_dir, DD_USED_REFRSRV,
	    unused_refres_delta, 0, 0, tx);

	/*
	 * Swap deadlists.
	 */
	dsl_deadlist_close(&clone->ds_deadlist);
	dsl_deadlist_close(&origin_head->ds_deadlist);
	SWITCH64(origin_head->ds_phys->ds_deadlist_obj,
	    clone->ds_phys->ds_deadlist_obj);
	dsl_deadlist_open(&clone->ds_deadlist, dp->dp_meta_objset,
	    clone->ds_phys->ds_deadlist_obj);
	dsl_deadlist_open(&origin_head->ds_deadlist, dp->dp_meta_objset,
	    origin_head->ds_phys->ds_deadlist_obj);

	dsl_scan_ds_clone_swapped(origin_head, clone, tx);

	spa_history_log_internal_ds(clone, "clone swap", tx,
	    "parent=%s", origin_head->ds_dir->dd_myname);
}

/*
 * Given a pool name and a dataset object number in that pool,
 * return the name of that dataset.
 */
int
dsl_dsobj_to_dsname(char *pname, uint64_t obj, char *buf)
{
	dsl_pool_t *dp;
	dsl_dataset_t *ds;
	int error;

	error = dsl_pool_hold(pname, FTAG, &dp);
	if (error != 0)
		return (error);

	error = dsl_dataset_hold_obj(dp, obj, FTAG, &ds);
	if (error == 0) {
		dsl_dataset_name(ds, buf);
		dsl_dataset_rele(ds, FTAG);
	}
	dsl_pool_rele(dp, FTAG);

	return (error);
}

int
dsl_dataset_check_quota(dsl_dataset_t *ds, boolean_t check_quota,
    uint64_t asize, uint64_t inflight, uint64_t *used, uint64_t *ref_rsrv)
{
	int error = 0;

	ASSERT3S(asize, >, 0);

	/*
	 * *ref_rsrv is the portion of asize that will come from any
	 * unconsumed refreservation space.
	 */
	*ref_rsrv = 0;

	mutex_enter(&ds->ds_lock);
	/*
	 * Make a space adjustment for reserved bytes.
	 */
	if (ds->ds_reserved > ds->ds_phys->ds_unique_bytes) {
		ASSERT3U(*used, >=,
		    ds->ds_reserved - ds->ds_phys->ds_unique_bytes);
		*used -= (ds->ds_reserved - ds->ds_phys->ds_unique_bytes);
		*ref_rsrv =
		    asize - MIN(asize, parent_delta(ds, asize + inflight));
	}

	if (!check_quota || ds->ds_quota == 0) {
		mutex_exit(&ds->ds_lock);
		return (0);
	}
	/*
	 * If they are requesting more space, and our current estimate
	 * is over quota, they get to try again unless the actual
	 * on-disk is over quota and there are no pending changes (which
	 * may free up space for us).
	 */
	if (ds->ds_phys->ds_referenced_bytes + inflight >= ds->ds_quota) {
		if (inflight > 0 ||
		    ds->ds_phys->ds_referenced_bytes < ds->ds_quota)
			error = SET_ERROR(ERESTART);
		else
			error = SET_ERROR(EDQUOT);
	}
	mutex_exit(&ds->ds_lock);

	return (error);
}

typedef struct dsl_dataset_set_qr_arg {
	const char *ddsqra_name;
	zprop_source_t ddsqra_source;
	uint64_t ddsqra_value;
} dsl_dataset_set_qr_arg_t;


/* ARGSUSED */
static int
dsl_dataset_set_refquota_check(void *arg, dmu_tx_t *tx)
{
	dsl_dataset_set_qr_arg_t *ddsqra = arg;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	dsl_dataset_t *ds;
	int error;
	uint64_t newval;

	if (spa_version(dp->dp_spa) < SPA_VERSION_REFQUOTA)
		return (SET_ERROR(ENOTSUP));

	error = dsl_dataset_hold(dp, ddsqra->ddsqra_name, FTAG, &ds);
	if (error != 0)
		return (error);

	if (dsl_dataset_is_snapshot(ds)) {
		dsl_dataset_rele(ds, FTAG);
		return (SET_ERROR(EINVAL));
	}

	error = dsl_prop_predict(ds->ds_dir,
	    zfs_prop_to_name(ZFS_PROP_REFQUOTA),
	    ddsqra->ddsqra_source, ddsqra->ddsqra_value, &newval);
	if (error != 0) {
		dsl_dataset_rele(ds, FTAG);
		return (error);
	}

	if (newval == 0) {
		dsl_dataset_rele(ds, FTAG);
		return (0);
	}

	if (newval < ds->ds_phys->ds_referenced_bytes ||
	    newval < ds->ds_reserved) {
		dsl_dataset_rele(ds, FTAG);
		return (SET_ERROR(ENOSPC));
	}

	dsl_dataset_rele(ds, FTAG);
	return (0);
}

static void
dsl_dataset_set_refquota_sync(void *arg, dmu_tx_t *tx)
{
	dsl_dataset_set_qr_arg_t *ddsqra = arg;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	dsl_dataset_t *ds;
	uint64_t newval;

	VERIFY0(dsl_dataset_hold(dp, ddsqra->ddsqra_name, FTAG, &ds));

	dsl_prop_set_sync_impl(ds,
	    zfs_prop_to_name(ZFS_PROP_REFQUOTA),
	    ddsqra->ddsqra_source, sizeof (ddsqra->ddsqra_value), 1,
	    &ddsqra->ddsqra_value, tx);

	VERIFY0(dsl_prop_get_int_ds(ds,
	    zfs_prop_to_name(ZFS_PROP_REFQUOTA), &newval));

	if (ds->ds_quota != newval) {
		dmu_buf_will_dirty(ds->ds_dbuf, tx);
		ds->ds_quota = newval;
	}
	dsl_dataset_rele(ds, FTAG);
}

int
dsl_dataset_set_refquota(const char *dsname, zprop_source_t source,
    uint64_t refquota)
{
	dsl_dataset_set_qr_arg_t ddsqra;

	ddsqra.ddsqra_name = dsname;
	ddsqra.ddsqra_source = source;
	ddsqra.ddsqra_value = refquota;

	return (dsl_sync_task(dsname, dsl_dataset_set_refquota_check,
	    dsl_dataset_set_refquota_sync, &ddsqra, 0));
}

static int
dsl_dataset_set_refreservation_check(void *arg, dmu_tx_t *tx)
{
	dsl_dataset_set_qr_arg_t *ddsqra = arg;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	dsl_dataset_t *ds;
	int error;
	uint64_t newval, unique;

	if (spa_version(dp->dp_spa) < SPA_VERSION_REFRESERVATION)
		return (SET_ERROR(ENOTSUP));

	error = dsl_dataset_hold(dp, ddsqra->ddsqra_name, FTAG, &ds);
	if (error != 0)
		return (error);

	if (dsl_dataset_is_snapshot(ds)) {
		dsl_dataset_rele(ds, FTAG);
		return (SET_ERROR(EINVAL));
	}

	error = dsl_prop_predict(ds->ds_dir,
	    zfs_prop_to_name(ZFS_PROP_REFRESERVATION),
	    ddsqra->ddsqra_source, ddsqra->ddsqra_value, &newval);
	if (error != 0) {
		dsl_dataset_rele(ds, FTAG);
		return (error);
	}

	/*
	 * If we are doing the preliminary check in open context, the
	 * space estimates may be inaccurate.
	 */
	if (!dmu_tx_is_syncing(tx)) {
		dsl_dataset_rele(ds, FTAG);
		return (0);
	}

	mutex_enter(&ds->ds_lock);
	if (!DS_UNIQUE_IS_ACCURATE(ds))
		dsl_dataset_recalc_head_uniq(ds);
	unique = ds->ds_phys->ds_unique_bytes;
	mutex_exit(&ds->ds_lock);

	if (MAX(unique, newval) > MAX(unique, ds->ds_reserved)) {
		uint64_t delta = MAX(unique, newval) -
		    MAX(unique, ds->ds_reserved);

		if (delta >
		    dsl_dir_space_available(ds->ds_dir, NULL, 0, B_TRUE) ||
		    (ds->ds_quota > 0 && newval > ds->ds_quota)) {
			dsl_dataset_rele(ds, FTAG);
			return (SET_ERROR(ENOSPC));
		}
	}

	dsl_dataset_rele(ds, FTAG);
	return (0);
}

void
dsl_dataset_set_refreservation_sync_impl(dsl_dataset_t *ds,
    zprop_source_t source, uint64_t value, dmu_tx_t *tx)
{
	uint64_t newval;
	uint64_t unique;
	int64_t delta;

	dsl_prop_set_sync_impl(ds, zfs_prop_to_name(ZFS_PROP_REFRESERVATION),
	    source, sizeof (value), 1, &value, tx);

	VERIFY0(dsl_prop_get_int_ds(ds,
	    zfs_prop_to_name(ZFS_PROP_REFRESERVATION), &newval));

	dmu_buf_will_dirty(ds->ds_dbuf, tx);
	mutex_enter(&ds->ds_dir->dd_lock);
	mutex_enter(&ds->ds_lock);
	ASSERT(DS_UNIQUE_IS_ACCURATE(ds));
	unique = ds->ds_phys->ds_unique_bytes;
	delta = MAX(0, (int64_t)(newval - unique)) -
	    MAX(0, (int64_t)(ds->ds_reserved - unique));
	ds->ds_reserved = newval;
	mutex_exit(&ds->ds_lock);

	dsl_dir_diduse_space(ds->ds_dir, DD_USED_REFRSRV, delta, 0, 0, tx);
	mutex_exit(&ds->ds_dir->dd_lock);
}

static void
dsl_dataset_set_refreservation_sync(void *arg, dmu_tx_t *tx)
{
	dsl_dataset_set_qr_arg_t *ddsqra = arg;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	dsl_dataset_t *ds;

	VERIFY0(dsl_dataset_hold(dp, ddsqra->ddsqra_name, FTAG, &ds));
	dsl_dataset_set_refreservation_sync_impl(ds,
	    ddsqra->ddsqra_source, ddsqra->ddsqra_value, tx);
	dsl_dataset_rele(ds, FTAG);
}

int
dsl_dataset_set_refreservation(const char *dsname, zprop_source_t source,
    uint64_t refreservation)
{
	dsl_dataset_set_qr_arg_t ddsqra;

	ddsqra.ddsqra_name = dsname;
	ddsqra.ddsqra_source = source;
	ddsqra.ddsqra_value = refreservation;

	return (dsl_sync_task(dsname, dsl_dataset_set_refreservation_check,
	    dsl_dataset_set_refreservation_sync, &ddsqra, 0));
}

/*
 * Return (in *usedp) the amount of space written in new that is not
 * present in oldsnap.  New may be a snapshot or the head.  Old must be
 * a snapshot before new, in new's filesystem (or its origin).  If not then
 * fail and return EINVAL.
 *
 * The written space is calculated by considering two components:  First, we
 * ignore any freed space, and calculate the written as new's used space
 * minus old's used space.  Next, we add in the amount of space that was freed
 * between the two snapshots, thus reducing new's used space relative to old's.
 * Specifically, this is the space that was born before old->ds_creation_txg,
 * and freed before new (ie. on new's deadlist or a previous deadlist).
 *
 * space freed                         [---------------------]
 * snapshots                       ---O-------O--------O-------O------
 *                                         oldsnap            new
 */
int
dsl_dataset_space_written(dsl_dataset_t *oldsnap, dsl_dataset_t *new,
    uint64_t *usedp, uint64_t *compp, uint64_t *uncompp)
{
	int err = 0;
	uint64_t snapobj;
	dsl_pool_t *dp = new->ds_dir->dd_pool;

	ASSERT(dsl_pool_config_held(dp));

	*usedp = 0;
	*usedp += new->ds_phys->ds_referenced_bytes;
	*usedp -= oldsnap->ds_phys->ds_referenced_bytes;

	*compp = 0;
	*compp += new->ds_phys->ds_compressed_bytes;
	*compp -= oldsnap->ds_phys->ds_compressed_bytes;

	*uncompp = 0;
	*uncompp += new->ds_phys->ds_uncompressed_bytes;
	*uncompp -= oldsnap->ds_phys->ds_uncompressed_bytes;

	snapobj = new->ds_object;
	while (snapobj != oldsnap->ds_object) {
		dsl_dataset_t *snap;
		uint64_t used, comp, uncomp;

		if (snapobj == new->ds_object) {
			snap = new;
		} else {
			err = dsl_dataset_hold_obj(dp, snapobj, FTAG, &snap);
			if (err != 0)
				break;
		}

		if (snap->ds_phys->ds_prev_snap_txg ==
		    oldsnap->ds_phys->ds_creation_txg) {
			/*
			 * The blocks in the deadlist can not be born after
			 * ds_prev_snap_txg, so get the whole deadlist space,
			 * which is more efficient (especially for old-format
			 * deadlists).  Unfortunately the deadlist code
			 * doesn't have enough information to make this
			 * optimization itself.
			 */
			dsl_deadlist_space(&snap->ds_deadlist,
			    &used, &comp, &uncomp);
		} else {
			dsl_deadlist_space_range(&snap->ds_deadlist,
			    0, oldsnap->ds_phys->ds_creation_txg,
			    &used, &comp, &uncomp);
		}
		*usedp += used;
		*compp += comp;
		*uncompp += uncomp;

		/*
		 * If we get to the beginning of the chain of snapshots
		 * (ds_prev_snap_obj == 0) before oldsnap, then oldsnap
		 * was not a snapshot of/before new.
		 */
		snapobj = snap->ds_phys->ds_prev_snap_obj;
		if (snap != new)
			dsl_dataset_rele(snap, FTAG);
		if (snapobj == 0) {
			err = SET_ERROR(EINVAL);
			break;
		}

	}
	return (err);
}

/*
 * Return (in *usedp) the amount of space that will be reclaimed if firstsnap,
 * lastsnap, and all snapshots in between are deleted.
 *
 * blocks that would be freed            [---------------------------]
 * snapshots                       ---O-------O--------O-------O--------O
 *                                        firstsnap        lastsnap
 *
 * This is the set of blocks that were born after the snap before firstsnap,
 * (birth > firstsnap->prev_snap_txg) and died before the snap after the
 * last snap (ie, is on lastsnap->ds_next->ds_deadlist or an earlier deadlist).
 * We calculate this by iterating over the relevant deadlists (from the snap
 * after lastsnap, backward to the snap after firstsnap), summing up the
 * space on the deadlist that was born after the snap before firstsnap.
 */
int
dsl_dataset_space_wouldfree(dsl_dataset_t *firstsnap,
    dsl_dataset_t *lastsnap,
    uint64_t *usedp, uint64_t *compp, uint64_t *uncompp)
{
	int err = 0;
	uint64_t snapobj;
	dsl_pool_t *dp = firstsnap->ds_dir->dd_pool;

	ASSERT(dsl_dataset_is_snapshot(firstsnap));
	ASSERT(dsl_dataset_is_snapshot(lastsnap));

	/*
	 * Check that the snapshots are in the same dsl_dir, and firstsnap
	 * is before lastsnap.
	 */
	if (firstsnap->ds_dir != lastsnap->ds_dir ||
	    firstsnap->ds_phys->ds_creation_txg >
	    lastsnap->ds_phys->ds_creation_txg)
		return (SET_ERROR(EINVAL));

	*usedp = *compp = *uncompp = 0;

	snapobj = lastsnap->ds_phys->ds_next_snap_obj;
	while (snapobj != firstsnap->ds_object) {
		dsl_dataset_t *ds;
		uint64_t used, comp, uncomp;

		err = dsl_dataset_hold_obj(dp, snapobj, FTAG, &ds);
		if (err != 0)
			break;

		dsl_deadlist_space_range(&ds->ds_deadlist,
		    firstsnap->ds_phys->ds_prev_snap_txg, UINT64_MAX,
		    &used, &comp, &uncomp);
		*usedp += used;
		*compp += comp;
		*uncompp += uncomp;

		snapobj = ds->ds_phys->ds_prev_snap_obj;
		ASSERT3U(snapobj, !=, 0);
		dsl_dataset_rele(ds, FTAG);
	}
	return (err);
}

/*
 * Return TRUE if 'earlier' is an earlier snapshot in 'later's timeline.
 * For example, they could both be snapshots of the same filesystem, and
 * 'earlier' is before 'later'.  Or 'earlier' could be the origin of
 * 'later's filesystem.  Or 'earlier' could be an older snapshot in the origin's
 * filesystem.  Or 'earlier' could be the origin's origin.
 *
 * If non-zero, earlier_txg is used instead of earlier's ds_creation_txg.
 */
boolean_t
dsl_dataset_is_before(dsl_dataset_t *later, dsl_dataset_t *earlier,
	uint64_t earlier_txg)
{
	dsl_pool_t *dp = later->ds_dir->dd_pool;
	int error;
	boolean_t ret;

	ASSERT(dsl_pool_config_held(dp));
	ASSERT(dsl_dataset_is_snapshot(earlier) || earlier_txg != 0);

	if (earlier_txg == 0)
		earlier_txg = earlier->ds_phys->ds_creation_txg;

	if (dsl_dataset_is_snapshot(later) &&
	    earlier_txg >= later->ds_phys->ds_creation_txg)
		return (B_FALSE);

	if (later->ds_dir == earlier->ds_dir)
		return (B_TRUE);
	if (!dsl_dir_is_clone(later->ds_dir))
		return (B_FALSE);

	if (later->ds_dir->dd_phys->dd_origin_obj == earlier->ds_object)
		return (B_TRUE);
	dsl_dataset_t *origin;
	error = dsl_dataset_hold_obj(dp,
	    later->ds_dir->dd_phys->dd_origin_obj, FTAG, &origin);
	if (error != 0)
		return (B_FALSE);
	ret = dsl_dataset_is_before(origin, earlier, earlier_txg);
	dsl_dataset_rele(origin, FTAG);
	return (ret);
}


void
dsl_dataset_zapify(dsl_dataset_t *ds, dmu_tx_t *tx)
{
	objset_t *mos = ds->ds_dir->dd_pool->dp_meta_objset;
	dmu_object_zapify(mos, ds->ds_object, DMU_OT_DSL_DATASET, tx);
}
