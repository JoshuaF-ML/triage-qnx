#include <pthread.h>
#include <string.h>

#include "cancel.h"
#include "tier_sched.h"
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

/*
 * THE INTERFERENCE MODEL
 * ======================
 *
 * The cost model above answers "what does this tier cost". That is not the
 * question a deadline asks. The question a deadline asks is "will this
 * finish in time", and the answer has two terms:
 *
 *      predicted = clean cost  x  expected interference
 *
 * Getting this wrong was measured, not theorised. The first on-target run
 * missed 8 deadlines of 100, and the cause was structural rather than
 * unlucky: sched_record() updates the cost model only from requests that
 * ran clean, which is right for measuring what a tier costs and useless
 * for predicting whether it fits. The stolen time that actually breaks
 * deadlines was exactly the data being excluded. The scheduler priced
 * detect/medium at 13.2 ms, fitted it into a 16 ms budget, and then lost
 * 3-10 ms to interference it had no way to anticipate.
 *
 * So interference is now a second tracked quantity, deliberately NOT
 * folded into the cost estimate. Two numbers, two meanings: est_us stays a
 * clean measurement of model cost, which is what the benchmark and the
 * dashboard want to report, while infl_pct carries the environment, which
 * is what the deadline decision needs. Collapsing them into one would lose
 * both answers.
 *
 * A DECAYING MAXIMUM, NOT A MEAN
 * ------------------------------
 * The mean is the wrong statistic. A deadline is not met on average; it is
 * met or missed on each request, and what makes it miss is a bad moment,
 * not a typical one. Mean interference of 150% with occasional spikes to
 * 400% produces precisely the failure we measured: the model says it fits,
 * and sometimes it does not.
 *
 * So the figure kept is a maximum that decays. It jumps immediately to any
 * worse interference observed and fades back towards 100% as evidence
 * ages: quick to react to a machine getting busy, slow to forget, which is
 * the right asymmetry when being wrong costs a missed deadline rather than
 * a wasted millisecond.
 *
 * This is a heuristic and should be read as one. It bounds nothing -- a
 * real guarantee needs the budget enforcement that has to replace Adaptive
 * Partitioning, which is a different mechanism with different teeth. What
 * it does is stop the scheduler being structurally blind to the one thing
 * that breaks it.
 */
#define INFL_MIN_PCT   100u    /* no interference expected              */
#define INFL_MAX_PCT   1000u   /* cap at 10x: past this, degrade rather
                                * than pretend the estimate means much  */
#define INFL_DECAY_NUM 63u     /* per request recorded in the band,     */
#define INFL_DECAY_DEN 64u     /* keep 63/64 of the old maximum         */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static triage_stats_t  stats;

void sched_init(void)
{
    triage_task_t t;
    triage_tier_t k;
    unsigned      b;

    pthread_mutex_lock(&lock);
    memset(&stats, 0, sizeof stats);
    stats.min_prio_seen = -1;
    stats.max_prio_seen = -1;
    for (t = 0; t < TRIAGE_TASK__COUNT; t++)
        for (k = 0; k < TRIAGE_TIER__COUNT; k++)
            stats.est_us[t][k] = engine_nominal_us(t, k);
    /* Start optimistic: assume no interference until some is observed.
     * The alternative -- starting pessimistic -- would degrade the first
     * requests on an idle machine for no reason, and the figure climbs
     * fast enough that one bad request is all it takes. */
    for (b = 0; b < TRIAGE_PRIO_BANDS; b++)
        stats.infl_pct[b] = INFL_MIN_PCT;
    pthread_mutex_unlock(&lock);
}

