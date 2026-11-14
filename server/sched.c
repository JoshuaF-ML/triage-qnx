#include <pthread.h>
#include <string.h>

#include "sched.h"
#include "engine.h"

/*
 * Keep a safety margin: a tier is only admitted if its estimated cost
 * fits inside SAFETY_NUM/SAFETY_DEN of the remaining budget. Inference
 * cost is not constant, and a deadline we meet 50% of the time is not a
 * deadline.
 */
#define SAFETY_NUM  4
#define SAFETY_DEN  5   /* 80% */

/* EWMA weight for new observations: est = (3*est + obs) / 4. */
#define EWMA_SHIFT  2

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static triage_stats_t  stats;

void sched_init(void)
{
    triage_task_t t;
    triage_tier_t k;

    pthread_mutex_lock(&lock);
    memset(&stats, 0, sizeof stats);
    stats.min_prio_seen = -1;
    stats.max_prio_seen = -1;
    for (t = 0; t < TRIAGE_TASK__COUNT; t++)
        for (k = 0; k < TRIAGE_TIER__COUNT; k++)
            stats.est_us[t][k] = engine_nominal_us(t, k);
    pthread_mutex_unlock(&lock);
}

uint64_t sched_estimate_us(triage_task_t task, triage_tier_t tier)
{
    uint64_t v;

    if (task >= TRIAGE_TASK__COUNT || tier >= TRIAGE_TIER__COUNT)
        return 0;
    pthread_mutex_lock(&lock);
    v = stats.est_us[task][tier];
    pthread_mutex_unlock(&lock);
    return v;
}

triage_tier_t sched_pick_tier(triage_task_t task, uint64_t budget_us,
                              triage_tier_t want_tier, uint32_t flags,
                              uint32_t *degraded)
{
    triage_tier_t ceiling, k, chosen = TRIAGE_TIER_NONE;
    uint64_t allowed;

    *degraded = 0;

    if (task >= TRIAGE_TASK__COUNT)
        return TRIAGE_TIER_NONE;

    ceiling = (want_tier > TRIAGE_TIER_NONE && want_tier < TRIAGE_TIER__COUNT)
                  ? want_tier : TRIAGE_TIER_LARGE;

    /* No deadline: the caller is not in a hurry, give it what it asked
     * for. Adaptive Partitioning, not us, is what stops this starving
     * anybody -- see docs/architecture.md. */
    if (budget_us == 0)
        return ceiling;

    /* The caller insists on its tier and will accept being late. */
    if (flags & TRIAGE_F_NO_DEGRADE)
        return ceiling;

    allowed = budget_us * SAFETY_NUM / SAFETY_DEN;

    for (k = ceiling; k > TRIAGE_TIER_NONE; k--) {
        if (sched_estimate_us(task, k) <= allowed) {
            chosen = k;
            break;
        }
    }

    if (chosen == TRIAGE_TIER_NONE) {
        /* Nothing fits. Either fail loudly, or do the cheapest thing we
         * can and let the reply admit it will probably be late. */
        if (flags & TRIAGE_F_REQUIRE_TIER)
            return TRIAGE_TIER_NONE;
        chosen = TRIAGE_TIER_SMALL;
    }

    if (chosen < ceiling)
        *degraded = 1;

    return chosen;
}

void sched_record(triage_task_t task, const triage_reply_t *rep)
{
    triage_tier_t tier = (triage_tier_t)rep->tier_used;

    pthread_mutex_lock(&lock);

    stats.requests++;
    if (rep->deadline_met)
        stats.deadlines_met++;
    else
        stats.deadlines_missed++;
    if (rep->degraded)
        stats.degraded++;
    stats.preemptions += rep->preempted;

    if (tier < TRIAGE_TIER__COUNT)
        stats.per_tier[tier]++;
    if (task < TRIAGE_TASK__COUNT)
        stats.per_task[task]++;

    if (rep->total_us > stats.worst_total_us)
        stats.worst_total_us = rep->total_us;
    if (rep->queue_us > stats.worst_queue_us)
        stats.worst_queue_us = rep->queue_us;

    if (stats.min_prio_seen < 0 || rep->client_prio < stats.min_prio_seen)
        stats.min_prio_seen = rep->client_prio;
    if (rep->client_prio > stats.max_prio_seen)
        stats.max_prio_seen = rep->client_prio;

    /*
     * Update the cost model -- but only from requests that ran clean.
     * compute_us is wall clock, so a request that was preempted spent
     * time not running; folding that in would teach us that everything
     * is expensive whenever the machine is busy, and we would degrade
     * for no reason.
     */
    if (task < TRIAGE_TASK__COUNT && tier < TRIAGE_TIER__COUNT
        && rep->preempted == 0 && rep->compute_us > 0) {
        uint64_t est = stats.est_us[task][tier];
        stats.est_us[task][tier] =
            (est * ((1u << EWMA_SHIFT) - 1) + rep->compute_us) >> EWMA_SHIFT;
    }

    pthread_mutex_unlock(&lock);
}

void sched_get_stats(triage_stats_t *out)
{
    pthread_mutex_lock(&lock);
    *out = stats;
    pthread_mutex_unlock(&lock);
}

void sched_reset(void)
{
    sched_init();
}
