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
 * Copyright (c) 1995, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2014 Nexenta Systems, Inc.  All rights reserved.
 */

#include <sys/param.h>
#include <sys/errno.h>
#include <sys/vfs.h>
#include <sys/vnode.h>
#include <sys/cred.h>
#include <sys/cmn_err.h>
#include <sys/systm.h>
#include <sys/kmem.h>
#include <sys/pathname.h>
#include <sys/utsname.h>
#include <sys/debug.h>
#include <sys/door.h>
#include <sys/sdt.h>
#include <sys/thread.h>

#include <rpc/types.h>
#include <rpc/auth.h>
#include <rpc/clnt.h>

#include <nfs/nfs.h>
#include <nfs/export.h>
#include <nfs/nfs_clnt.h>
#include <nfs/auth.h>

#define	EQADDR(a1, a2)  \
	(bcmp((char *)(a1)->buf, (char *)(a2)->buf, (a1)->len) == 0 && \
	(a1)->len == (a2)->len)

static struct knetconfig auth_knconf;
static servinfo_t svp;
static clinfo_t ci;

static struct kmem_cache *exi_cache_handle;
static void exi_cache_reclaim(void *);
static void exi_cache_trim(struct exportinfo *exi);

extern pri_t minclsyspri;

int nfsauth_cache_hit;
int nfsauth_cache_miss;
int nfsauth_cache_refresh;
int nfsauth_cache_reclaim;

/*
 * The lifetime of an auth cache entry:
 * ------------------------------------
 *
 * An auth cache entry is created with both the auth_time
 * and auth_freshness times set to the current time.
 *
 * Upon every client access which results in a hit, the
 * auth_time will be updated.
 *
 * If a client access determines that the auth_freshness
 * indicates that the entry is STALE, then it will be
 * refreshed. Note that this will explicitly reset
 * auth_time.
 *
 * When the REFRESH successfully occurs, then the
 * auth_freshness is updated.
 *
 * There are two ways for an entry to leave the cache:
 *
 * 1) Purged by an action on the export (remove or changed)
 * 2) Memory backpressure from the kernel (check against NFSAUTH_CACHE_TRIM)
 *
 * For 2) we check the timeout value against auth_time.
 */

/*
 * Number of seconds until we mark for refresh an auth cache entry.
 */
#define	NFSAUTH_CACHE_REFRESH 600

/*
 * Number of idle seconds until we yield to backpressure
 * to trim a cache entry.
 */
#define	NFSAUTH_CACHE_TRIM 3600

/*
 * While we could encapuslate the exi_list inside the
 * exi structure, we can't do that for the auth_list.
 * So, to keep things looking clean, we keep them both
 * in these external lists.
 */
typedef struct refreshq_exi_node {
	struct exportinfo	*ren_exi;
	list_t			ren_authlist;
	list_node_t		ren_node;
} refreshq_exi_node_t;

typedef struct refreshq_auth_node {
	struct auth_cache	*ran_auth;
	list_node_t		ran_node;
} refreshq_auth_node_t;

/*
 * Used to manipulate things on the refreshq_queue.
 * Note that the refresh thread will effectively
 * pop a node off of the queue, at which point it
 * will no longer need to hold the mutex.
 */
static kmutex_t refreshq_lock;
static list_t refreshq_queue;
static kcondvar_t refreshq_cv;

/*
 * A list_t would be overkill. These are auth_cache
 * entries which are no longer linked to an exi.
 * It should be the case that all of their states
 * are NFS_AUTH_INVALID.
 *
 * I.e., the only way to be put on this list is
 * iff their state indicated that they had been placed
 * on the refreshq_queue.
 *
 * Note that while there is no link from the exi or
 * back to the exi, the exi can not go away until
 * these entries are harvested.
 */
static struct auth_cache	*refreshq_dead_entries;

/*
 * If there is ever a problem with loading the
 * module, then nfsauth_fini() needs to be called
 * to remove state. In that event, since the
 * refreshq thread has been started, they need to
 * work together to get rid of state.
 */
typedef enum nfsauth_refreshq_thread_state {
	REFRESHQ_THREAD_RUNNING,
	REFRESHQ_THREAD_FINI_REQ,
	REFRESHQ_THREAD_HALTED
} nfsauth_refreshq_thread_state_t;

nfsauth_refreshq_thread_state_t
refreshq_thread_state = REFRESHQ_THREAD_HALTED;

static void nfsauth_free_node(struct auth_cache *);
static void nfsauth_remove_dead_entry(struct auth_cache *);
static void nfsauth_refresh_thread(void);

