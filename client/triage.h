/*
 * triage.h -- the Triage client library.
 *
 * Asking the AI a question with a deadline should be three lines of code,
 * not a protocol. That is the whole job of this header:
 *
 *     int fd = triage_open(NULL);
 *     triage_reply_t rep;
 *     triage_ask(fd, TRIAGE_TASK_DETECT, "front camera frame", 20000, &rep);
 *
 * The caller's priority is not an argument to any of these functions, and
 * that is deliberate: QNX carries it into the server for us. A thread
 * that runs at priority 40 gets priority-40 AI. If you want urgent AI,
 * be urgent -- triage_set_priority() is here for demos that need to say
 * so explicitly.
 *
 * All functions return 0 on success, or -1 with errno set. errno values
 * come straight from the server:
 *   ETIME    no model tier fitted the deadline and TRIAGE_F_REQUIRE_TIER
 *            was set
 *   ENOTSUP  ABI mismatch between this library and the running server
 *   EINVAL   malformed request
 *   ENOTTY   the server does not know that command
 */
#ifndef TRIAGE_H_
#define TRIAGE_H_

#include "triage_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Open the Triage device. path == NULL means TRIAGE_DEV_PATH.
 * Returns a file descriptor, or -1 with errno set. */
int triage_open(const char *path);

/* Close a descriptor from triage_open(). */
int triage_close(int fd);

/* Full-control call: fill in a triage_request_t yourself. */
int triage_infer(int fd, const triage_request_t *req, triage_reply_t *rep);

/* The common case: ask a question, state how long you can wait, and let
 * Triage choose the model tier. deadline_us == 0 means best effort. */
int triage_ask(int fd, triage_task_t task, const char *prompt,
               uint32_t deadline_us, triage_reply_t *rep);

/* Same, but demand a specific tier. Pass TRIAGE_F_NO_DEGRADE to be
 * answered at that tier even if it will be late, or
 * TRIAGE_F_REQUIRE_TIER to get ETIME instead of a smaller model. */
int triage_ask_tier(int fd, triage_task_t task, const char *prompt,
                    uint32_t deadline_us, triage_tier_t tier,
                    uint32_t flags, triage_reply_t *rep);

/* Server-side counters, for the dashboard and the benchmark. */
int triage_stats(int fd, triage_stats_t *stats);
int triage_reset_stats(int fd);

/* Priority helpers for the calling thread. triage_set_priority() needs
 * the PROCMGR_AID_PRIORITY ability to raise priority above the caller's
 * current one -- on a stock QNX image that means running as root. */
int triage_set_priority(int prio);
int triage_get_priority(void);

const char *triage_tier_name(triage_tier_t tier);
const char *triage_task_name(triage_task_t task);

#ifdef __cplusplus
}
#endif

#endif /* TRIAGE_H_ */
