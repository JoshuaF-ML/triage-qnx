/*
 * triage_proto.h -- wire protocol between Triage clients and the
 *                   /dev/triage resource manager.
 *
 * Shared by server/ and client/. This header is the ONLY contract between
 * them: everything a client needs to talk to Triage is a devctl() away.
 *
 * Transport: QNX native message passing. A client's devctl() becomes a
 * MsgSend() to the resource manager's channel; the manager's reply becomes
 * the client's return. There is no broker, no socket and no serialisation
 * step -- which is also why the caller's priority can follow the request
 * into the AI work (see docs/architecture.md).
 */
#ifndef TRIAGE_PROTO_H_
#define TRIAGE_PROTO_H_

#include <devctl.h>
#include <stdint.h>

#define TRIAGE_DEV_PATH     "/dev/triage"
#define TRIAGE_ABI_VERSION  1u

/* ------------------------------------------------------------------ */
/* Model tiers                                                         */
/*                                                                     */
/* Triage answers the same question with more or less compute depending */
/* on how much time the caller said it had. A rough answer on time is   */
/* worth more to a brake controller than a perfect answer that is late. */
/* ------------------------------------------------------------------ */
typedef enum {
    TRIAGE_TIER_NONE   = 0,  /* nothing fit the deadline                */
    TRIAGE_TIER_SMALL  = 1,  /* fastest, least accurate                 */
    TRIAGE_TIER_MEDIUM = 2,
    TRIAGE_TIER_LARGE  = 3,  /* slowest, most accurate                  */
    TRIAGE_TIER__COUNT
} triage_tier_t;

/* What kind of question is being asked. Tiers are calibrated per task. */
typedef enum {
    TRIAGE_TASK_DETECT = 0,  /* "is that a pedestrian?" -- vision-ish   */
    TRIAGE_TASK_CHAT   = 1,  /* "what's the weather?"   -- language-ish */
    TRIAGE_TASK__COUNT
} triage_task_t;

#define TRIAGE_PAYLOAD_MAX  256

/* Request flags */
#define TRIAGE_F_REQUIRE_TIER  0x1u  /* fail with ETIME rather than
                                      * silently answer at a lower tier */
#define TRIAGE_F_NO_DEGRADE    0x2u  /* run the requested tier even if
                                      * the estimate says it will miss  */

typedef struct {
    uint32_t abi_version;    /* TRIAGE_ABI_VERSION                      */
    uint32_t task;           /* triage_task_t                          */
    uint32_t deadline_us;    /* 0 == best effort, no deadline           */
    uint32_t want_tier;      /* triage_tier_t, or TRIAGE_TIER_NONE to
                              * let Triage choose from the deadline     */
    uint32_t flags;          /* TRIAGE_F_*                              */
    uint32_t payload_len;    /* bytes used in payload[]                 */
    char     payload[TRIAGE_PAYLOAD_MAX];
} triage_request_t;

typedef struct {
    uint32_t abi_version;
    uint32_t tier_used;      /* triage_tier_t actually run              */
    uint32_t deadline_met;   /* 1 == answered within deadline_us        */
    uint32_t degraded;       /* 1 == we chose a smaller tier to fit     */

    /* Timing, all microseconds, measured on the server.
     *
     * Note what queue_us is NOT: time spent waiting on the channel is
     * invisible to us, because we only start measuring once a pool
     * thread has already received the message. queue_us is our own
     * admission overhead -- validation plus tier selection. A client
     * that wants true end-to-end latency should time its own devctl()
     * call; the difference from total_us is kernel queueing. */
    uint64_t queue_us;       /* received -> first chunk of compute      */
    uint64_t compute_us;     /* first chunk -> last chunk (wall clock)  */
    uint64_t total_us;       /* arrival -> reply                        */

    /* Preemption evidence. compute_us is wall clock, so when a more
     * urgent request takes the CPU mid-inference the gap shows up here. */
    uint32_t chunks;         /* chunks of compute executed              */
    uint32_t preempted;      /* chunk boundaries where wall clock ran
                              * ahead of the work we actually did       */
    uint64_t stolen_us;      /* total wall time lost to preemption      */

    /* Priority evidence -- the heart of the project.
     * client_prio is the priority the caller was running at (read from
     * the message info by the server). server_prio is the priority the
     * server's receive thread was running at while it did this work.
     * On QNX these are equal, with no code of ours to make it so. */
    int32_t  client_prio;
    int32_t  server_prio;

    uint32_t confidence;     /* 0..1000, scaled; higher tier -> higher  */
    uint32_t result_len;
    char     result[TRIAGE_PAYLOAD_MAX];
} triage_reply_t;

/*
 * devctl() uses one buffer for both directions, so it must be large
 * enough for the bigger of the two structs. Clients should always pass a
 * triage_xfer_t, not a bare request.
 */
typedef union {
    triage_request_t req;
    triage_reply_t   rep;
} triage_xfer_t;

/* ------------------------------------------------------------------ */
/* Server statistics (for the dashboard and the benchmark)             */
/* ------------------------------------------------------------------ */
typedef struct {
    uint64_t requests;
    uint64_t deadlines_met;
    uint64_t deadlines_missed;
    uint64_t degraded;
    uint64_t preemptions;
    uint64_t per_tier[TRIAGE_TIER__COUNT];
    uint64_t per_task[TRIAGE_TASK__COUNT];
    uint64_t worst_total_us;
    uint64_t worst_queue_us;
    /* Adaptive cost model: what a tier currently costs, in us.
     * Indexed [task][tier]. Updated by an EWMA as work completes. */
    uint64_t est_us[TRIAGE_TASK__COUNT][TRIAGE_TIER__COUNT];
    int32_t  min_prio_seen;
    int32_t  max_prio_seen;
} triage_stats_t;

/* ------------------------------------------------------------------ */
/* devctl command codes                                                */
/* ------------------------------------------------------------------ */
#define TRIAGE_DCMD_INFER  __DIOTF(_DCMD_MISC, 1, triage_xfer_t)
#define TRIAGE_DCMD_STATS  __DIOF (_DCMD_MISC, 2, triage_stats_t)
#define TRIAGE_DCMD_RESET  __DION (_DCMD_MISC, 3)

#endif /* TRIAGE_PROTO_H_ */