/*
 * mountd is a server-side only daemon. This will need to be
 * revisited if the NFS server is ever made zones-aware.
 */
kmutex_t	mountd_lock;
door_handle_t   mountd_dh;

void
mountd_args(uint_t did)
{
	mutex_enter(&mountd_lock);
	if (mountd_dh)
		door_ki_rele(mountd_dh);
	mountd_dh = door_ki_lookup(did);
	mutex_exit(&mountd_lock);
}

void
nfsauth_init(void)
{
	/*
	 * mountd can be restarted by smf(5). We need to make sure
	 * the updated door handle will safely make it to mountd_dh
	 */
	mutex_init(&mountd_lock, NULL, MUTEX_DEFAULT, NULL);

	mutex_init(&refreshq_lock, NULL, MUTEX_DEFAULT, NULL);
	list_create(&refreshq_queue, sizeof (refreshq_exi_node_t),
	    offsetof(refreshq_exi_node_t, ren_node));
	refreshq_dead_entries = NULL;

	cv_init(&refreshq_cv, NULL, CV_DEFAULT, NULL);

	/*
	 * Allocate nfsauth cache handle
	 */
	exi_cache_handle = kmem_cache_create("exi_cache_handle",
	    sizeof (struct auth_cache), 0, NULL, NULL,
	    exi_cache_reclaim, NULL, NULL, 0);

	refreshq_thread_state = REFRESHQ_THREAD_RUNNING;
	(void) zthread_create(NULL, 0, nfsauth_refresh_thread,
	    NULL, 0, minclsyspri);
}

/*
 * Finalization routine for nfsauth. It is important to call this routine
 * before destroying the exported_lock.
 */
void
nfsauth_fini(void)
{
	refreshq_exi_node_t	*ren;
	refreshq_auth_node_t	*ran;
	struct auth_cache	*p;
	struct auth_cache	*auth_next;

	/*
	 * Prevent the refreshq_thread from getting new
	 * work.
	 */
	mutex_enter(&refreshq_lock);
	if (refreshq_thread_state != REFRESHQ_THREAD_HALTED) {
		refreshq_thread_state = REFRESHQ_THREAD_FINI_REQ;
		cv_broadcast(&refreshq_cv);

		/*
		 * Also, wait for nfsauth_refresh_thread() to exit.
		 */
		while (refreshq_thread_state != REFRESHQ_THREAD_HALTED) {
			cv_wait(&refreshq_cv, &refreshq_lock);
		}
	}

	/*
	 * Walk the exi_list and in turn, walk the
	 * auth_lists.
	 */
	while ((ren = list_remove_head(&refreshq_queue))) {
		while ((ran = list_remove_head(&ren->ren_authlist))) {
			kmem_free(ran, sizeof (refreshq_auth_node_t));
		}

		list_destroy(&ren->ren_authlist);
		exi_rele(ren->ren_exi);
		kmem_free(ren, sizeof (refreshq_exi_node_t));
	}

	/*
	 * Okay, now that the lists are deleted, we
	 * need to see if there are any dead entries
	 * to harvest.
	 */
	for (p = refreshq_dead_entries; p != NULL; p = auth_next) {
		auth_next = p->auth_next;
		nfsauth_free_node(p);
	}

	mutex_exit(&refreshq_lock);

	list_destroy(&refreshq_queue);

	cv_destroy(&refreshq_cv);
	mutex_destroy(&refreshq_lock);

	mutex_destroy(&mountd_lock);

	/*
	 * Deallocate nfsauth cache handle
	 */
	kmem_cache_destroy(exi_cache_handle);
}

/*
 * Convert the address in a netbuf to
 * a hash index for the auth_cache table.
 */
static int
hash(struct netbuf *a)
{
	int i, h = 0;

	for (i = 0; i < a->len; i++)
		h ^= a->buf[i];

	return (h & (AUTH_TABLESIZE - 1));
}

/*
 * Mask out the components of an
 * address that do not identify
 * a host. For socket addresses the
 * masking gets rid of the port number.
 */
static void
addrmask(struct netbuf *addr, struct netbuf *mask)
{
	int i;

	for (i = 0; i < addr->len; i++)
		addr->buf[i] &= mask->buf[i];
}

/*
 * nfsauth4_access is used for NFS V4 auth checking. Besides doing
 * the common nfsauth_access(), it will check if the client can
 * have a limited access to this vnode even if the security flavor
 * used does not meet the policy.
 */