uint32_t sched_inflation_pct(int32_t client_prio)
{
    unsigned band = TRIAGE_PRIO_BAND(client_prio);
    uint32_t v;

    pthread_mutex_lock(&lock);
    v = stats.infl_pct[band];
    pthread_mutex_unlock(&lock);
    return v ? v : INFL_MIN_PCT;
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

/* Predicted wall-clock cost of a tier for a request in this band:
 * clean cost scaled by the interference we currently expect. */
static uint64_t predicted_us(triage_task_t task, triage_tier_t tier,
                             uint32_t infl_pct)
{
    return (sched_estimate_us(task, tier) * infl_pct) / 100u;
}

triage_tier_t sched_pick_tier(triage_task_t task, uint64_t budget_us,
                              triage_tier_t want_tier, uint32_t flags,
                              int32_t client_prio, sched_decision_t *out)
{
    triage_tier_t    ceiling, k, chosen = TRIAGE_TIER_NONE;
    uint64_t         allowed;
    uint32_t         infl;
    sched_decision_t d;

    memset(&d, 0, sizeof d);
    d.prio_band = TRIAGE_PRIO_BAND(client_prio);
    d.infl_pct  = INFL_MIN_PCT;

    if (task >= TRIAGE_TASK__COUNT) {
        if (out)
            *out = d;
        return TRIAGE_TIER_NONE;
    }

    ceiling = (want_tier > TRIAGE_TIER_NONE && want_tier < TRIAGE_TIER__COUNT)
                  ? want_tier : TRIAGE_TIER_LARGE;

    /* No deadline: the caller is not in a hurry, so give it what it asked
     * for. Nothing here stops that starving anybody -- that is the budget
     * mechanism's job, and it is still owed. See docs/architecture.md. */
    if (budget_us == 0) {
        d.est_used_us = (uint32_t)sched_estimate_us(task, ceiling);
        if (out)
            *out = d;
        return ceiling;
    }

    /* The caller insists on its tier and will accept being late. */
    if (flags & TRIAGE_F_NO_DEGRADE) {
        d.est_used_us = (uint32_t)sched_estimate_us(task, ceiling);
        if (out)
            *out = d;
        return ceiling;
    }

    infl       = sched_inflation_pct(client_prio);
    d.infl_pct = infl;
    allowed    = budget_us * SAFETY_NUM / SAFETY_DEN;

    for (k = ceiling; k > TRIAGE_TIER_NONE; k--) {
        if (predicted_us(task, k, infl) <= allowed) {
            chosen = k;
            break;
        }
    }

    if (chosen == TRIAGE_TIER_NONE) {
        /* Nothing fits. Either fail loudly, or do the cheapest thing we
         * can and let the reply admit it will probably be late. */
        if (flags & TRIAGE_F_REQUIRE_TIER) {
            if (out)
                *out = d;
            return TRIAGE_TIER_NONE;
        }
        chosen = TRIAGE_TIER_SMALL;
    }

    d.est_used_us = (uint32_t)predicted_us(task, chosen, infl);
    if (chosen < ceiling)
        d.degraded = 1;

    if (out)
        *out = d;
    return chosen;
}

void sched_record(triage_task_t task, const triage_reply_t *rep)
{
    triage_tier_t tier = (triage_tier_t)rep->tier_used;
    unsigned      band = TRIAGE_PRIO_BAND(rep->client_prio);

    pthread_mutex_lock(&lock);

    stats.requests++;
    if (rep->deadline_met)
        stats.deadlines_met++;
    else
        stats.deadlines_missed++;
    if (rep->degraded)
        stats.degraded++;
    stats.preemptions += rep->preempted;
    stats.stolen_us   += rep->stolen_us;
    if (rep->budget_capped)
        stats.budget_capped++;
    stats.band_requests[band]++;

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

    /*
     * Update the interference model -- from EVERY request, including the
     * preempted ones the cost model above must ignore. This is the data
     * that was being thrown away, and it is the data that decides whether
     * a tier fits.
     *
     * The ratio is wall time over time actually spent running:
     *
     *      infl = compute_us / (compute_us - stolen_us)
     *
     * so a request that lost half its wall time to other work reports
     * 200%: "work in this band currently takes about twice as long as it
     * costs". Note that this needs no knowledge of what the tier should
     * have cost -- it is self-relative, and so survives the cost model
     * being wrong, which on this target it measurably is.
     *
     * A clean request reports 100% and therefore only ever pulls the
     * maximum down, via the decay below. That is the intended behaviour:
     * evidence that the machine is quiet is still evidence.
     */
    {
        uint32_t observed = INFL_MIN_PCT;
        uint32_t decayed;

        if (rep->compute_us > 0 && rep->stolen_us < rep->compute_us) {
            uint64_t ran = rep->compute_us - rep->stolen_us;
            uint64_t pct = (rep->compute_us * 100u) / (ran ? ran : 1u);

            if (pct > INFL_MAX_PCT)
                pct = INFL_MAX_PCT;
            if (pct > INFL_MIN_PCT)
                observed = (uint32_t)pct;
        } else if (rep->stolen_us >= rep->compute_us && rep->compute_us > 0) {
            /* Almost all of it was stolen. Saturate rather than divide by
             * something that rounds to nothing. */
            observed = INFL_MAX_PCT;
        }

        decayed = (stats.infl_pct[band] * INFL_DECAY_NUM) / INFL_DECAY_DEN;
        if (decayed < INFL_MIN_PCT)
            decayed = INFL_MIN_PCT;

        stats.infl_pct[band] = observed > decayed ? observed : decayed;
    }

    pthread_mutex_unlock(&lock);
}

void sched_get_stats(triage_stats_t *out)
{
    pthread_mutex_lock(&lock);
    *out = stats;
    pthread_mutex_unlock(&lock);
    /* Cancellations are counted where they happen, in cancel.c, because
     * a cancelled request never reaches sched_record(). */
    out->cancelled = cancel_total();
}

void sched_reset(void)
{
    sched_init();
}
