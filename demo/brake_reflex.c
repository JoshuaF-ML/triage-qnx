/*
 * brake_reflex.c -- the safety-critical client.
 *
 * Stands in for the part of a car or robot that must not wait: it asks
 * "is that a pedestrian?" on a fixed period, with a hard deadline, at a
 * high priority. It never asks for a particular model -- it tells Triage
 * how long it can wait and accepts whatever fits.
 *
 * Run it alongside demo/chat_load, which floods the same /dev/triage with
 * low-priority chat requests. The whole claim of this project is that
 * this program's numbers do not change when that flood starts.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "triage.h"

static void sleep_us(uint64_t us)
{
    struct timespec ts;

    ts.tv_sec  = (time_t)(us / 1000000u);
    ts.tv_nsec = (long)((us % 1000000u) * 1000u);
    nanosleep(&ts, NULL);
}

static void usage(const char *a0)
{
    fprintf(stderr,
            "usage: %s [-P prio] [-d deadline_us] [-p period_us] [-n count] [-q]\n"
            "  -P prio         priority to run at (default 40)\n"
            "  -m pct          minimum %% of deadlines met to exit 0 (default 100)\n"
            "  -d deadline_us  deadline per request (default 20000 = 20ms)\n"
            "  -p period_us    how often to ask (default 50000 = 50ms)\n"
            "  -n count        number of requests (default 200, 0 = forever)\n"
            "  -q              summary only\n",
            a0);
}

int main(int argc, char **argv)
{
    int      prio = 40, quiet = 0, c, fd;
    /* Minimum percentage of deadlines that must be met for this program to
     * exit 0. Defaults to 100, because that is the claim; a caller that
     * knows its platform steals CPU can lower it deliberately and visibly
     * rather than having a weaker default hidden in here. */
    long     min_met_pct = 100;
    /* Which device to ask. The point of -D is that the SAME client, with
     * the same code path, can be pointed at Triage or at the FCFS
     * baseline -- so a difference in the numbers cannot be the client. */
    const char *dev = NULL;
    /* Against the baseline, priority NOT being inherited is the expected
     * result rather than a failure, so -b stops that from being an error.
     * It never silences the report, only the exit code. */
    int      baseline = 0;
    uint32_t deadline_us = 20000;
    uint64_t period_us = 50000;
    long     count = 200, i;
    unsigned met = 0, missed = 0, degraded = 0, errors = 0;
    uint64_t worst_us = 0, sum_us = 0;
    /* Priority inheritance is the central claim of this project, so it gets
     * checked rather than printed for a human to eyeball. Every reply
     * carries the client's priority and the priority the server thread
     * actually ran at; if they ever differ, the mechanism is not doing what
     * the README says, and this program exits non-zero. */
    unsigned prio_checked = 0, prio_mismatch = 0;
    unsigned preempted_reqs = 0;
    uint64_t stolen_total_us = 0;

    while ((c = getopt(argc, argv, "P:d:p:n:m:D:bqh")) != -1) {
        switch (c) {
        case 'P': prio = atoi(optarg); break;
        case 'm': min_met_pct = strtol(optarg, NULL, 10); break;
        case 'D': dev = optarg; break;
        case 'b': baseline = 1; break;
        case 'd': deadline_us = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'p': period_us = strtoull(optarg, NULL, 10); break;
        case 'n': count = strtol(optarg, NULL, 10); break;
        case 'q': quiet = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    /* Be urgent. This is the only thing we do to get urgent AI: QNX
     * carries this priority into the server on every request. */
    if (triage_set_priority(prio) != 0) {
        fprintf(stderr, "brake_reflex: cannot set priority %d: %s\n"
                        "  (raising priority needs PROCMGR_AID_PRIORITY; try as root)\n",
                prio, strerror(errno));
        return 1;
    }

    if ((fd = triage_open(dev)) < 0) {
        fprintf(stderr, "brake_reflex: open %s: %s\n",
                dev ? dev : TRIAGE_DEV_PATH, strerror(errno));
        return 1;
    }

    printf("brake_reflex: dev=%s prio=%d deadline=%uus period=%lluus\n",
           dev ? dev : TRIAGE_DEV_PATH,
           triage_get_priority(), deadline_us,
           (unsigned long long)period_us);

    for (i = 0; count == 0 || i < count; i++) {
        triage_reply_t rep;

        if (triage_ask(fd, TRIAGE_TASK_DETECT, "front camera frame",
                       deadline_us, &rep) != 0) {
            errors++;
            if (!quiet)
                printf("  [%4ld] ERROR %s\n", i, strerror(errno));
            sleep_us(period_us);
            continue;
        }

        prio_checked++;
        if (rep.client_prio != rep.server_prio) {
            prio_mismatch++;
            if (!quiet && !baseline)
                printf("  [%4ld] PRIORITY NOT INHERITED: client=%d server=%d\n",
                       i, rep.client_prio, rep.server_prio);
        }
        if (rep.preempted) {
            preempted_reqs++;
            stolen_total_us += rep.stolen_us;
        }

        if (rep.deadline_met)
            met++;
        else
            missed++;
        if (rep.degraded)
            degraded++;
        sum_us += rep.total_us;
        if (rep.total_us > worst_us)
            worst_us = rep.total_us;

        if (!quiet)
            printf("  [%4ld] %-4s %6lluus tier=%-6s prio=%d/%d preempt=%u%s\n",
                   i,
                   rep.deadline_met ? "MET" : "MISS",
                   (unsigned long long)rep.total_us,
                   triage_tier_name((triage_tier_t)rep.tier_used),
                   rep.client_prio, rep.server_prio,
                   rep.preempted,
                   rep.degraded ? " degraded" : "");

        sleep_us(period_us);
    }

    printf("\nbrake_reflex summary\n"
           "  requests  %u\n"
           "  met       %u (%.1f%%)\n"
           "  missed    %u\n"
           "  degraded  %u\n"
           "  errors    %u\n"
           "  mean      %lluus\n"
           "  worst     %lluus\n"
           "  preempted %u req, %lluus stolen\n"
           "  prio      %u checked, %u not inherited\n",
           met + missed,
           met, (met + missed) ? 100.0 * met / (met + missed) : 0.0,
           missed, degraded, errors,
           (unsigned long long)((met + missed) ? sum_us / (met + missed) : 0),
           (unsigned long long)worst_us,
           preempted_reqs, (unsigned long long)stolen_total_us,
           prio_checked, prio_mismatch);

    if (prio_mismatch && !baseline)
        printf("  FAIL priority was not inherited on %u of %u requests\n",
               prio_mismatch, prio_checked);
    else if (prio_mismatch)
        printf("  (expected: this server does not inherit priority)\n");

    triage_close(fd);

    /* Priority inheritance is pass/fail -- it either happened on every
     * request or the project's central claim is false. Deadlines are a
     * percentage, because the platform gets a vote. */
    if (prio_mismatch && !baseline)
        return 1;
    if (met + missed == 0)
        return errors ? 1 : 0;
    return (100u * met / (met + missed) < (unsigned long)min_met_pct) ? 1 : 0;
}
