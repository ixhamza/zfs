// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
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
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */
/*
 * Copyright (c) 2012 by Delphix. All rights reserved.
 */

#include <sys/rrwlock.h>
#include <sys/trace_zfs.h>

/*
 * This file contains the implementation of a re-entrant read
 * reader/writer lock (aka "rrwlock").
 *
 * This is a normal reader/writer lock with the additional feature
 * of allowing threads who have already obtained a read lock to
 * re-enter another read lock (re-entrant read) - even if there are
 * waiting writers.
 *
 * Callers who have not obtained a read lock give waiting writers priority.
 *
 * The rrwlock_t lock does not allow re-entrant writers, nor does it
 * allow a re-entrant mix of reads and writes (that is, it does not
 * allow a caller who has already obtained a read lock to be able to
 * then grab a write lock without first dropping all read locks, and
 * vice versa).
 *
 * The rrwlock_t uses tsd (thread specific data) to keep a list of
 * nodes (rrw_node_t), where each node keeps track of which specific
 * lock (rrw_node_t::rn_lock) the thread has grabbed.  Since re-entering
 * should be rare, a thread that grabs multiple reads on the same rrwlock_t
 * will store multiple rrw_node_ts of the same 'rn_lock'. Nodes on the
 * tsd list can represent a different rrwlock_t.  This allows a thread
 * to enter multiple and unique rrwlock_ts for read locks at the same time.
 *
 * Since using tsd exposes some overhead, the rrwlock_t only needs to
 * keep tsd data when writers are waiting.  If no writers are waiting, then
 * a reader just bumps the anonymous read count (rr_anon_rcount) - no tsd
 * is needed.  Once a writer attempts to grab the lock, readers then
 * keep tsd data and bump the linked readers count (rr_linked_rcount).
 *
 * If there are waiting writers and there are anonymous readers, then a
 * reader doesn't know if it is a re-entrant lock. But since it may be one,
 * we allow the read to proceed (otherwise it could deadlock).  Since once
 * waiting writers are active, readers no longer bump the anonymous count,
 * the anonymous readers will eventually flush themselves out.  At this point,
 * readers will be able to tell if they are a re-entrant lock (have a
 * rrw_node_t entry for the lock) or not. If they are a re-entrant lock, then
 * we must let the proceed.  If they are not, then the reader blocks for the
 * waiting writers.  Hence, we do not starve writers.
 */

/* global key for TSD */
uint_t rrw_tsd_key;

/*
 * rn_lock is the lock the thread holds for read: an rrwlock_t, or the
 * rrm_stripe_t of an rrmlock_t.
 */
typedef struct rrw_node {
	struct rrw_node *rn_next;
	const void *rn_lock;
	const void *rn_tag;
} rrw_node_t;

static rrw_node_t *
rrn_find_lock(const void *lock)
{
	rrw_node_t *rn;

	for (rn = tsd_get(rrw_tsd_key); rn != NULL; rn = rn->rn_next) {
		if (rn->rn_lock == lock)
			return (rn);
	}
	return (NULL);
}

static rrw_node_t *
rrn_find(rrwlock_t *rrl)
{
	if (zfs_refcount_count(&rrl->rr_linked_rcount) == 0)
		return (NULL);

	return (rrn_find_lock(rrl));
}

/*
 * Add a node to the head of the singly linked list.
 */
static void
rrn_add(const void *lock, const void *tag)
{
	rrw_node_t *rn;

	rn = kmem_alloc(sizeof (*rn), KM_SLEEP);
	rn->rn_lock = lock;
	rn->rn_next = tsd_get(rrw_tsd_key);
	rn->rn_tag = tag;
	VERIFY0(tsd_set(rrw_tsd_key, rn));
}

/*
 * If a node is found for 'lock', then remove the node from this
 * thread's list and return TRUE; otherwise return FALSE.
 */
