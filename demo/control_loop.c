/*
 * control_loop.c -- the task that does not use AI at all, and must not be
 *                   starved by the tasks that do.
 *
 * WHY THIS DEMO EXISTS
 * --------------------
 * Priority inheritance protects the urgent AI request. It does nothing for
 * the opposite failure, which is the one pillar 4 of the build plan was
 * for: a greedy AI client eating the machine and starving work that never
 * talks to Triage at all. Housekeeping, health monitoring, a slow
 * actuator trim loop -- useful, periodic, and deliberately run at a LOW
 * priority precisely because each cycle is not individually urgent.
 *
 * That is the shape of the problem. A chat client at priority 10 asking
 * for 300ms of inference, over and over, will starve a priority-5 control
 * loop completely. Priority says the chat request goes first, and priority
 * alone never says "but not for this long".
 *
 * So this program is the victim, and it is not a Triage client. It opens
 * no device and sends no message. It just wakes on a period, does a little
 * work, and measures how late it was. If the AI is eating the machine, its
 * lateness explodes. That is the number the CPU ceiling has to fix.
 *
 * WHAT TO RUN
 * -----------
 *   ./control_loop -P 5 -p 10000 -n 300 &     # the victim
 *   ./chat_load -n 4 -P 10 -s 10              # the greedy AI client
 *
 * with the server started WITHOUT -b (no ceiling), then again WITH it.
 * The ceiling does not make chat slower on an idle machine -- sporadic
 * scheduling demotes a thread past its budget, it does not idle it -- so
 * the thing it changes is exactly this program's lateness.
 *
 * Exit code is 0 if the worst lateness stayed within -j, so it can gate a
 * test rather than be eyeballed.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <pthread.h>
#include <sched.h>

static uint64_t work_per_us = 1000;

static uint64_t now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* Fixed arithmetic, no memory traffic, nothing the compiler can drop. */
static void burn_work(uint64_t units)
{
    static volatile uint64_t sink;
    uint64_t acc = sink | 1u;
    uint64_t i;

    for (i = 0; i < units; i++)
        acc = acc * 6364136223846793005ull + 1442695040888963407ull;
    sink = acc;
}

/* Fastest of a few trials: an interrupted trial understates the rate, and
 * the quickest one most nearly had the CPU to itself. */
static void calibrate(void)
{
    const uint64_t probe = 200000u;
    uint64_t best = (uint64_t)-1;
    int i;

    burn_work(probe);
    for (i = 0; i < 5; i++) {
        uint64_t t0 = now_us(), dt;

        burn_work(probe);
        dt = now_us() - t0;
        if (dt > 0 && dt < best)
            best = dt;
    }
    if (best != (uint64_t)-1 && best > 0)
        work_per_us = probe / best;
    if (work_per_us == 0)
        work_per_us = 1;
}

static void sleep_until(uint64_t target_us)
{
    uint64_t n = now_us();

    while (n < target_us) {
        struct timespec ts;
        uint64_t        d = target_us - n;

        ts.tv_sec  = (time_t)(d / 1000000ull);
        ts.tv_nsec = (long)(d % 1000000ull) * 1000L;
        if (nanosleep(&ts, NULL) == 0)
            break;
        n = now_us();
    }
}

static void usage(const char *a0)
{
    fprintf(stderr,
            "usage: %s [-P prio] [-p period_us] [-w work_us] [-n cycles]\n"
            "          [-j max_jitter_us] [-q]\n"
            "  -P prio          priority to run at (default 5 -- BELOW the\n"
            "                   chat clients, which is the whole point)\n"
            "  -p period_us     cycle period (default 10000)\n"
            "  -w work_us       CPU work per cycle (default 1000)\n"
            "  -n cycles        how many cycles (default 300)\n"
            "  -j max_jitter_us exit non-zero if worst lateness exceeds\n"
            "                   this (default 0 = never fail)\n"
            "  -S               saturating mode: spin flat out and report\n"
            "                   work completed instead of lateness. Use it\n"
            "                   to measure starvation on a host that steals\n"
            "                   CPU -- see the comment in main().\n"
            "  -q               summary only\n",
            a0);
}

