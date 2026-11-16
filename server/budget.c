#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>

#include "budget.h"

static int      enabled;
static uint32_t cfg_budget_us = 3000;    /* 3ms  */
static uint32_t cfg_period_us = 10000;   /* 10ms -> a 30% ceiling */
static int      cfg_low_prio  = 1;       /* just above idle */
static int      cfg_threshold = 15;      /* the chat band and below */

static pthread_mutex_t stat_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t        applied_total;
static uint64_t        failed_total;

/*
 * Saved per thread, because pool threads are shared. Each one has to put
 * back exactly the policy and priority it had, and "exactly" includes the
 * priority it inherited from this request's client -- which is not a
 * constant and is not ours to guess.
 */
static __thread int                saved_valid;
static __thread int                saved_policy;
static __thread struct sched_param saved_param;

void budget_configure(uint32_t budget_us, uint32_t period_us,
                      int low_prio, int threshold)
{
    if (budget_us > 0)
        cfg_budget_us = budget_us;
    if (period_us > 0)
        cfg_period_us = period_us;
    cfg_low_prio  = low_prio;
    cfg_threshold = threshold;

    /* A budget that is not smaller than its period is not a ceiling: the
     * thread never exhausts it and never drops. Refuse rather than ship a
     * cap that silently does nothing. */
    if (cfg_budget_us >= cfg_period_us) {
        fprintf(stderr,
                "triage: budget %uus >= period %uus is not a ceiling; "
                "budget disabled\n", cfg_budget_us, cfg_period_us);
        enabled = 0;
        return;
    }
    enabled = 1;
}

int budget_enabled(void)
{
    return enabled;
}

int budget_apply(int client_prio)
{
    struct sched_param p;
    int                policy = 0;

    saved_valid = 0;

    if (!enabled || client_prio > cfg_threshold)
        return 0;

    if (pthread_getschedparam(pthread_self(), &policy, &saved_param) != EOK)
        return 0;
    saved_policy = policy;
    saved_valid  = 1;

    /*
     * Keep sched_priority where it is. The request arrived at the client's
     * priority and should still be served at it -- inheritance is not what
     * we are overriding here. What changes is that once this thread has
     * burned cfg_budget_us of CPU within cfg_period_us, the kernel drops it
     * to cfg_low_prio until the budget replenishes.
     */
    p = saved_param;
    p.sched_ss_low_priority        = cfg_low_prio;
    p.sched_ss_max_repl            = 4;
    p.sched_ss_init_budget.tv_sec  = cfg_budget_us / 1000000u;
    p.sched_ss_init_budget.tv_nsec = (long)(cfg_budget_us % 1000000u) * 1000L;
    p.sched_ss_repl_period.tv_sec  = cfg_period_us / 1000000u;
    p.sched_ss_repl_period.tv_nsec = (long)(cfg_period_us % 1000000u) * 1000L;

    if (pthread_setschedparam(pthread_self(), SCHED_SPORADIC, &p) != EOK) {
        /* Switching policy on a running thread may be refused even though
         * the policy works when set at thread creation. Say so once per
         * occurrence in the counter rather than per request in the log,
         * and carry on uncapped -- an uncapped request is a worse service
         * than we intended, not a broken one. */
        saved_valid = 0;
        pthread_mutex_lock(&stat_lock);
        failed_total++;
        pthread_mutex_unlock(&stat_lock);
        return 0;
    }

    pthread_mutex_lock(&stat_lock);
    applied_total++;
    pthread_mutex_unlock(&stat_lock);
    return 1;
}

void budget_release(void)
{
    if (!saved_valid)
        return;
    pthread_setschedparam(pthread_self(), saved_policy, &saved_param);
    saved_valid = 0;
}

uint64_t budget_applied_total(void)
{
    uint64_t v;

    pthread_mutex_lock(&stat_lock);
    v = applied_total;
    pthread_mutex_unlock(&stat_lock);
    return v;
}

uint64_t budget_failed_total(void)
{
    uint64_t v;

    pthread_mutex_lock(&stat_lock);
    v = failed_total;
    pthread_mutex_unlock(&stat_lock);
    return v;
}

void budget_describe(FILE *out)
{
    if (!enabled) {
        fprintf(out, "triage: CPU ceiling off (enable with -b)\n");
        return;
    }
    fprintf(out,
            "triage: CPU ceiling ON -- requests at prio <= %d get %uus "
            "per %uus (%u%%), dropping to prio %d when spent\n",
            cfg_threshold, cfg_budget_us, cfg_period_us,
            (unsigned)((uint64_t)cfg_budget_us * 100u / cfg_period_us),
            cfg_low_prio);
}