static boolean_t
rrn_find_and_remove_lock(const void *lock, const void *tag)
{
	rrw_node_t *rn;
	rrw_node_t *prev = NULL;

	for (rn = tsd_get(rrw_tsd_key); rn != NULL; rn = rn->rn_next) {
		if (rn->rn_lock == lock && rn->rn_tag == tag) {
			if (prev)
				prev->rn_next = rn->rn_next;
			else
				VERIFY0(tsd_set(rrw_tsd_key, rn->rn_next));
			kmem_free(rn, sizeof (*rn));
			return (B_TRUE);
		}
		prev = rn;
	}
	return (B_FALSE);
}

static boolean_t
rrn_find_and_remove(rrwlock_t *rrl, const void *tag)
{
	if (zfs_refcount_count(&rrl->rr_linked_rcount) == 0)
		return (B_FALSE);

	return (rrn_find_and_remove_lock(rrl, tag));
}

void
rrw_init(rrwlock_t *rrl, boolean_t track_all)
{
	mutex_init(&rrl->rr_lock, NULL, MUTEX_DEFAULT, NULL);
	cv_init(&rrl->rr_cv, NULL, CV_DEFAULT, NULL);
	rrl->rr_writer = NULL;
	zfs_refcount_create(&rrl->rr_anon_rcount);
	zfs_refcount_create(&rrl->rr_linked_rcount);
	rrl->rr_writer_wanted = B_FALSE;
	rrl->rr_track_all = track_all;
}

void
rrw_destroy(rrwlock_t *rrl)
{
	mutex_destroy(&rrl->rr_lock);
	cv_destroy(&rrl->rr_cv);
	ASSERT0P(rrl->rr_writer);
	zfs_refcount_destroy(&rrl->rr_anon_rcount);
	zfs_refcount_destroy(&rrl->rr_linked_rcount);
}

static void
rrw_enter_read_impl(rrwlock_t *rrl, boolean_t prio, const void *tag)
{
	mutex_enter(&rrl->rr_lock);
#if !defined(ZFS_DEBUG) && defined(_KERNEL)
	if (rrl->rr_writer == NULL && !rrl->rr_writer_wanted &&
	    !rrl->rr_track_all) {
		rrl->rr_anon_rcount.rc_count++;
		mutex_exit(&rrl->rr_lock);
		return;
	}
	DTRACE_PROBE(zfs__rrwfastpath__rdmiss);
#endif
	ASSERT(rrl->rr_writer != curthread);
	ASSERT(zfs_refcount_count(&rrl->rr_anon_rcount) >= 0);

	while (rrl->rr_writer != NULL || (rrl->rr_writer_wanted &&
	    zfs_refcount_is_zero(&rrl->rr_anon_rcount) && !prio &&
	    rrn_find(rrl) == NULL))
		cv_wait(&rrl->rr_cv, &rrl->rr_lock);

	if (rrl->rr_writer_wanted || rrl->rr_track_all) {
		/* may or may not be a re-entrant enter */
		rrn_add(rrl, tag);
		(void) zfs_refcount_add(&rrl->rr_linked_rcount, tag);
	} else {
		(void) zfs_refcount_add(&rrl->rr_anon_rcount, tag);
	}
	ASSERT0P(rrl->rr_writer);
	mutex_exit(&rrl->rr_lock);
}

void
rrw_enter_read(rrwlock_t *rrl, const void *tag)
{
	rrw_enter_read_impl(rrl, B_FALSE, tag);
}

/*
 * take a read lock even if there are pending write lock requests. if we want
 * to take a lock reentrantly, but from different threads (that have a
 * relationship to each other), the normal detection mechanism to overrule
 * the pending writer does not work, so we have to give an explicit hint here.
 */
void
rrw_enter_read_prio(rrwlock_t *rrl, const void *tag)
{
	rrw_enter_read_impl(rrl, B_TRUE, tag);
}


