/*
 * sched.h -- deadline-aware tier selection and statistics.
 *
 * This is the part of Triage that is not just "a priority queue". QNX
 * already gives us priority: the resource manager's receive thread runs
 * at the caller's priority, so urgent requests preempt lazy ones without
 * any scheduling code of ours. What the OS cannot do for us is decide
 * HOW MUCH AI to run. That is this file: given a deadline and a running
 * estimate of what each tier currently costs, pick the largest tier that
 * still fits, and say so in the reply.
 */
#ifndef TRIAGE_SCHED_H_
#define TRIAGE_SCHED_H_

#include <stdint.h>
#include "triage_proto.h"

/* Seeds the cost model from the engine's nominal numbers. */
void sched_init(void);

/*
 * Choose a tier for a request.
 *
 *   task         what is being asked
 *   budget_us    microseconds left until the deadline, 0 == best effort
 *   want_tier    caller's preference, or TRIAGE_TIER_NONE for "you decide"
 *   flags        TRIAGE_F_*
 *   client_prio  the caller's priority, which decides how much
 *                interference this request should expect to suffer
 *   out          filled in with the figures the decision was made on, so
 *                the reply can carry them; may be NULL
 *
 * Returns TRIAGE_TIER_NONE if not even the smallest tier fits and the
 * caller said it would rather fail than be late (TRIAGE_F_REQUIRE_TIER).
 */
typedef struct {
    uint32_t degraded;      /* 1 == picked something smaller than asked */
    uint32_t est_used_us;   /* predicted cost of the chosen tier        */
    uint32_t infl_pct;      /* interference inflation applied, percent  */
    uint32_t prio_band;     /* band whose history the prediction used   */
} sched_decision_t;

triage_tier_t sched_pick_tier(triage_task_t task, uint64_t budget_us,
                              triage_tier_t want_tier, uint32_t flags,
                              int32_t client_prio, sched_decision_t *out);

/* Fold a finished request into the stats and the cost model. */
void sched_record(triage_task_t task, const triage_reply_t *rep);

void sched_get_stats(triage_stats_t *out);
void sched_reset(void);

/* Current CLEAN cost estimate for a tier, in microseconds -- what it
 * costs when nothing competes with it. NOT what it will take under load;
 * for that ask sched_pick_tier, which adds the interference term. */
uint64_t sched_estimate_us(triage_task_t task, triage_tier_t tier);

/* Current interference inflation for a priority band, in percent.
 * 100 == no interference expected. */
uint32_t sched_inflation_pct(int32_t client_prio);

#endif /* TRIAGE_SCHED_H_ */
