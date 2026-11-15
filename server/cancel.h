/*
 * cancel.h -- knowing when nobody is waiting for the answer any more.
 *
 * A client calls devctl() and blocks. If it is killed, or hits its own
 * timeout, or its process dies, QNX tells us: the channel was created with
 * _NTO_CHF_UNBLOCK, so the kernel sends an unblock pulse and the resource
 * manager calls our io_unblock handler.
 *
 * Without that handler a dead client's request keeps running. The pool
 * thread burns a full tier of CPU producing an answer that will be thrown
 * away -- and it burns it at the dead client's inherited priority, so a
 * cancelled priority-40 request goes on preempting live priority-10 work
 * for an answer nobody will ever read. On a loaded single-core system
 * that is a real way to miss real deadlines.
 *
 * So: io_devctl registers the rcvid before computing, the chunk loop asks
 * between chunks whether the client has given up, and io_unblock marks the
 * slot when the kernel says so. Chunking already exists for deadline
 * re-evaluation; this is the same decision point used for a second purpose.
 */
#ifndef TRIAGE_CANCEL_H_
#define TRIAGE_CANCEL_H_

#include <stdint.h>

/* Register an in-flight request. Returns a slot handle, or -1 if the table
 * is full -- in which case the request still runs, just without being
 * cancellable, which is the safe direction to fail in. */
int  cancel_register(int rcvid);

/* Release a slot once the request is finished, cancelled or not. */
void cancel_unregister(int slot);

/* Has the client for this slot given up? Called between chunks, so it is
 * on the measurement path and stays a mutex and one read. */
int  cancel_requested(int slot);

/* The kernel says this client is unblocking. Returns 1 if we were holding
 * that rcvid and have now marked it cancelled, 0 if we did not know it. */
int  cancel_mark(int rcvid);

/* How many requests have been abandoned by their clients. For the stats,
 * because a number that stays zero is evidence the handler is wired up and
 * a number that climbs is evidence clients really do walk away. */
uint64_t cancel_total(void);

#endif /* TRIAGE_CANCEL_H_ */