void
rrw_enter_write(rrwlock_t *rrl)
{
	mutex_enter(&rrl->rr_lock);
	ASSERT(rrl->rr_writer != curthread);

	while (zfs_refcount_count(&rrl->rr_anon_rcount) > 0 ||
	    zfs_refcount_count(&rrl->rr_linked_rcount) > 0 ||
	    rrl->rr_writer != NULL) {
		rrl->rr_writer_wanted = B_TRUE;
		cv_wait(&rrl->rr_cv, &rrl->rr_lock);
	}
	rrl->rr_writer_wanted = B_FALSE;
	rrl->rr_writer = curthread;
	mutex_exit(&rrl->rr_lock);
}

void
rrw_enter(rrwlock_t *rrl, krw_t rw, const void *tag)
{
	if (rw == RW_READER)
		rrw_enter_read(rrl, tag);
	else
		rrw_enter_write(rrl);
}

void
rrw_exit(rrwlock_t *rrl, const void *tag)
{
	mutex_enter(&rrl->rr_lock);
#if !defined(ZFS_DEBUG) && defined(_KERNEL)
	if (!rrl->rr_writer && rrl->rr_linked_rcount.rc_count == 0) {
		rrl->rr_anon_rcount.rc_count--;
		if (rrl->rr_anon_rcount.rc_count == 0)
			cv_broadcast(&rrl->rr_cv);
		mutex_exit(&rrl->rr_lock);
		return;
	}
	DTRACE_PROBE(zfs__rrwfastpath__exitmiss);
#endif
	ASSERT(!zfs_refcount_is_zero(&rrl->rr_anon_rcount) ||
	    !zfs_refcount_is_zero(&rrl->rr_linked_rcount) ||
	    rrl->rr_writer != NULL);

	if (rrl->rr_writer == NULL) {
		int64_t count;
		if (rrn_find_and_remove(rrl, tag)) {
			count = zfs_refcount_remove(
			    &rrl->rr_linked_rcount, tag);
		} else {
			ASSERT(!rrl->rr_track_all);
			count = zfs_refcount_remove(&rrl->rr_anon_rcount, tag);
		}
		if (count == 0)
			cv_broadcast(&rrl->rr_cv);
	} else {
		ASSERT(rrl->rr_writer == curthread);
		ASSERT(zfs_refcount_is_zero(&rrl->rr_anon_rcount) &&
		    zfs_refcount_is_zero(&rrl->rr_linked_rcount));
		rrl->rr_writer = NULL;
		cv_broadcast(&rrl->rr_cv);
	}
	mutex_exit(&rrl->rr_lock);
}

/*
 * If the lock was created with track_all, rrw_held(RW_READER) will return
 * B_TRUE iff the current thread has the lock for reader.  Otherwise it may
 * return B_TRUE if any thread has the lock for reader.
 */
boolean_t
rrw_held(rrwlock_t *rrl, krw_t rw)
{
	boolean_t held;

	mutex_enter(&rrl->rr_lock);
	if (rw == RW_WRITER) {
		held = (rrl->rr_writer == curthread);
	} else {
		held = (!zfs_refcount_is_zero(&rrl->rr_anon_rcount) ||
		    rrn_find(rrl) != NULL);
	}
	mutex_exit(&rrl->rr_lock);

	return (held);
}

void
rrw_tsd_destroy(void *arg)
{
	rrw_node_t *rn = arg;
	if (rn != NULL) {
		panic("thread %p terminating with rrw lock %p held",
		    (void *)curthread, rn->rn_lock);
	}
}