int
nfsauth4_access(struct exportinfo *exi, vnode_t *vp, struct svc_req *req)
{
	int access;

	access = nfsauth_access(exi, req);

	/*
	 * There are cases that the server needs to allow the client
	 * to have a limited view.
	 *
	 * e.g.
	 * /export is shared as "sec=sys,rw=dfs-test-4,sec=krb5,rw"
	 * /export/home is shared as "sec=sys,rw"
	 *
	 * When the client mounts /export with sec=sys, the client
	 * would get a limited view with RO access on /export to see
	 * "home" only because the client is allowed to access
	 * /export/home with auth_sys.
	 */
	if (access & NFSAUTH_DENIED || access & NFSAUTH_WRONGSEC) {
		/*
		 * Allow ro permission with LIMITED view if there is a
		 * sub-dir exported under vp.
		 */
		if (has_visible(exi, vp))
			return (NFSAUTH_LIMITED);
	}

	return (access);
}

static void
sys_log(const char *msg)
{
	static time_t	tstamp = 0;
	time_t		now;

	/*
	 * msg is shown (at most) once per minute
	 */
	now = gethrestime_sec();
	if ((tstamp + 60) < now) {
		tstamp = now;
		cmn_err(CE_WARN, msg);
	}
}

/*
 * Callup to the mountd to get access information in the kernel.
 */
static bool_t
nfsauth_retrieve(struct exportinfo *exi, char *req_netid, int flavor,
    struct netbuf *addr, int *access)
{
	varg_t			  varg = {0};
	nfsauth_res_t		  res = {0};
	XDR			  xdrs_a;
	XDR			  xdrs_r;
	size_t			  absz;
	caddr_t			  abuf;
	size_t			  rbsz = (size_t)(BYTES_PER_XDR_UNIT * 2);
	char			  result[BYTES_PER_XDR_UNIT * 2] = {0};
	caddr_t			  rbuf = (caddr_t)&result;
	int			  last = 0;
	door_arg_t		  da;
	door_info_t		  di;
	door_handle_t		  dh;
	uint_t			  ntries = 0;

	/*
	 * No entry in the cache for this client/flavor
	 * so we need to call the nfsauth service in the
	 * mount daemon.
	 */
retry:
	mutex_enter(&mountd_lock);
	dh = mountd_dh;
	if (dh)
		door_ki_hold(dh);
	mutex_exit(&mountd_lock);

	if (dh == NULL) {
		/*
		 * The rendezvous point has not been established yet !
		 * This could mean that either mountd(1m) has not yet
		 * been started or that _this_ routine nuked the door
		 * handle after receiving an EINTR for a REVOKED door.
		 *
		 * Returning NFSAUTH_DROP will cause the NFS client
		 * to retransmit the request, so let's try to be more
		 * rescillient and attempt for ntries before we bail.
		 */
		if (++ntries % NFSAUTH_DR_TRYCNT) {
			delay(hz);
			goto retry;
		}

		sys_log("nfsauth: mountd has not established door");
		*access = NFSAUTH_DROP;
		return (FALSE);
	}

	ntries = 0;
	varg.vers = V_PROTO;
	varg.arg_u.arg.cmd = NFSAUTH_ACCESS;
	varg.arg_u.arg.areq.req_client.n_len = addr->len;
	varg.arg_u.arg.areq.req_client.n_bytes = addr->buf;
	varg.arg_u.arg.areq.req_netid = req_netid;
	varg.arg_u.arg.areq.req_path = exi->exi_export.ex_path;
	varg.arg_u.arg.areq.req_flavor = flavor;

	/*
	 * Setup the XDR stream for encoding the arguments. Notice that
	 * in addition to the args having variable fields (req_netid and
	 * req_path), the argument data structure is itself versioned,
	 * so we need to make sure we can size the arguments buffer
	 * appropriately to encode all the args. If we can't get sizing
	 * info _or_ properly encode the arguments, there's really no
	 * point in continuting, so we fail the request.
	 */
	DTRACE_PROBE1(nfsserv__func__nfsauth__varg, varg_t *, &varg);
	if ((absz = xdr_sizeof(xdr_varg, (void *)&varg)) == 0) {
		door_ki_rele(dh);
		*access = NFSAUTH_DENIED;
		return (FALSE);
	}

	abuf = (caddr_t)kmem_alloc(absz, KM_SLEEP);
	xdrmem_create(&xdrs_a, abuf, absz, XDR_ENCODE);
	if (!xdr_varg(&xdrs_a, &varg)) {
		door_ki_rele(dh);
		goto fail;
	}
	XDR_DESTROY(&xdrs_a);

