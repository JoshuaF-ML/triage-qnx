/*
 * triage_stat.c -- read the server's counters.
 *
 * A text dashboard, and the thing the week-3 web dashboard will replace.
 * Also the simplest possible proof that /dev/triage is a real device: this
 * program opens it and asks it a question, nothing more.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "triage.h"

static void show(const triage_stats_t *s)
{
    triage_tier_t k;
    triage_task_t t;
    unsigned      b;
    unsigned long long decided = s->deadlines_met + s->deadlines_missed;

    printf("requests        %llu\n", (unsigned long long)s->requests);
    printf("deadlines met   %llu (%.1f%%)\n",
           (unsigned long long)s->deadlines_met,
           decided ? 100.0 * (double)s->deadlines_met / (double)decided : 0.0);
    printf("deadlines miss  %llu\n", (unsigned long long)s->deadlines_missed);
    printf("degraded        %llu\n", (unsigned long long)s->degraded);
    printf("preemptions     %llu\n", (unsigned long long)s->preemptions);
    printf("stolen total    %lluus\n", (unsigned long long)s->stolen_us);
    printf("worst total     %lluus\n", (unsigned long long)s->worst_total_us);
    printf("worst queue     %lluus\n", (unsigned long long)s->worst_queue_us);
    printf("client prio     %d..%d\n", s->min_prio_seen, s->max_prio_seen);

    printf("by tier         ");
    for (k = TRIAGE_TIER_SMALL; k < TRIAGE_TIER__COUNT; k++)
        printf("%s=%llu ", triage_tier_name(k),
               (unsigned long long)s->per_tier[k]);
    printf("\n");

    printf("by task         ");
    for (t = 0; t < TRIAGE_TASK__COUNT; t++)
        printf("%s=%llu ", triage_task_name(t),
               (unsigned long long)s->per_task[t]);
    printf("\n");

    printf("cost model (clean cost, us)\n");
    for (t = 0; t < TRIAGE_TASK__COUNT; t++) {
        printf("  %-6s ", triage_task_name(t));
        for (k = TRIAGE_TIER_SMALL; k < TRIAGE_TIER__COUNT; k++)
            printf("%s=%llu ", triage_tier_name(k),
                   (unsigned long long)s->est_us[t][k]);
        printf("\n");
    }

    /* The other half of the prediction. Only bands that have seen traffic
     * are worth printing -- the rest are still at their 100% seed and say
     * nothing. A band reading well above 100% is the scheduler's reason
     * for degrading requests at that priority. */
    printf("interference (worst recent, %% of clean cost)\n");
    for (b = 0; b < TRIAGE_PRIO_BANDS; b++) {
        if (s->band_requests[b] == 0)
            continue;
        printf("  prio %3u-%-3u  %4u%%  (%llu requests)\n",
               b * 16u, b * 16u + 15u, s->infl_pct[b],
               (unsigned long long)s->band_requests[b]);
    }
}

static void usage(const char *a0)
{
    fprintf(stderr,
            "usage: %s [-w seconds] [-r]\n"
            "  -w seconds  repeat every N seconds\n"
            "  -r          reset the counters and exit\n",
            a0);
}

int main(int argc, char **argv)
{
    int fd, c, watch = 0, reset = 0;

    while ((c = getopt(argc, argv, "w:rh")) != -1) {
        switch (c) {
        case 'w': watch = atoi(optarg); break;
        case 'r': reset = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    if ((fd = triage_open(NULL)) < 0) {
        fprintf(stderr, "triage_stat: open %s: %s\n",
                TRIAGE_DEV_PATH, strerror(errno));
        return 1;
    }

    if (reset) {
        if (triage_reset_stats(fd) != 0) {
            fprintf(stderr, "triage_stat: reset: %s\n", strerror(errno));
            return 1;
        }
        printf("triage_stat: counters reset\n");
        triage_close(fd);
        return 0;
    }

    for (;;) {
        triage_stats_t s;

        if (triage_stats(fd, &s) != 0) {
            fprintf(stderr, "triage_stat: stats: %s\n", strerror(errno));
            return 1;
        }
        show(&s);
        if (watch <= 0)
            break;
        printf("\n");
        sleep((unsigned)watch);
    }

    triage_close(fd);
    return 0;
}