/*
 * A reader-mostly lock implementation, tuning above reader-writer locks
 * for hightly parallel read acquisitions, while pessimizing writes.
 *
 * The idea is to split single busy lock into stripes, so that each reader
 * uses only one of them, depending on result of simple hash function.
 * Writer at the same time has to sequentially acquire all the stripes.
 * That makes write acquisition proportionally slower, but in places where
 * it is used (filesystem unmount, suspend for rollback and receive)
 * performance is not critical.
 *
 * Each stripe sits on cache lines of its own and keeps the rules of an
 * rrwlock_t for its readers, without its mutex on the fast paths:
 *
 * - While a stripe is RRM_OPEN, a reader enters with an atomic increment
 *   of rs_readers and leaves with an atomic decrement.  These anonymous
 *   readers of different stripes share nothing.
 * - A writer sets a stripe RRM_WANTED and waits for it to drain.  New
 *   readers then stop counting themselves in rs_readers, and one that
 *   raced the writer takes its increment back at once, so but for that
 *   moment the count of anonymous readers only shrinks, as rr_anon_rcount
 *   does.  A new reader is let in only if running anonymous readers
 *   remain, because it may be one of them re-entering, or if it already
 *   holds the stripe as a linked reader.  It then becomes a linked reader
 *   itself, counted in rs_linked and recorded in its tsd.  Any other
 *   reader waits, so the writer is not starved.  An anonymous reader
 *   leaves with a decrement that is its last access to the lock, as a
 *   writer that sees the stripe drained may go on to free the lock, so
 *   the writer polls for anonymous readers to leave.  Linked readers, and
 *   readers that raced it, leave under rm_lock and wake it.
 * - Once rs_readers and rs_linked are both zero, the writer sets the
 *   stripe RRM_WRITER and moves on to the next one.
 * - The writer leaving counts the readers that wait for a stripe into its
 *   rs_readers before opening it, so they get in ahead of the next writer,
 *   which waits for them to run.  Until each is back from its wait it is
 *   also counted in rs_granted, so that it does not let others in as a
 *   running reader would.  An rrwlock_t lets a writer that is already
 *   queued go first instead, so back-to-back writers can keep its waiting
 *   readers out.
 *
 * Writers change rs_state and rs_gen, and writers and readers change
 * rs_linked, rs_waiters and rs_granted, only under rm_lock.  Readers leave the
 * fast paths for rm_lock, so the slow paths are serialized against the
 * writer.
 *
 * The fast enter is a store-load handshake: a reader updates rs_readers
 * and then reads rs_state, and a writer updates rs_state and then reads the
 * counts, so either the writer sees the reader or the reader sees the
 * writer.  A reader that finds rs_state RRM_OPEN reads it again after its
 * increment, and membar_consumer() then makes the stores of the writer that
 * opened the stripe visible to it, while the branch on that load keeps its
 * own stores after it, as with smp_acquire__after_ctrl_dep().  The atomics
 * on both sides are full barriers, so they also keep a reader's accesses
 * before its decrement, and a writer's between its draining the stripes
 * and opening them again.
 *
 * As with the rrwlock_t stripes this replaces, the lock must be released
 * by the same thread that acquired it, writers do not re-enter, and a
 * writer may not take the lock for read.
 */

/*
 * The value-returning atomics and atomic_swap_32() are full barriers in the
 * Linux kernel (Documentation/atomic_t.txt), but not necessarily elsewhere,
 * where RRM_MEMBAR() makes them so on the sides the lock relies on.
 */
#if defined(_KERNEL) && defined(__linux__)
#define	RRM_MEMBAR()	((void) 0)
#else
#define	RRM_MEMBAR()	membar_sync()
#endif

/*
 * Debug builds keep the tags of the anonymous readers, so that a release
 * with the wrong tag is caught where it happens when reference tracking is
 * enabled, as with an rrwlock_t.
 */
#ifdef	ZFS_DEBUG
#define	RRM_TAG_ADD(rs, tag)	\
	(void) zfs_refcount_add(&(rs)->rs_anon_tags, (tag))
#define	RRM_TAG_REMOVE(rs, tag)	\
	(void) zfs_refcount_remove(&(rs)->rs_anon_tags, (tag))
#else
#define	RRM_TAG_ADD(rs, tag)	((void) (rs), (void) (tag))
#define	RRM_TAG_REMOVE(rs, tag)	((void) (rs), (void) (tag))
#endif