	/*
	 * The result (nfsauth_res_t) is always two int's, so we don't
	 * have to dynamically size (or allocate) the results buffer.
	 * Now that we've got what we need, we prep the door arguments
	 * and place the call.
	 */
	da.data_ptr = (char *)abuf;
	da.data_size = absz;
	da.desc_ptr = NULL;
	da.desc_num = 0;
	da.rbuf = (char *)rbuf;
	da.rsize = rbsz;

	switch (door_ki_upcall_limited(dh, &da, NULL, SIZE_MAX, 0)) {
		case 0:				/* Success */
			if (da.data_ptr != da.rbuf && da.data_size == 0) {
				/*
				 * The door_return that contained the data
				 * failed ! We're here because of the 2nd
				 * door_return (w/o data) such that we can
				 * get control of the thread (and exit
				 * gracefully).
				 */
				DTRACE_PROBE1(nfsserv__func__nfsauth__door__nil,
				    door_arg_t *, &da);
				door_ki_rele(dh);
				goto fail;

			} else if (rbuf != da.rbuf) {
				/*
				 * The only time this should be true
				 * is iff userland wanted to hand us
				 * a bigger response than what we
				 * expect; that should not happen
				 * (nfsauth_res_t is only 2 int's),
				 * but we check nevertheless.
				 */
				rbuf = da.rbuf;
				rbsz = da.rsize;

			} else if (rbsz > da.data_size) {
				/*
				 * We were expecting two int's; but if
				 * userland fails in encoding the XDR
				 * stream, we detect that here, since
				 * the mountd forces down only one byte
				 * in such scenario.
				 */
				door_ki_rele(dh);
				goto fail;
			}
			door_ki_rele(dh);
			break;

		case EAGAIN:
			/*
			 * Server out of resources; back off for a bit
			 */
			door_ki_rele(dh);
			kmem_free(abuf, absz);
			delay(hz);
			goto retry;
			/* NOTREACHED */

		case EINTR:
			if (!door_ki_info(dh, &di)) {
				if (di.di_attributes & DOOR_REVOKED) {
					/*
					 * The server barfed and revoked
					 * the (existing) door on us; we
					 * want to wait to give smf(5) a
					 * chance to restart mountd(1m)
					 * and establish a new door handle.
					 */
					mutex_enter(&mountd_lock);
					if (dh == mountd_dh)
						mountd_dh = NULL;
					mutex_exit(&mountd_lock);
					door_ki_rele(dh);
					kmem_free(abuf, absz);
					delay(hz);
					goto retry;
				}
				/*
				 * If the door was _not_ revoked on us,
				 * then more than likely we took an INTR,
				 * so we need to fail the operation.
				 */
				door_ki_rele(dh);
				goto fail;
			}
			/*
			 * The only failure that can occur from getting
			 * the door info is EINVAL, so we let the code
			 * below handle it.
			 */
			/* FALLTHROUGH */

		case EBADF:
		case EINVAL:
		default:
			/*
			 * If we have a stale door handle, give smf a last
			 * chance to start it by sleeping for a little bit.
			 * If we're still hosed, we'll fail the call.
			 *
			 * Since we're going to reacquire the door handle
			 * upon the retry, we opt to sleep for a bit and
			 * _not_ to clear mountd_dh. If mountd restarted
			 * and was able to set mountd_dh, we should see
			 * the new instance; if not, we won't get caught
			 * up in the retry/DELAY loop.
			 */
			door_ki_rele(dh);
			if (!last) {
				delay(hz);
				last++;
				goto retry;
			}
			sys_log("nfsauth: stale mountd door handle");
			goto fail;
	}

	/*
	 * No door errors encountered; setup the XDR stream for decoding
	 * the results. If we fail to decode the results, we've got no
	 * other recourse than to fail the request.
	 */
	xdrmem_create(&xdrs_r, rbuf, rbsz, XDR_DECODE);
	if (!xdr_nfsauth_res(&xdrs_r, &res))
		goto fail;
	XDR_DESTROY(&xdrs_r);

	DTRACE_PROBE1(nfsserv__func__nfsauth__results, nfsauth_res_t *, &res);
	switch (res.stat) {
		case NFSAUTH_DR_OKAY:
			*access = res.ares.auth_perm;
			kmem_free(abuf, absz);
			break;

		case NFSAUTH_DR_EFAIL:
		case NFSAUTH_DR_DECERR:
		case NFSAUTH_DR_BADCMD:
		default:
fail:
			*access = NFSAUTH_DENIED;
			kmem_free(abuf, absz);
			return (FALSE);
			/* NOTREACHED */
	}