int main(int argc, char **argv)
{
    int                prio = 5, quiet = 0, saturate = 0, c;
    uint64_t           period_us = 10000, work_us = 1000;
    long               cycles = 300, i;
    uint64_t           max_jitter = 0;
    uint64_t           worst = 0, sum = 0, start, target;
    unsigned           late_cycles = 0;
    struct sched_param param;
    int                policy = 0;

    while ((c = getopt(argc, argv, "P:p:w:n:j:Sqh")) != -1) {
        switch (c) {
        case 'P': prio = atoi(optarg); break;
        case 'p': period_us = strtoull(optarg, NULL, 10); break;
        case 'w': work_us = strtoull(optarg, NULL, 10); break;
        case 'n': cycles = strtol(optarg, NULL, 10); break;
        case 'j': max_jitter = strtoull(optarg, NULL, 10); break;
        case 'q': quiet = 1; break;
        case 'S': saturate = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    if (pthread_getschedparam(pthread_self(), &policy, &param) == 0) {
        param.sched_priority = prio;
        if (pthread_setschedparam(pthread_self(), policy, &param) != 0) {
            fprintf(stderr, "control_loop: cannot set priority %d: %s\n",
                    prio, strerror(errno));
            return 1;
        }
    }

    calibrate();

    /* Report the priority we ACTUALLY got, not the one we asked for.
     * A demo whose whole point is being outranked has to prove it is
     * outranked. */
    {
        struct sched_param chk;
        int                pol = 0, eff = -1;

        if (pthread_getschedparam(pthread_self(), &pol, &chk) == 0)
            eff = chk.sched_priority;
        printf("control_loop: prio=%d (asked %d) period=%lluus work=%lluus "
               "cycles=%ld\n",
               eff, prio, (unsigned long long)period_us,
               (unsigned long long)work_us, cycles);
    }

    /*
     * SATURATING MODE, and why it had to exist.
     *
     * The periodic mode below measures lateness, which is the natural way
     * to describe starvation and the wrong way to measure it here. On this
     * QEMU host the hypervisor steals CPU in lumps of about 9ms, which is
     * roughly a whole period, so lateness is mostly host noise. Measured
     * that way, a run with no AI load at all came out WORSE than a run
     * under a full flood -- 6853us worst lateness against 2788us. That is
     * not a result, it is a thermometer measuring the weather in the next
     * room.
     *
     * So this mode does what bench/sched_probe.c does, for the same
     * reason: spin flat out and report how much work got done. Starvation
     * then shows up as a share of the CPU rather than as a delay, and
     * because every competitor in the run eats the host's theft equally,
     * the theft cancels instead of swamping the signal.
     */
    if (saturate) {
        uint64_t t0 = now_us(), done = 0, elapsed, ms;

        while (now_us() - t0 < (uint64_t)cycles * period_us) {
            burn_work(10000);
            done += 10000;
        }
        elapsed = now_us() - t0;
        ms      = elapsed / 1000;
        printf("\ncontrol_loop saturating summary\n"
               "  work units   %llu\n"
               "  elapsed      %lluus\n"
               "  units_per_ms %llu\n",
               (unsigned long long)done,
               (unsigned long long)elapsed,
               (unsigned long long)(ms ? done / ms : 0));
        return 0;
    }

    start  = now_us();
    target = start;

    for (i = 0; i < cycles; i++) {
        uint64_t woke, late;

        target += period_us;
        sleep_until(target);

        woke = now_us();
        late = woke > target ? woke - target : 0;

        sum += late;
        if (late > worst)
            worst = late;
        /* "Late" meaning late enough to matter: more than a tenth of the
         * period. Below that it is scheduler noise, not starvation. */
        if (late > period_us / 10)
            late_cycles++;

        if (!quiet && late > period_us)
            printf("  [%4ld] late by %lluus (more than a whole period)\n",
                   i, (unsigned long long)late);

        burn_work(work_us * work_per_us);
    }

    printf("\ncontrol_loop summary\n"
           "  cycles       %ld\n"
           "  mean late    %lluus\n"
           "  worst late   %lluus\n"
           "  late cycles  %u (> %lluus)\n"
           "  wall clock   %lluus for %lluus of scheduled time\n",
           cycles,
           (unsigned long long)(cycles ? sum / (uint64_t)cycles : 0),
           (unsigned long long)worst,
           late_cycles, (unsigned long long)(period_us / 10),
           (unsigned long long)(now_us() - start),
           (unsigned long long)((uint64_t)cycles * period_us));

    if (max_jitter && worst > max_jitter) {
        printf("  FAIL worst lateness %lluus exceeds %lluus\n",
               (unsigned long long)worst, (unsigned long long)max_jitter);
        return 1;
    }
    return 0;
}