void
rrm_init(rrmlock_t *rrl, boolean_t track_all)
{
	VERIFY(!track_all);

	mutex_init(&rrl->rm_lock, NULL, MUTEX_DEFAULT, NULL);
	cv_init(&rrl->rm_rcv, NULL, CV_DEFAULT, NULL);
	cv_init(&rrl->rm_wcv, NULL, CV_DEFAULT, NULL);
	rrl->rm_writer = NULL;
	for (int i = 0; i < RRM_NUM_LOCKS; i++) {
		rrm_stripe_t *rs = &rrl->rm_stripes[i];

		rs->rs_readers = 0;
		rs->rs_linked = 0;
		rs->rs_state = RRM_OPEN;
		rs->rs_waiters = 0;
		rs->rs_granted = 0;
		rs->rs_gen = 0;
#ifdef	ZFS_DEBUG
		zfs_refcount_create(&rs->rs_anon_tags);
#endif
	}
}

/*
 * No thread may be inside rrm_enter() or rrm_exit() here.  As with the
 * rrwlock_t stripes, a reader is done with the lock once its decrement on
 * the fast path, or its mutex_exit() on the slow path, lets a writer see
 * the stripe drained, so a reader that a writer waited for no longer uses
 * the lock when the writer frees it.  A writer leaving wakes its waiters
 * after dropping rm_lock, so the lock may be freed only once that writer's
 * rrm_exit() has returned.
 */
void
rrm_destroy(rrmlock_t *rrl)
{
	ASSERT0P(rrl->rm_writer);
	for (int i = 0; i < RRM_NUM_LOCKS; i++) {
		rrm_stripe_t *rs = &rrl->rm_stripes[i];

		ASSERT0(rs->rs_readers);
		ASSERT0(rs->rs_linked);
		ASSERT0(rs->rs_waiters);
		ASSERT0(rs->rs_granted);
		ASSERT3U(rs->rs_state, ==, RRM_OPEN);
#ifdef	ZFS_DEBUG
		zfs_refcount_destroy(&rs->rs_anon_tags);
#endif
	}
	mutex_destroy(&rrl->rm_lock);
	cv_destroy(&rrl->rm_rcv);
	cv_destroy(&rrl->rm_wcv);
}

void
rrm_enter(rrmlock_t *rrl, krw_t rw, const void *tag)
{
	if (rw == RW_READER)
		rrm_enter_read(rrl, tag);
	else
		rrm_enter_write(rrl);
}

/*
 * This maps the current thread to a specific lock.  Note that the lock
 * must be released by the same thread that acquired it.  We do this
 * mapping by taking the thread pointer mod a prime number.  We examine
 * only the low 32 bits of the thread pointer, because 32-bit division
 * is faster than 64-bit division, and the high 32 bits have little
 * entropy anyway.
 */
#define	RRM_TD_LOCK()	(((uint32_t)(uintptr_t)(curthread)) % RRM_NUM_LOCKS)

/*
 * Anonymous readers do not wake a writer that waits for them, so it polls
 * the stripe, at first after RRM_POLL_MIN and then twice as long each time,
 * up to RRM_POLL_MAX.
 */
#define	RRM_POLL_MIN	USEC2NSEC(10)
#define	RRM_POLL_MAX	MSEC2NSEC(1)

/*
 * A writer may be waiting for this stripe to drain; wake it if it has.
 */
static void
rrm_wake_drained(rrmlock_t *rrl, rrm_stripe_t *rs)
{
	ASSERT(MUTEX_HELD(&rrl->rm_lock));
	if (atomic_load_32(&rs->rs_state) == RRM_WANTED &&
	    atomic_load_64(&rs->rs_readers) == 0 && rs->rs_linked == 0)
		cv_broadcast(&rrl->rm_wcv);
}

/*
 * Called with rm_lock held for a reader that did not get in on the fast
 * path and is not counted in rs_readers.
 */
