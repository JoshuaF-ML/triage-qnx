/*
 * sched_probe.c -- does this kernel actually enforce SCHED_SPORADIC?
 *
 * WHY THIS EXISTS
 * ---------------
 * Triage needs a CPU budget ceiling: priority inheritance decides who runs
 * first, but it says nothing about how much AI runs in total, so a flood of
 * low-priority inference can still starve control loops that use no AI at
 * all. The original plan used QNX Adaptive Partitioning for that. APS is
 * discontinued in QNX 8.0.
 *
 * POSIX sporadic scheduling is the replacement candidate, and it would be a
 * better answer than anything we could implement: a sporadic thread runs at
 * a high priority until it exhausts a CPU budget, then drops to a low
 * priority until the budget replenishes. That is a ceiling enforced by the
 * kernel rather than by our own good intentions.
 *
 * But the headers are not evidence. SCHED_SPORADIC is defined in
 * <sched.h> on this SDP -- and so is SCHED_EXT_APS, for a scheduler that
 * does not exist any more. Having already been caught once believing a
 * header, this program asks the kernel instead.
 *
 * WHAT IT CHECKS, IN ORDER
 * ------------------------
 *   1. Does pthread_setschedparam accept SCHED_SPORADIC at all?
 *   2. Does pthread_getschedparam report the policy back?
 *   3. Does the budget actually get ENFORCED?
 *
 * Only the third question matters. A kernel can accept the policy, report
 * it faithfully, and schedule the thread as though it were SCHED_RR -- in
 * which case the ceiling is imaginary and Triage must implement its own.
 *
 * The functional test puts a sporadic thread at priority 40 with a 2ms
 * budget per 10ms period and a drop-to priority of 5, against a plain
 * competitor spinning at priority 10, on one CPU. If the budget is
 * enforced the sporadic thread gets roughly 20% of the CPU: it runs at 40
 * for its 2ms, falls to 5 (below the competitor), and waits. If it is not
 * enforced, priority 40 beats priority 10 and the sporadic thread takes
 * essentially all of the CPU.
 *
 * So the measurement separates the two outcomes by a factor of about five.
 * There is no ambiguous middle to argue about.
 *
 *   ./sched_probe            # run the probe
 *   ./sched_probe -v         # show per-thread work counts
 *
 * Exit code 0 means enforced and usable; 1 means not, and Triage should
 * fall back to accounting budgets itself at chunk boundaries.
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/neutrino.h>
#include <sys/syspage.h>

#define SPORADIC_PRIO   40
#define SPORADIC_LOW     5
#define COMPETITOR_PRIO 10
#define BUDGET_MS        2
#define PERIOD_MS       10
#define RUN_MS        1000

static volatile int      stop;
static volatile uint64_t work_sporadic;
static volatile uint64_t work_competitor;
static int               verbose;

/* Dependency-chained arithmetic: no memory traffic, nothing the compiler
 * can elide, cost per unit stable. Same idea as the Triage engine. */
static uint64_t spin(volatile uint64_t *counter)
{
    uint64_t acc = 1;
    uint64_t n   = 0;

    while (!stop) {
        int i;
        for (i = 0; i < 1000; i++)
            acc = acc * 6364136223846793005ull + 1442695040888963407ull;
        n++;
        *counter = n;
    }
    return acc;
}

static void *sporadic_thread(void *arg)
{
    (void)arg;
    spin(&work_sporadic);
    return NULL;
}

static void *competitor_thread(void *arg)
{
    struct sched_param p;
    int                policy = 0;

    (void)arg;

    if (pthread_getschedparam(pthread_self(), &policy, &p) == EOK) {
        p.sched_priority = COMPETITOR_PRIO;
        pthread_setschedparam(pthread_self(), policy, &p);
    }
    spin(&work_competitor);
    return NULL;
}