	return (TRUE);
}

static void
nfsauth_refresh_thread(void)
{
	refreshq_exi_node_t	*ren;
	refreshq_auth_node_t	*ran;

	struct exportinfo	*exi;

	int			access;
	bool_t			retrieval;

	callb_cpr_t		cprinfo;

	CALLB_CPR_INIT(&cprinfo, &refreshq_lock, callb_generic_cpr,
	    "nfsauth_refresh");

	for (;;) {
		mutex_enter(&refreshq_lock);
		if (refreshq_thread_state != REFRESHQ_THREAD_RUNNING) {
			/* Keep the hold on the lock! */
			break;
		}

		ren = list_remove_head(&refreshq_queue);
		if (ren == NULL) {
			CALLB_CPR_SAFE_BEGIN(&cprinfo);
			cv_wait(&refreshq_cv, &refreshq_lock);
			CALLB_CPR_SAFE_END(&cprinfo, &refreshq_lock);
			mutex_exit(&refreshq_lock);
			continue;
		}
		mutex_exit(&refreshq_lock);

		exi = ren->ren_exi;
		ASSERT(exi != NULL);

		/*
		 * Since the ren was removed from the refreshq_queue above,
		 * this is the only thread aware about the ren existence, so we
		 * have the exclusive ownership of it and we do not need to
		 * protect it by any lock.
		 */
		while ((ran = list_remove_head(&ren->ren_authlist))) {

			struct auth_cache *p = ran->ran_auth;

			ASSERT(p != NULL);
			kmem_free(ran, sizeof (refreshq_auth_node_t));

			/*
			 * We are shutting down. No need to refresh
			 * entries which are about to be nuked.
			 *
			 * So just throw them away until we are done
			 * with this exi node...
			 */
			if (refreshq_thread_state != REFRESHQ_THREAD_RUNNING)
				continue;

			mutex_enter(&p->auth_lock);

			/*
			 * Make sure the state is valid now that
			 * we have the lock. Note that once we
			 * change the state to NFS_AUTH_REFRESHING,
			 * no other thread will be able to work on
			 * this entry.
			 */
			if (p->auth_state != NFS_AUTH_STALE) {
				/*
				 * Once it goes INVALID, it can not
				 * change state.
				 */
				if (p->auth_state == NFS_AUTH_INVALID) {
					mutex_exit(&p->auth_lock);
					nfsauth_remove_dead_entry(p);
				} else
					mutex_exit(&p->auth_lock);

				continue;
			}

			p->auth_state = NFS_AUTH_REFRESHING;
			mutex_exit(&p->auth_lock);

			DTRACE_PROBE2(nfsauth__debug__cache__refresh,
			    struct exportinfo *, exi,
			    struct auth_cache *, p);

			/*
			 * The first caching of the access rights
			 * is done with the netid pulled out of the
			 * request from the client. All subsequent
			 * users of the cache may or may not have
			 * the same netid. It doesn't matter. So
			 * when we refresh, we simply use the netid
			 * of the request which triggered the
			 * refresh attempt.
			 */
			ASSERT(p->auth_netid != NULL);

			retrieval = nfsauth_retrieve(exi, p->auth_netid,
			    p->auth_flavor, &p->auth_addr, &access);

			/*
			 * This can only be set in one other place
			 * and the state has to be NFS_AUTH_FRESH.
			 */
			kmem_free(p->auth_netid, strlen(p->auth_netid) + 1);
			p->auth_netid = NULL;

			mutex_enter(&p->auth_lock);
			if (p->auth_state == NFS_AUTH_INVALID) {
				mutex_exit(&p->auth_lock);
				nfsauth_remove_dead_entry(p);
			} else {
				/*
				 * If we got an error, do not reset the
				 * time. This will cause the next access
				 * check for the client to reschedule this
				 * node.
				 */
				if (retrieval == TRUE) {
					p->auth_access = access;
					p->auth_freshness = gethrestime_sec();
				}
				p->auth_state = NFS_AUTH_FRESH;
				mutex_exit(&p->auth_lock);
			}
		}

		list_destroy(&ren->ren_authlist);
		exi_rele(ren->ren_exi);
		kmem_free(ren, sizeof (refreshq_exi_node_t));
	}

	refreshq_thread_state = REFRESHQ_THREAD_HALTED;
	cv_broadcast(&refreshq_cv);
	CALLB_CPR_EXIT(&cprinfo);
	zthread_exit();
}

