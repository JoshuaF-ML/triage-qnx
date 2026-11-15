/*
 * chat_load.c -- the greedy, unimportant client.
 *
 * Stands in for infotainment: several threads asking the assistant long
 * questions as fast as they can, at a low priority, with a generous
 * deadline. It exists to be in the way.
 *
 * On a first-come-first-served inference server, this program is what
 * makes brake_reflex miss its deadlines. On Triage it should not be able
 * to, because every one of its requests is served at ITS priority, not at
 * the server's.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "triage.h"

static int      n_threads   = 4;
static int      prio        = 10;
static uint32_t deadline_us = 2000000;   /* 2 s: we are not in a hurry */
static int      run_seconds = 30;
static const char *dev;          /* NULL == TRIAGE_DEV_PATH */
static volatile int stop;

static unsigned long long total_requests;
static unsigned long long total_met;
static pthread_mutex_t    tally = PTHREAD_MUTEX_INITIALIZER;

static const char *questions[] = {
    "what is the weather like in Bengaluru today",
    "play something upbeat for the drive home",
    "how far is the next charging station",
    "summarise my unread messages",
    "what is the capital of Finland",
};
#define N_QUESTIONS ((int)(sizeof questions / sizeof questions[0]))

static void *worker(void *arg)
{
    long id = (long)arg;
    int  fd;
    unsigned long long n = 0, met = 0;
    unsigned errs = 0;

    if (triage_set_priority(prio) != 0) {
        fprintf(stderr, "chat_load[%ld]: set priority: %s\n",
                id, strerror(errno));
        return NULL;
    }

    if ((fd = triage_open(dev)) < 0) {
        fprintf(stderr, "chat_load[%ld]: open %s: %s\n",
                id, dev ? dev : TRIAGE_DEV_PATH, strerror(errno));
        return NULL;
    }

    while (!stop) {
        triage_reply_t rep;
        const char *q = questions[(int)((n + (unsigned)id) % N_QUESTIONS)];

        if (triage_ask(fd, TRIAGE_TASK_CHAT, q, deadline_us, &rep) == 0) {
            n++;
            if (rep.deadline_met)
                met++;
            errs = 0;
        } else if (++errs >= 20) {
            /* Give up rather than spin. A failing loop with no sleep is a
             * busy-wait at this thread's priority, which would be load of
             * a completely different kind from the load this program is
             * supposed to generate -- and it would be invisible, because
             * the request counter would stay at zero. */
            fprintf(stderr, "chat_load[%ld]: giving up after %u consecutive "
                            "errors (last: %s)\n", id, errs, strerror(errno));
            break;
        }
    }

    triage_close(fd);

    pthread_mutex_lock(&tally);
    total_requests += n;
    total_met      += met;
    pthread_mutex_unlock(&tally);
    return NULL;
}

static void usage(const char *a0)
{
    fprintf(stderr,
            "usage: %s [-n threads] [-P prio] [-d deadline_us] [-s seconds]\n"
            "  -n threads      concurrent chat clients (default 4)\n"
            "  -P prio         priority to run at (default 10)\n"
            "  -d deadline_us  deadline per request (default 2000000 = 2s)\n"
            "  -s seconds      how long to flood (default 30, 0 = forever)\n",
            a0);
}

int main(int argc, char **argv)
{
    pthread_t *tids;
    int        c, i;
    time_t     started;

    while ((c = getopt(argc, argv, "n:P:d:s:D:h")) != -1) {
        switch (c) {
        case 'D': dev = optarg; break;
        case 'n': n_threads = atoi(optarg); break;
        case 'P': prio = atoi(optarg); break;
        case 'd': deadline_us = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 's': run_seconds = atoi(optarg); break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }
    if (n_threads < 1)
        n_threads = 1;

    if ((tids = calloc((size_t)n_threads, sizeof *tids)) == NULL) {
        perror("chat_load: calloc");
        return 1;
    }

    printf("chat_load: dev=%s %d threads at prio %d, deadline %uus, %ds\n",
           dev ? dev : TRIAGE_DEV_PATH,
           n_threads, prio, deadline_us, run_seconds);

    for (i = 0; i < n_threads; i++) {
        if (pthread_create(&tids[i], NULL, worker, (void *)(long)i) != 0) {
            perror("chat_load: pthread_create");
            n_threads = i;
            break;
        }
    }

    started = time(NULL);
    while (run_seconds == 0 || time(NULL) - started < run_seconds) {
        struct timespec ts = { 0, 200 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    stop = 1;

    for (i = 0; i < n_threads; i++)
        pthread_join(tids[i], NULL);

    printf("chat_load: %llu requests, %llu met deadline, %.1f req/s\n",
           total_requests, total_met,
           run_seconds > 0 ? (double)total_requests / run_seconds : 0.0);

    free(tids);
    return 0;
}
