/*
 * budget.h -- a CPU ceiling for low-priority AI, enforced by the kernel.
 *
 * WHAT THIS IS FOR
 * ----------------
 * Priority inheritance protects the urgent request. It does nothing about
 * the opposite failure: a flood of unimportant inference eating the
 * machine, starving control loops that use no AI at all and never talk to
 * Triage. Priority answers "who goes first". It says nothing about "how
 * much in total". Something else has to cap that.
 *
 * The build plan used QNX Adaptive Partitioning. APS is discontinued in
 * QNX 8.0, so this is the replacement -- POSIX sporadic scheduling, which
 * bench/sched_probe.c verified is not merely declared on this kernel but
 * actually ENFORCED (a thread with a 2ms/10ms budget got 16.5% of the CPU
 * where an unenforced one takes ~100%).
 *
 * A sporadic thread runs at its normal priority until it exhausts a CPU
 * budget, then drops to a low priority until the budget replenishes. So a
 * chat request still arrives at priority 10 and is still served at
 * priority 10 -- inheritance is untouched -- but it cannot consume more
 * than budget/period of the CPU before the kernel demotes it beneath
 * whatever else wants to run.
 *
 * WHY THE KERNEL AND NOT US
 * -------------------------
 * The fallback was a token bucket debited at chunk boundaries in
 * tier_sched.c. That would have worked and been weaker, for a reason worth
 * stating: our own accounting can only decline to START work. The kernel
 * can take the CPU away from work already running. A chunk that overruns
 * is exactly the case a self-imposed budget cannot handle and a sporadic
 * budget handles without our involvement.
 *
 * WHY IT IS OFF BY DEFAULT
 * ------------------------
 * Enabling it changes how much CPU low-priority work gets, which would
 * change the Triage-vs-FCFS comparison into a measurement of two things at
 * once. The benchmark measures scheduling; this is demonstrated on its own.
 * Turn it on with -b.
 */
#ifndef TRIAGE_BUDGET_H_
#define TRIAGE_BUDGET_H_

#include <stdint.h>
#include <stdio.h>

/*
 * Enable the ceiling.
 *
 *   budget_us     CPU a capped request may use per period
 *   period_us     replenishment period
 *   low_prio      priority a request drops to once the budget is spent
 *   threshold     requests at or below this client priority are capped;
 *                 anything above it is left alone, because capping the
 *                 brake controller would defeat the entire project
 */
void budget_configure(uint32_t budget_us, uint32_t period_us,
                      int low_prio, int threshold);

int  budget_enabled(void);

/*
 * Put the calling thread under the ceiling if this client's priority is at
 * or below the threshold. Returns 1 if a cap was applied, 0 if not.
 *
 * Call budget_release() afterwards without fail: pool threads are reused,
 * and a thread left sporadic would cap the next request it serves no
 * matter whose it is.
 */
int  budget_apply(int client_prio);

/* Restore the policy and priority the thread had before budget_apply. */
void budget_release(void);

/* How many requests have run under the ceiling. */
uint64_t budget_applied_total(void);

/* How many times applying it failed, which should be 0 or everything. */
uint64_t budget_failed_total(void);

void budget_describe(FILE *out);

#endif /* TRIAGE_BUDGET_H_ */