/*
 * Get the access information from the cache or callup to the mountd
 * to get and cache the access information in the kernel.
 */
int
nfsauth_cache_get(struct exportinfo *exi, struct svc_req *req, int flavor)
{
	struct netbuf		*taddrmask;
	struct netbuf		addr;
	struct netbuf		*claddr;
	struct auth_cache	**head;
	struct auth_cache	*p;
	int			access;
	time_t			refresh;

	refreshq_exi_node_t	*ren;
	refreshq_auth_node_t	*ran;

	/*
	 * Now check whether this client already
	 * has an entry for this flavor in the cache
	 * for this export.
	 * Get the caller's address, mask off the
	 * parts of the address that do not identify
	 * the host (port number, etc), and then hash
	 * it to find the chain of cache entries.
	 */

	claddr = svc_getrpccaller(req->rq_xprt);
	addr = *claddr;
	addr.buf = kmem_alloc(addr.len, KM_SLEEP);
	bcopy(claddr->buf, addr.buf, claddr->len);
	SVC_GETADDRMASK(req->rq_xprt, SVC_TATTR_ADDRMASK, (void **)&taddrmask);
	ASSERT(taddrmask != NULL);
	if (taddrmask)
		addrmask(&addr, taddrmask);

	rw_enter(&exi->exi_cache_lock, RW_READER);
	head = &exi->exi_cache[hash(&addr)];
	for (p = *head; p; p = p->auth_next) {
		if (EQADDR(&addr, &p->auth_addr) && flavor == p->auth_flavor)
			break;
	}

	if (p != NULL) {
		nfsauth_cache_hit++;

		refresh = gethrestime_sec() - p->auth_freshness;
		DTRACE_PROBE2(nfsauth__debug__cache__hit,
		    int, nfsauth_cache_hit,
		    time_t, refresh);

		mutex_enter(&p->auth_lock);
		if ((refresh > NFSAUTH_CACHE_REFRESH) &&
		    p->auth_state == NFS_AUTH_FRESH) {
			p->auth_state = NFS_AUTH_STALE;
			mutex_exit(&p->auth_lock);

			ASSERT(p->auth_netid == NULL);
			p->auth_netid =
			    strdup(svc_getnetid(req->rq_xprt));

			nfsauth_cache_refresh++;

			DTRACE_PROBE3(nfsauth__debug__cache__stale,
			    struct exportinfo *, exi,
			    struct auth_cache *, p,
			    int, nfsauth_cache_refresh);

			ran = kmem_alloc(sizeof (refreshq_auth_node_t),
			    KM_SLEEP);
			ran->ran_auth = p;

			mutex_enter(&refreshq_lock);
			/*
			 * We should not add a work queue
			 * item if the thread is not
			 * accepting them.
			 */
			if (refreshq_thread_state == REFRESHQ_THREAD_RUNNING) {
				/*
				 * Is there an existing exi_list?
				 */
				for (ren = list_head(&refreshq_queue);
				    ren != NULL;
				    ren = list_next(&refreshq_queue, ren)) {
					if (ren->ren_exi == exi) {
						list_insert_tail(
						    &ren->ren_authlist, ran);
						break;
					}
				}

				if (ren == NULL) {
					ren = kmem_alloc(
					    sizeof (refreshq_exi_node_t),
					    KM_SLEEP);

					exi_hold(exi);
					ren->ren_exi = exi;

					list_create(&ren->ren_authlist,
					    sizeof (refreshq_auth_node_t),
					    offsetof(refreshq_auth_node_t,
					    ran_node));

					list_insert_tail(&ren->ren_authlist,
					    ran);
					list_insert_tail(&refreshq_queue, ren);
				}

				cv_broadcast(&refreshq_cv);
			} else {
				kmem_free(ran, sizeof (refreshq_auth_node_t));
			}

			mutex_exit(&refreshq_lock);
		} else {
			mutex_exit(&p->auth_lock);
		}

		access = p->auth_access;
		p->auth_time = gethrestime_sec();

		rw_exit(&exi->exi_cache_lock);
		kmem_free(addr.buf, addr.len);

		return (access);
	}

	rw_exit(&exi->exi_cache_lock);

	nfsauth_cache_miss++;

	if (!nfsauth_retrieve(exi, svc_getnetid(req->rq_xprt), flavor,
	    &addr, &access)) {
		kmem_free(addr.buf, addr.len);
		return (access);
	}

	/*
	 * Now cache the result on the cache chain
	 * for this export (if there's enough memory)
	 */
	p = kmem_cache_alloc(exi_cache_handle, KM_NOSLEEP);
	if (p != NULL) {
		p->auth_addr = addr;
		p->auth_flavor = flavor;
		p->auth_access = access;
		p->auth_time = p->auth_freshness = gethrestime_sec();
		p->auth_state = NFS_AUTH_FRESH;
		p->auth_netid = NULL;
		mutex_init(&p->auth_lock, NULL, MUTEX_DEFAULT, NULL);

		rw_enter(&exi->exi_cache_lock, RW_WRITER);
		p->auth_next = *head;
		*head = p;
		rw_exit(&exi->exi_cache_lock);
	} else {
		kmem_free(addr.buf, addr.len);
	}

	return (access);
}