static void
rrm_enter_read_slow(rrmlock_t *rrl, rrm_stripe_t *rs, const void *tag)
{
	uint32_t state = atomic_load_32(&rs->rs_state);
	uint32_t gen;

	ASSERT(MUTEX_HELD(&rrl->rm_lock));
	ASSERT3P(rrl->rm_writer, !=, curthread);

	if (state == RRM_OPEN) {
		(void) atomic_inc_64_nv(&rs->rs_readers);
	} else if (state == RRM_WANTED &&
	    (atomic_load_64(&rs->rs_readers) > rs->rs_granted ||
	    (rs->rs_linked != 0 && rrn_find_lock(rs) != NULL))) {
		/* Let it in as a linked reader. */
		rrn_add(rs, tag);
		atomic_inc_64(&rs->rs_linked);
		return;
	} else {
		/*
		 * Wait for the writer to leave.  It counts us in rs_readers
		 * and rs_granted when it opens the stripe, which it marks with
		 * rs_gen.
		 */
		gen = rs->rs_gen;
		rs->rs_waiters++;
		do {
			cv_wait(&rrl->rm_rcv, &rrl->rm_lock);
		} while (rs->rs_gen == gen);
		ASSERT3U(rs->rs_granted, >, 0);
		rs->rs_granted--;
	}
	RRM_TAG_ADD(rs, tag);
}

void
rrm_enter_read(rrmlock_t *rrl, const void *tag)
{
	rrm_stripe_t *rs = &rrl->rm_stripes[RRM_TD_LOCK()];
	boolean_t raced = B_FALSE;

	if (likely(atomic_load_32(&rs->rs_state) == RRM_OPEN)) {
		(void) atomic_inc_64_nv(&rs->rs_readers);
		RRM_MEMBAR();
		if (likely(atomic_load_32(&rs->rs_state) == RRM_OPEN)) {
			membar_consumer();
			RRM_TAG_ADD(rs, tag);
			return;
		}
		/*
		 * It raced a writer that took the stripe, RRM_WANTED or
		 * RRM_WRITER.  Take the increment back at once, as other
		 * readers would take it for an anonymous reader, and wake the
		 * writer if it was the last.
		 */
		atomic_dec_64(&rs->rs_readers);
		raced = B_TRUE;
	}
	DTRACE_PROBE(zfs__rrwfastpath__rdmiss);
	mutex_enter(&rrl->rm_lock);
	if (raced)
		rrm_wake_drained(rrl, rs);
	rrm_enter_read_slow(rrl, rs, tag);
	ASSERT3U(atomic_load_32(&rs->rs_state), !=, RRM_WRITER);
	mutex_exit(&rrl->rm_lock);
}

void
rrm_enter_write(rrmlock_t *rrl)
{
	mutex_enter(&rrl->rm_lock);
	ASSERT3P(rrl->rm_writer, !=, curthread);
	ASSERT3P(rrn_find_lock(&rrl->rm_stripes[RRM_TD_LOCK()]), ==, NULL);
	while (rrl->rm_writer != NULL)
		cv_wait(&rrl->rm_rcv, &rrl->rm_lock);
	rrl->rm_writer = curthread;
	for (int i = 0; i < RRM_NUM_LOCKS; i++) {
		rrm_stripe_t *rs = &rrl->rm_stripes[i];
		hrtime_t poll = RRM_POLL_MIN;

		ASSERT3U(rs->rs_state, ==, RRM_OPEN);
		(void) atomic_swap_32(&rs->rs_state, RRM_WANTED);
		RRM_MEMBAR();
		while (atomic_load_64(&rs->rs_readers) != 0 ||
		    rs->rs_linked != 0) {
			(void) cv_timedwait_hires(&rrl->rm_wcv, &rrl->rm_lock,
			    poll, poll / 4, 0);
			poll = MIN(poll * 2, RRM_POLL_MAX);
		}
		(void) atomic_swap_32(&rs->rs_state, RRM_WRITER);
		RRM_MEMBAR();
	}
	mutex_exit(&rrl->rm_lock);
}

