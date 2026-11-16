/*
 * ask.c -- the one client used by every step of the codelab.
 *
 *   ./ask -D /dev/clab1                     one question
 *   ./ask -D /dev/clab2 -P 40               ask at priority 40
 *   ./ask -D /dev/clab3 -P 40 -w 50000 -n 5 ask for 50ms of thinking, 5x
 *
 * It does three things worth copying into your own code:
 *
 * 1. It RAISES ITS OWN PRIORITY and then does nothing else about urgency.
 *    There is no "urgent" flag in the request. The priority of this thread
 *    IS the urgency, and QNX carries it into the server.
 *
 * 2. It CHECKS the priority pair the server reports rather than printing
 *    it for a human to admire. If the server did not run our work at our
 *    priority, this program exits non-zero. A claim that is only printed
 *    is a claim nobody is testing.
 *
 * 3. It times the round trip itself, so the latency number does not come
 *    from the thing being measured.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <time.h>

#include <sys/neutrino.h>
#include <sys/syspage.h>

#include "clab.h"

static uint64_t cps_us;

static uint64_t now_us(void)
{
    return (uint64_t)ClockCycles() / cps_us;
}

static int set_prio(int prio)
{
    struct sched_param p;

    memset(&p, 0, sizeof p);
    p.sched_priority = prio;
    return pthread_setschedparam(pthread_self(), SCHED_RR, &p);
}

static int get_prio(void)
{
    struct sched_param p;
    int                policy = 0;

    if (pthread_getschedparam(pthread_self(), &policy, &p) != EOK)
        return -1;
    return p.sched_priority;
}

static void usage(const char *a0)
{
    fprintf(stderr,
            "usage: %s -D dev [-P prio] [-w burn_us] [-n count] [-f] [-q]\n"
            "  -D dev      device to ask (e.g. /dev/clab1)\n"
            "  -P prio     priority to ask at (default: leave alone)\n"
            "  -w burn_us  ask the server to think this long\n"
            "  -n count    number of questions (default 1)\n"
            "  -W wait_us  open the device, then wait this long before\n"
            "              the first question. Needed whenever the server\n"
            "              may be busy: open() is itself a message to the\n"
            "              same resource manager and queues on the same\n"
            "              threads, so a late open hides the wait OUTSIDE\n"
            "              the timed region.\n"
            "  -f          expect priority NOT to be inherited, and fail\n"
            "              if it is. For the fixed-priority experiment.\n"
            "  -q          summary only\n", a0);
}

int main(int argc, char **argv)
{
    const char *dev = NULL;
    int         prio = -1, quiet = 0, expect_fixed = 0, c, fd;
    uint32_t    burn_us = 0, wait_us = 0;
    long        count = 1, i;
    unsigned    inherited = 0, not_inherited = 0, errors = 0, unreported = 0;
    uint64_t    worst_us = 0, sum_us = 0;

    cps_us = SYSPAGE_ENTRY(qtime)->cycles_per_sec / 1000000u;
    if (cps_us == 0)
        cps_us = 1;

    while ((c = getopt(argc, argv, "D:P:w:W:n:fqh")) != -1) {
        switch (c) {
        case 'D': dev = optarg; break;
        case 'P': prio = atoi(optarg); break;
        case 'w': burn_us = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'W': wait_us = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'n': count = strtol(optarg, NULL, 10); break;
        case 'f': expect_fixed = 1; break;
        case 'q': quiet = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }
    if (dev == NULL) {
        usage(argv[0]);
        return 1;
    }

    if (prio >= 0 && set_prio(prio) != EOK) {
        fprintf(stderr, "ask: cannot set priority %d: %s\n"
                        "  (raising priority needs PROCMGR_AID_PRIORITY;"
                        " try as root)\n", prio, strerror(errno));
        return 1;
    }

    if ((fd = open(dev, O_RDWR)) < 0) {
        fprintf(stderr, "ask: open %s: %s\n", dev, strerror(errno));
        return 1;
    }

    printf("ask: dev=%s my_prio=%d burn=%uus n=%ld\n",
           dev, get_prio(), burn_us, count);

    /*
     * Wait AFTER opening, if asked to.
     *
     * This option exists because of a measurement bug worth inheriting.
     * With a single-threaded server, an urgent client started while the
     * server was mid-way through a 2-second request reported a 1.5 ms
     * round trip -- it was apparently not queued behind anything at all.
     *
     * The wait was real. It happened inside open(), because the connect
     * message goes to the same resource manager and queues on the same
     * receive threads as the devctl does, and open() is not inside the
     * timed region. The server looked fast because the client had already
     * paid, somewhere nobody was looking.
     *
     * So: connect while the server is idle, then start asking. Now the
     * queueing appears where it is being measured.
     */
    if (wait_us) {
        struct timespec ts;

        ts.tv_sec  = (time_t)(wait_us / 1000000u);
        ts.tv_nsec = (long)((wait_us % 1000000u) * 1000u);
        nanosleep(&ts, NULL);
    }

    for (i = 0; i < count; i++) {
        clab_msg_t m;
        uint64_t   t0, dt;

        memset(&m, 0, sizeof m);
        m.burn_us = burn_us;

        t0 = now_us();
        if (devctl(fd, CLAB_DCMD_ASK, &m, sizeof m, NULL) != EOK) {
            errors++;
            if (!quiet)
                printf("  [%ld] ERROR %s\n", i, strerror(errno));
            continue;
        }
        dt = now_us() - t0;

        sum_us += dt;
        if (dt > worst_us)
            worst_us = dt;

        /* A server that does not report priorities at all -- step 1 --
         * sets both to -1. That is not evidence either way, so it is
         * counted separately rather than quietly passing the check. */
        if (m.client_prio < 0 && m.server_prio < 0)
            unreported++;
        else if (m.client_prio == m.server_prio)
            inherited++;
        else
            not_inherited++;

        if (!quiet)
            printf("  [%ld] %6lluus prio=%d/%d served=%lluus  %s\n",
                   i, (unsigned long long)dt,
                   m.client_prio, m.server_prio,
                   (unsigned long long)m.served_us, m.text);
    }

    printf("\nask summary: %u inherited, %u not inherited, %u unreported, %u errors\n"
           "  mean %lluus  worst %lluus\n",
           inherited, not_inherited, unreported, errors,
           (unsigned long long)((count - errors) ? sum_us / (count - errors) : 0),
           (unsigned long long)worst_us);

    close(fd);

    if (errors)
        return 1;

    /*
     * The assertion. Which way round it points is the -f flag's only job:
     * without it, every request must have been served at our priority;
     * with it, none of them must have been. Step 2 runs the same binary
     * both ways against the same server source, and that is the
     * experiment.
     */
    if (unreported && !inherited && !not_inherited) {
        printf("  n/a  this server does not report priorities\n");
        return 0;
    }
    if (expect_fixed) {
        if (inherited) {
            printf("  FAIL expected fixed priority, but %u requests were "
                   "inherited\n", inherited);
            return 1;
        }
        printf("  OK   priority was not inherited, as expected\n");
        return 0;
    }
    if (not_inherited) {
        printf("  FAIL priority was not inherited on %u requests\n",
               not_inherited);
        return 1;
    }
    printf("  OK   priority was inherited on every request\n");
    return 0;
}