/*
 * Check if the requesting client has access to the filesystem with
 * a given nfs flavor number which is an explicitly shared flavor.
 */
int
nfsauth4_secinfo_access(struct exportinfo *exi, struct svc_req *req,
			int flavor, int perm)
{
	int access;

	if (! (perm & M_4SEC_EXPORTED)) {
		return (NFSAUTH_DENIED);
	}

	/*
	 * Optimize if there are no lists
	 */
	if ((perm & (M_ROOT|M_NONE)) == 0) {
		perm &= ~M_4SEC_EXPORTED;
		if (perm == M_RO)
			return (NFSAUTH_RO);
		if (perm == M_RW)
			return (NFSAUTH_RW);
	}

	access = nfsauth_cache_get(exi, req, flavor);

	return (access);
}

int
nfsauth_access(struct exportinfo *exi, struct svc_req *req)
{
	int access, mapaccess;
	struct secinfo *sp;
	int i, flavor, perm;
	int authnone_entry = -1;

	/*
	 *  Get the nfs flavor number from xprt.
	 */
	flavor = (int)(uintptr_t)req->rq_xprt->xp_cookie;

	/*
	 * First check the access restrictions on the filesystem.  If
	 * there are no lists associated with this flavor then there's no
	 * need to make an expensive call to the nfsauth service or to
	 * cache anything.
	 */

	sp = exi->exi_export.ex_secinfo;
	for (i = 0; i < exi->exi_export.ex_seccnt; i++) {
		if (flavor != sp[i].s_secinfo.sc_nfsnum) {
			if (sp[i].s_secinfo.sc_nfsnum == AUTH_NONE)
				authnone_entry = i;
			continue;
		}
		break;
	}

	mapaccess = 0;

	if (i >= exi->exi_export.ex_seccnt) {
		/*
		 * Flavor not found, but use AUTH_NONE if it exists
		 */
		if (authnone_entry == -1)
			return (NFSAUTH_DENIED);
		flavor = AUTH_NONE;
		mapaccess = NFSAUTH_MAPNONE;
		i = authnone_entry;
	}

	/*
	 * If the flavor is in the ex_secinfo list, but not an explicitly
	 * shared flavor by the user, it is a result of the nfsv4 server
	 * namespace setup. We will grant an RO permission similar for
	 * a pseudo node except that this node is a shared one.
	 *
	 * e.g. flavor in (flavor) indicates that it is not explictly
	 *	shared by the user:
	 *
	 *		/	(sys, krb5)
	 *		|
	 *		export  #share -o sec=sys (krb5)
	 *		|
	 *		secure  #share -o sec=krb5
	 *
	 *	In this case, when a krb5 request coming in to access
	 *	/export, RO permission is granted.
	 */
	if (!(sp[i].s_flags & M_4SEC_EXPORTED))
		return (mapaccess | NFSAUTH_RO);

	/*
	 * Optimize if there are no lists
	 */
	perm = sp[i].s_flags;
	if ((perm & (M_ROOT|M_NONE)) == 0) {
		perm &= ~M_4SEC_EXPORTED;
		if (perm == M_RO)
			return (mapaccess | NFSAUTH_RO);
		if (perm == M_RW)
			return (mapaccess | NFSAUTH_RW);
	}

	access = nfsauth_cache_get(exi, req, flavor);

	/*
	 * Client's security flavor doesn't match with "ro" or
	 * "rw" list. Try again using AUTH_NONE if present.
	 */
	if ((access & NFSAUTH_WRONGSEC) && (flavor != AUTH_NONE)) {
		/*
		 * Have we already encountered AUTH_NONE ?
		 */
		if (authnone_entry != -1) {
			mapaccess = NFSAUTH_MAPNONE;
			access = nfsauth_cache_get(exi, req, AUTH_NONE);
		} else {
			/*
			 * Check for AUTH_NONE presence.
			 */
			for (; i < exi->exi_export.ex_seccnt; i++) {
				if (sp[i].s_secinfo.sc_nfsnum == AUTH_NONE) {
					mapaccess = NFSAUTH_MAPNONE;
					access = nfsauth_cache_get(exi, req,
					    AUTH_NONE);
					break;
				}
			}
		}
	}

	if (access & NFSAUTH_DENIED)
		access = NFSAUTH_DENIED;

	return (access | mapaccess);
}