static void ms_sleep(int ms)
{
    struct timespec ts;

    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv)
{
    pthread_t          sp_tid, comp_tid;
    pthread_attr_t     attr;
    struct sched_param param;
    int                policy = 0, c, rc;
    int                accepted = 0, reported = 0, enforced = 0;
    double             share;

    while ((c = getopt(argc, argv, "vh")) != -1) {
        switch (c) {
        case 'v': verbose = 1; break;
        default:
            fprintf(stderr, "usage: %s [-v]\n", argv[0]);
            return c == 'h' ? 0 : 1;
        }
    }

    printf("sched_probe: does this kernel enforce SCHED_SPORADIC?\n");
    printf("  sporadic thread : prio %d, budget %dms / period %dms, "
           "drops to prio %d\n",
           SPORADIC_PRIO, BUDGET_MS, PERIOD_MS, SPORADIC_LOW);
    printf("  competitor      : prio %d, plain, spinning\n", COMPETITOR_PRIO);
    printf("  duration        : %dms on %d CPU(s)\n\n",
           RUN_MS, (int)_syspage_ptr->num_cpu);

    if (_syspage_ptr->num_cpu > 1)
        printf("  NOTE: more than one CPU online. The share below is not\n"
               "        meaningful unless both threads compete for one.\n\n");

    /* ---- 1. is the policy accepted at all? ---- */
    memset(&param, 0, sizeof param);
    param.sched_priority                 = SPORADIC_PRIO;
    param.sched_ss_low_priority          = SPORADIC_LOW;
    param.sched_ss_max_repl              = 2;
    param.sched_ss_init_budget.tv_sec    = 0;
    param.sched_ss_init_budget.tv_nsec   = BUDGET_MS * 1000000L;
    param.sched_ss_repl_period.tv_sec    = 0;
    param.sched_ss_repl_period.tv_nsec   = PERIOD_MS * 1000000L;

    pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_SPORADIC);
    pthread_attr_setschedparam(&attr, &param);

    rc = pthread_create(&sp_tid, &attr, sporadic_thread, NULL);
    if (rc != EOK) {
        printf("  1. accepted  : NO -- pthread_create with SCHED_SPORADIC: %s\n",
               strerror(rc));
        printf("\nVERDICT: SCHED_SPORADIC is not usable on this kernel.\n");
        printf("Triage must account for budgets itself at chunk boundaries.\n");
        pthread_attr_destroy(&attr);
        return 1;
    }
    accepted = 1;
    printf("  1. accepted  : yes\n");
    pthread_attr_destroy(&attr);

    /* ---- 2. is it reported back? ---- */
    if (pthread_getschedparam(sp_tid, &policy, &param) == EOK) {
        reported = (policy == SCHED_SPORADIC);
        printf("  2. reported  : %s (policy=%d, %s)\n",
               reported ? "yes" : "NO", policy,
               policy == SCHED_SPORADIC ? "SCHED_SPORADIC" :
               policy == SCHED_RR       ? "SCHED_RR" :
               policy == SCHED_FIFO     ? "SCHED_FIFO" : "other");
    } else {
        printf("  2. reported  : could not read it back: %s\n",
               strerror(errno));
    }

    /* ---- 3. is the budget enforced? ---- */
    if (pthread_create(&comp_tid, NULL, competitor_thread, NULL) != EOK) {
        perror("sched_probe: competitor pthread_create");
        stop = 1;
        return 1;
    }

    ms_sleep(RUN_MS);
    stop = 1;
    pthread_join(sp_tid, NULL);
    pthread_join(comp_tid, NULL);

    if (work_sporadic + work_competitor == 0) {
        printf("  3. enforced  : no work recorded; probe inconclusive\n");
        return 1;
    }

    share = (double)work_sporadic /
            (double)(work_sporadic + work_competitor);

    if (verbose)
        printf("     work: sporadic=%llu competitor=%llu\n",
               (unsigned long long)work_sporadic,
               (unsigned long long)work_competitor);

    printf("  3. enforced  : sporadic thread got %.1f%% of the CPU\n",
           share * 100.0);
    printf("     expected  : ~%d%% if the budget is enforced,\n",
           100 * BUDGET_MS / PERIOD_MS);
    printf("                 ~100%% if it is ignored (prio %d beats %d)\n",
           SPORADIC_PRIO, COMPETITOR_PRIO);

    /* Generous band. We are separating ~20% from ~100%, so anything under
     * half is enforcement and anything over is not. The point is to avoid
     * arguing about a boundary that the measurement does not need. */
    enforced = (share < 0.50);

    printf("\nVERDICT: ");
    if (accepted && reported && enforced) {
        printf("SCHED_SPORADIC works and the budget is enforced.\n");
        printf("Triage can use the kernel for its CPU ceiling.\n");
        return 0;
    }
    if (accepted && !enforced) {
        printf("the policy is accepted but the budget is NOT enforced.\n");
        printf("The ceiling would be imaginary. Triage must account for\n");
        printf("budgets itself at chunk boundaries.\n");
        return 1;
    }
    printf("SCHED_SPORADIC is not usable here.\n");
    return 1;
}