static void
rrm_exit_write(rrmlock_t *rrl)
{
	mutex_enter(&rrl->rm_lock);
	ASSERT3P(rrl->rm_writer, ==, curthread);
	for (int i = 0; i < RRM_NUM_LOCKS; i++) {
		rrm_stripe_t *rs = &rrl->rm_stripes[i];

		ASSERT3U(rs->rs_state, ==, RRM_WRITER);
		ASSERT0(rs->rs_linked);
		if (rs->rs_waiters != 0) {
			atomic_add_64(&rs->rs_readers, rs->rs_waiters);
			rs->rs_granted += rs->rs_waiters;
			rs->rs_waiters = 0;
			rs->rs_gen++;
		}
		/* Makes our stores visible to the readers it lets in. */
		RRM_MEMBAR();
		(void) atomic_swap_32(&rs->rs_state, RRM_OPEN);
	}
	rrl->rm_writer = NULL;
	mutex_exit(&rrl->rm_lock);
	/*
	 * Wake the readers and writers waiting for us only now, so that they
	 * do not pile up on rm_lock while we still hold it.
	 */
	cv_broadcast(&rrl->rm_rcv);
}

void
rrm_exit(rrmlock_t *rrl, const void *tag)
{
	rrm_stripe_t *rs = &rrl->rm_stripes[RRM_TD_LOCK()];

	/*
	 * Readers never find the stripe they hold RRM_WRITER, as the writer
	 * sets that only once the stripe has no readers.
	 */
	if (unlikely(atomic_load_32(&rs->rs_state) == RRM_WRITER)) {
		ASSERT3P(rrl->rm_writer, ==, curthread);
		rrm_exit_write(rrl);
		return;
	}
	/*
	 * A linked reader always sees its own count in rs_linked, so zero
	 * means this thread holds the stripe anonymously, also while a writer
	 * waits for it.
	 */
	if (likely(atomic_load_64(&rs->rs_linked) == 0)) {
		uint64_t readers;

		RRM_TAG_REMOVE(rs, tag);
		RRM_MEMBAR();
		/*
		 * Our last access to the lock: a writer waiting for the stripe
		 * to drain polls for it, and may free the lock once it has.
		 */
		readers = atomic_dec_64_nv(&rs->rs_readers);
		ASSERT3U(readers, !=, UINT64_MAX);
		return;
	}
	DTRACE_PROBE(zfs__rrwfastpath__exitmiss);
	mutex_enter(&rrl->rm_lock);
	if (rs->rs_linked != 0 && rrn_find_and_remove_lock(rs, tag)) {
		atomic_dec_64(&rs->rs_linked);
	} else {
		RRM_TAG_REMOVE(rs, tag);
		VERIFY3U(atomic_dec_64_nv(&rs->rs_readers), !=, UINT64_MAX);
	}
	rrm_wake_drained(rrl, rs);
	mutex_exit(&rrl->rm_lock);
}

/*
 * rrm_held(RW_READER) returns B_TRUE if any thread holds the current
 * thread's stripe anonymously or this thread holds it as a linked reader,
 * like rrw_held() for an rrwlock_t without track_all.  A reader racing a
 * writer counts for a moment, so it may also be B_TRUE for the writer.
 */
boolean_t
rrm_held(rrmlock_t *rrl, krw_t rw)
{
	rrm_stripe_t *rs;

	if (rw == RW_WRITER) {
		rs = &rrl->rm_stripes[RRM_NUM_LOCKS - 1];
		return (rrl->rm_writer == curthread &&
		    atomic_load_32(&rs->rs_state) == RRM_WRITER);
	}

	rs = &rrl->rm_stripes[RRM_TD_LOCK()];
	return (atomic_load_64(&rs->rs_readers) != 0 ||
	    (atomic_load_64(&rs->rs_linked) != 0 &&
	    rrn_find_lock(rs) != NULL));
}