static void
nfsauth_free_node(struct auth_cache *p)
{
	if (p->auth_netid != NULL)
		kmem_free(p->auth_netid, strlen(p->auth_netid) + 1);
	kmem_free(p->auth_addr.buf, p->auth_addr.len);
	mutex_destroy(&p->auth_lock);
	kmem_cache_free(exi_cache_handle, (void *)p);
}

/*
 * Remove the dead entry from the refreshq_dead_entries
 * list.
 */
static void
nfsauth_remove_dead_entry(struct auth_cache *dead)
{
	struct auth_cache	*p;
	struct auth_cache	*prev;
	struct auth_cache	*next;

	mutex_enter(&refreshq_lock);
	prev = NULL;
	for (p = refreshq_dead_entries; p != NULL; p = next) {
		next = p->auth_next;

		if (p == dead) {
			if (prev == NULL)
				refreshq_dead_entries = next;
			else
				prev->auth_next = next;

			nfsauth_free_node(dead);
			break;
		}

		prev = p;
	}
	mutex_exit(&refreshq_lock);
}

/*
 * Free the nfsauth cache for a given export
 */
void
nfsauth_cache_free(struct exportinfo *exi)
{
	int i;
	struct auth_cache *p, *next;

	for (i = 0; i < AUTH_TABLESIZE; i++) {
		for (p = exi->exi_cache[i]; p; p = next) {
			next = p->auth_next;

			/*
			 * The only way we got here
			 * was with an exi_rele, which
			 * means that no auth cache entry
			 * is being refreshed.
			 */
			nfsauth_free_node(p);
		}
	}
}

/*
 * Called by the kernel memory allocator when
 * memory is low. Free unused cache entries.
 * If that's not enough, the VM system will
 * call again for some more.
 */
/*ARGSUSED*/
void
exi_cache_reclaim(void *cdrarg)
{
	int i;
	struct exportinfo *exi;

	rw_enter(&exported_lock, RW_READER);

	for (i = 0; i < EXPTABLESIZE; i++) {
		for (exi = exptable[i]; exi; exi = exi->fid_hash.next) {
			exi_cache_trim(exi);
		}
	}
	nfsauth_cache_reclaim++;

	rw_exit(&exported_lock);
}

void
exi_cache_trim(struct exportinfo *exi)
{
	struct auth_cache *p;
	struct auth_cache *prev, *next;
	int i;
	time_t stale_time;

	stale_time = gethrestime_sec() - NFSAUTH_CACHE_TRIM;

	rw_enter(&exi->exi_cache_lock, RW_WRITER);

	for (i = 0; i < AUTH_TABLESIZE; i++) {

		/*
		 * Free entries that have not been
		 * used for NFSAUTH_CACHE_TRIM seconds.
		 */
		prev = NULL;
		for (p = exi->exi_cache[i]; p; p = next) {
			next = p->auth_next;
			if (p->auth_time > stale_time) {
				prev = p;
				continue;
			}

			mutex_enter(&p->auth_lock);
			DTRACE_PROBE1(nfsauth__debug__trim__state,
			    auth_state_t, p->auth_state);

			if (p->auth_state != NFS_AUTH_FRESH) {
				p->auth_state = NFS_AUTH_INVALID;
				mutex_exit(&p->auth_lock);

				mutex_enter(&refreshq_lock);
				p->auth_next = refreshq_dead_entries;
				refreshq_dead_entries = p;
				mutex_exit(&refreshq_lock);
			} else {
				mutex_exit(&p->auth_lock);
				nfsauth_free_node(p);
			}

			if (prev == NULL)
				exi->exi_cache[i] = next;
			else
				prev->auth_next = next;
		}
	}

	rw_exit(&exi->exi_cache_lock);
}
