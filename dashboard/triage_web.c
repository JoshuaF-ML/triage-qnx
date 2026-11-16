/*
 * triage_web.c -- the dashboard, served by the QNX target itself.
 *
 * WHAT THIS IS
 * ------------
 * Two things in one process, deliberately:
 *
 *   1. A CLIENT. A thread at priority 40 asks /dev/triage "what is in
 *      this frame?" on a fixed period with a deadline, exactly as
 *      demo/brake_reflex does, and keeps the last few hundred replies in
 *      a ring buffer.
 *
 *   2. A WEB SERVER. A tiny HTTP/1.1 server hands that ring buffer to a
 *      browser as JSON, along with the resource manager's own counters.
 *
 * It is one process because the point is that everything on the screen is
 * a real reply from a real request to a real resource manager on a real
 * QNX target. Nothing in the browser is simulated except the car's
 * position on the road, and that is driven by the replies.
 *
 * WHY THE BROWSER IS NOT LYING TO YOU
 * -----------------------------------
 * Every datum the UI draws is a field of triage_reply_t that the server
 * filled in, or a field of triage_stats_t:
 *
 *   the two priorities        rep.client_prio, rep.server_prio
 *   which model ran           rep.tier_used, rep.degraded
 *   whether it was on time    rep.deadline_met and our own round trip
 *   what it saw, and truth    rep.det_class, rep.det_truth
 *   what it cost              rep.total_us, rep.compute_us, rep.stolen_us
 *   why that tier             rep.est_used_us, rep.infl_pct
 *
 * The round trip is timed HERE, around the devctl(), not taken from the
 * server's own total_us -- see docs/architecture.md for the weeks that
 * distinction cost.
 *
 * CONTROLS
 * --------
 * The browser can change the deadline live and start or stop a chat
 * flood. Both are the demo: tighten the deadline and watch the tier fall
 * and the detector get things wrong; start the flood and watch the
 * priority-40 work carry on meeting its deadline anyway.
 *
 *   ./triage_web -r /data/dashboard &
 *   # then from the host:  http://localhost:8080/
 */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <sys/neutrino.h>
#include <sys/syspage.h>

#include "triage.h"

extern char **environ;

#define RING        256
#define DEF_PORT    8080
#define DEF_PERIOD  400000u      /* 2.5 frames/s -- a readable demo rate */
#define DEF_DEADLINE 20000u

/* ------------------------------------------------------------------ */
/* Shared state                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t seq;
    uint64_t rtt_us;         /* OUR measurement, around the devctl      */
    uint64_t total_us;       /* the server's, for comparison            */
    uint64_t compute_us;
    uint64_t stolen_us;
    uint32_t tier;
    uint32_t degraded;
    uint32_t met;
    uint32_t preempted;
    uint32_t est_us;
    uint32_t infl_pct;
    uint32_t confidence;
    uint32_t capped;
    int32_t  cls;
    int32_t  truth;
    int32_t  cprio;
    int32_t  sprio;
    int32_t  err;            /* errno, or 0 */
} event_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static event_t  ring[RING];
static uint64_t ring_n;              /* total events ever               */
static uint32_t cfg_deadline_us = DEF_DEADLINE;
static uint32_t cfg_period_us   = DEF_PERIOD;
static int      cfg_prio        = 40;
static pid_t    flood_pid       = -1;
static int      flood_threads   = 4;

static const char *dev_path;
static const char *web_root = ".";
static volatile int running = 1;

static uint64_t cps_us = 1;

static uint64_t now_us(void)
{
    return (uint64_t)ClockCycles() / cps_us;
}

static void sleep_us(uint64_t us)
{
    struct timespec ts;

    ts.tv_sec  = (time_t)(us / 1000000u);
    ts.tv_nsec = (long)((us % 1000000u) * 1000u);
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------------ */
/* The car: a priority-40 client, on a period, with a deadline          */
/* ------------------------------------------------------------------ */

static void *car_thread(void *arg)
{
    int      fd;
    uint64_t i = 0;

    (void)arg;

    /* Be urgent. This is the only thing we do about urgency; QNX carries
     * it into the server on every request. */
    if (triage_set_priority(cfg_prio) != 0)
        fprintf(stderr, "triage_web: cannot set priority %d: %s\n",
                cfg_prio, strerror(errno));

    if ((fd = triage_open(dev_path)) < 0) {
        fprintf(stderr, "triage_web: open %s: %s\n",
                dev_path ? dev_path : TRIAGE_DEV_PATH, strerror(errno));
        return NULL;
    }

    while (running) {
        triage_reply_t rep;
        event_t        e;
        char           prompt[64];
        uint32_t       deadline;
        uint64_t       period, t0;
        int            rc;

        pthread_mutex_lock(&lock);
        deadline = cfg_deadline_us;
        period   = cfg_period_us;
        pthread_mutex_unlock(&lock);

        /* A different frame every time. Cameras do not send the same
         * image twice, and varying it is what makes the accuracy figure
         * on screen mean anything. */
        snprintf(prompt, sizeof prompt, "front camera frame %llu",
                 (unsigned long long)i);

        memset(&e, 0, sizeof e);
        t0 = now_us();
        rc = triage_ask(fd, TRIAGE_TASK_DETECT, prompt, deadline, &rep);
        e.rtt_us = now_us() - t0;
        e.seq    = i++;

        if (rc != 0) {
            e.err   = errno;
            e.cls   = -1;
            e.truth = -1;
        } else {
            e.total_us    = rep.total_us;
            e.compute_us  = rep.compute_us;
            e.stolen_us   = rep.stolen_us;
            e.tier        = rep.tier_used;
            e.degraded    = rep.degraded;
            e.met         = rep.deadline_met;
            e.preempted   = rep.preempted;
            e.est_us      = rep.est_used_us;
            e.infl_pct    = rep.infl_pct;
            e.confidence  = rep.confidence;
            e.capped      = rep.budget_capped;
            e.cls         = rep.det_class;
            e.truth       = rep.det_truth;
            e.cprio       = rep.client_prio;
            e.sprio       = rep.server_prio;
        }

        pthread_mutex_lock(&lock);
        ring[ring_n % RING] = e;
        ring_n++;
        pthread_mutex_unlock(&lock);

        sleep_us(period);
    }

    triage_close(fd);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* The chat flood, started and stopped from the browser                */
/* ------------------------------------------------------------------ */

static void flood_stop(void)
{
    if (flood_pid > 0) {
        kill(flood_pid, SIGTERM);
        waitpid(flood_pid, NULL, 0);
        flood_pid = -1;
    }
}

static int flood_start(void)
{
    char       nthreads[16], secs[16];
    char      *argv[12];
    int        n = 0, rc;
    pid_t      pid;

    if (flood_pid > 0)
        return 0;

    snprintf(nthreads, sizeof nthreads, "%d", flood_threads);
    snprintf(secs, sizeof secs, "%d", 0);   /* 0 == until we kill it */

    argv[n++] = (char *)"chat_load";
    argv[n++] = (char *)"-n";  argv[n++] = nthreads;
    argv[n++] = (char *)"-P";  argv[n++] = (char *)"10";
    argv[n++] = (char *)"-s";  argv[n++] = secs;
    if (dev_path) { argv[n++] = (char *)"-D"; argv[n++] = (char *)dev_path; }
    argv[n] = NULL;

    /* ./chat_load, next to us. posix_spawn rather than system() so there
     * is a pid to kill when the browser turns the flood off again. */
    rc = posix_spawn(&pid, "./chat_load", NULL, NULL, argv, environ);
    if (rc != 0) {
        rc = posix_spawn(&pid, "/data/chat_load", NULL, NULL, argv, environ);
        if (rc != 0)
            return -1;
    }
    flood_pid = pid;
    return 0;
}

/* ------------------------------------------------------------------ */
/* JSON                                                                */
/* ------------------------------------------------------------------ */

#define JBUF (256 * 1024)

static int append(char *buf, int cap, int *len, const char *fmt, ...)
{
    va_list ap;
    int     n;

    if (*len >= cap - 1)
        return -1;
    va_start(ap, fmt);
    n = vsnprintf(buf + *len, (size_t)(cap - *len), fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    *len += (n > cap - *len) ? (cap - *len) : n;
    return 0;
}

static int build_state(int fd_stats, char *buf, int cap)
{
    triage_stats_t st;
    int            len = 0, have_stats, i, first;
    uint64_t       n, start;

    have_stats = (fd_stats >= 0 && triage_stats(fd_stats, &st) == 0);
    if (!have_stats)
        memset(&st, 0, sizeof st);

    pthread_mutex_lock(&lock);
    n     = ring_n;
    start = (n > RING) ? n - RING : 0;

    append(buf, cap, &len,
           "{\"seq\":%llu,\"deadline_us\":%u,\"period_us\":%u,"
           "\"prio\":%d,\"flood\":%s,\"dev\":\"%s\",",
           (unsigned long long)n, cfg_deadline_us, cfg_period_us,
           cfg_prio, flood_pid > 0 ? "true" : "false",
           dev_path ? dev_path : TRIAGE_DEV_PATH);

    append(buf, cap, &len, "\"events\":[");
    first = 1;
    for (; start < n; start++) {
        const event_t *e = &ring[start % RING];

        append(buf, cap, &len,
               "%s{\"seq\":%llu,\"rtt\":%llu,\"total\":%llu,\"compute\":%llu,"
               "\"stolen\":%llu,\"tier\":%u,\"degraded\":%u,\"met\":%u,"
               "\"preempted\":%u,\"est\":%u,\"infl\":%u,\"conf\":%u,"
               "\"capped\":%u,\"cls\":%d,\"truth\":%d,\"cprio\":%d,"
               "\"sprio\":%d,\"err\":%d}",
               first ? "" : ",",
               (unsigned long long)e->seq,
               (unsigned long long)e->rtt_us,
               (unsigned long long)e->total_us,
               (unsigned long long)e->compute_us,
               (unsigned long long)e->stolen_us,
               e->tier, e->degraded, e->met, e->preempted,
               e->est_us, e->infl_pct, e->confidence, e->capped,
               e->cls, e->truth, e->cprio, e->sprio, e->err);
        first = 0;
    }
    append(buf, cap, &len, "],");
    pthread_mutex_unlock(&lock);

    append(buf, cap, &len, "\"stats\":{\"have\":%s", have_stats ? "true" : "false");
    if (have_stats) {
        append(buf, cap, &len,
               ",\"requests\":%llu,\"met\":%llu,\"missed\":%llu,"
               "\"degraded\":%llu,\"preemptions\":%llu,\"stolen_us\":%llu,"
               "\"cancelled\":%llu,\"capped\":%llu,\"worst_total_us\":%llu,"
               "\"detect\":%llu,\"chat\":%llu",
               (unsigned long long)st.requests,
               (unsigned long long)st.deadlines_met,
               (unsigned long long)st.deadlines_missed,
               (unsigned long long)st.degraded,
               (unsigned long long)st.preemptions,
               (unsigned long long)st.stolen_us,
               (unsigned long long)st.cancelled,
               (unsigned long long)st.budget_capped,
               (unsigned long long)st.worst_total_us,
               (unsigned long long)st.per_task[TRIAGE_TASK_DETECT],
               (unsigned long long)st.per_task[TRIAGE_TASK_CHAT]);

        append(buf, cap, &len, ",\"per_tier\":[");
        for (i = 0; i < TRIAGE_TIER__COUNT; i++)
            append(buf, cap, &len, "%s%llu", i ? "," : "",
                   (unsigned long long)st.per_tier[i]);
        append(buf, cap, &len, "]");

        append(buf, cap, &len, ",\"est_detect\":[");
        for (i = 0; i < TRIAGE_TIER__COUNT; i++)
            append(buf, cap, &len, "%s%llu", i ? "," : "",
                   (unsigned long long)st.est_us[TRIAGE_TASK_DETECT][i]);
        append(buf, cap, &len, "]");

        append(buf, cap, &len, ",\"est_chat\":[");
        for (i = 0; i < TRIAGE_TIER__COUNT; i++)
            append(buf, cap, &len, "%s%llu", i ? "," : "",
                   (unsigned long long)st.est_us[TRIAGE_TASK_CHAT][i]);
        append(buf, cap, &len, "]");

        /* Interference per priority band, but only the bands that have
         * actually seen traffic -- an untouched band reports 100% and
         * would draw a flat line that means "no data", not "no
         * interference". */
        append(buf, cap, &len, ",\"bands\":[");
        first = 1;
        for (i = 0; i < TRIAGE_PRIO_BANDS; i++) {
            if (st.band_requests[i] == 0)
                continue;
            append(buf, cap, &len,
                   "%s{\"band\":%d,\"lo\":%d,\"hi\":%d,\"infl\":%u,\"n\":%llu}",
                   first ? "" : ",", i, i * 16, i * 16 + 15,
                   st.infl_pct[i], (unsigned long long)st.band_requests[i]);
            first = 0;
        }
        append(buf, cap, &len, "]");
    }
    append(buf, cap, &len, "}}");

    return len;
}

/* ------------------------------------------------------------------ */
/* A very small HTTP/1.1 server                                        */
/* ------------------------------------------------------------------ */

/*
 * Close without throwing away what we just sent.
 *
 * close() on a socket with unread data from the peer sends an RST, and
 * the RST can discard data still sitting in the send buffer -- so the
 * browser gets ERR_EMPTY_RESPONSE for a reply that was written in full.
 * With a poll every 500ms that showed up as the dashboard flickering
 * "target not responding" for no reason at all.
 *
 * So: say we are done writing, then drain whatever the peer still has to
 * say until it closes too, with a short timeout so a rude client cannot
 * hold the single-threaded accept loop.
 */
static void polite_close(int s)
{
    struct timeval tv = { 0, 200000 };   /* 200ms */
    char           sink[512];
    int            i;

    shutdown(s, SHUT_WR);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    for (i = 0; i < 8; i++)
        if (recv(s, sink, sizeof sink, 0) <= 0)
            break;
    close(s);
}

static void send_all(int s, const char *p, int n)
{
    while (n > 0) {
        int k = (int)send(s, p, (size_t)n, 0);
        if (k <= 0)
            return;
        p += k;
        n -= k;
    }
}

static void send_simple(int s, const char *status, const char *ctype,
                        const char *body, int blen)
{
    char hdr[512];
    int  n;

    n = snprintf(hdr, sizeof hdr,
                 "HTTP/1.1 %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %d\r\n"
                 "Cache-Control: no-store\r\n"
                 "Connection: close\r\n"
                 "\r\n", status, ctype, blen);
    send_all(s, hdr, n);
    if (blen > 0)
        send_all(s, body, blen);
}

/* Serve a file from web_root. Paths are checked for "..", which is the
 * whole of the security model here -- this listens on a loopback-forwarded
 * port on a throwaway VM and is not a general-purpose web server. */
static void serve_file(int s, const char *path)
{
    char  full[768];
    char *body;
    int   fd;
    long  size;
    const char *ctype = "text/html; charset=utf-8";

    /* Bound it before it goes anywhere near a buffer: the path came off
     * the wire and can be as long as the request line. */
    if (strlen(path) > 200 || strstr(path, "..") != NULL) {
        send_simple(s, "400 Bad Request", "text/plain", "no", 2);
        return;
    }
    if (strcmp(path, "/") == 0)
        path = "/index.html";

    if (strstr(path, ".css"))      ctype = "text/css";
    else if (strstr(path, ".js"))  ctype = "application/javascript";

    snprintf(full, sizeof full, "%s%s", web_root, path);
    if ((fd = open(full, O_RDONLY)) < 0) {
        send_simple(s, "404 Not Found", "text/plain", "not found", 9);
        return;
    }
    size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (size < 0 || size > 4 * 1024 * 1024) {
        close(fd);
        send_simple(s, "500 Internal Server Error", "text/plain", "big", 3);
        return;
    }
    if ((body = malloc((size_t)size + 1)) == NULL) {
        close(fd);
        send_simple(s, "500 Internal Server Error", "text/plain", "mem", 3);
        return;
    }
    if (read(fd, body, (size_t)size) != size) {
        close(fd);
        free(body);
        send_simple(s, "500 Internal Server Error", "text/plain", "io", 2);
        return;
    }
    close(fd);
    send_simple(s, "200 OK", ctype, body, (int)size);
    free(body);
}

static long qparam(const char *q, const char *key, long dflt)
{
    const char *p = q ? strstr(q, key) : NULL;

    if (p == NULL)
        return dflt;
    p += strlen(key);
    if (*p != '=')
        return dflt;
    return strtol(p + 1, NULL, 10);
}

static void handle(int s, int fd_stats, char *json)
{
    char  req[2048];
    char *path, *query, *sp;
    int   n;

    n = (int)recv(s, req, sizeof req - 1, 0);
    if (n <= 0)
        return;
    req[n] = '\0';

    if (strncmp(req, "GET ", 4) != 0) {
        send_simple(s, "405 Method Not Allowed", "text/plain", "GET only", 8);
        return;
    }
    path = req + 4;
    if ((sp = strchr(path, ' ')) == NULL)
        return;
    *sp = '\0';

    query = strchr(path, '?');
    if (query)
        *query++ = '\0';

    if (strcmp(path, "/api/state") == 0) {
        int len = build_state(fd_stats, json, JBUF);
        send_simple(s, "200 OK", "application/json", json, len);
        return;
    }

    if (strcmp(path, "/api/config") == 0) {
        long d = qparam(query, "deadline_us", -1);
        long p = qparam(query, "period_us", -1);
        long f = qparam(query, "flood", -1);
        long r = qparam(query, "reset", -1);

        pthread_mutex_lock(&lock);
        if (d >= 100 && d <= 5000000)
            cfg_deadline_us = (uint32_t)d;
        if (p >= 10000 && p <= 2000000)
            cfg_period_us = (uint32_t)p;
        if (f == 0)
            flood_stop();
        else if (f == 1 && flood_start() != 0)
            fprintf(stderr, "triage_web: could not start chat_load\n");
        if (r == 1) {
            ring_n = 0;
            memset(ring, 0, sizeof ring);
        }
        pthread_mutex_unlock(&lock);

        if (r == 1 && fd_stats >= 0)
            triage_reset_stats(fd_stats);

        send_simple(s, "200 OK", "application/json", "{\"ok\":true}", 11);
        return;
    }

    serve_file(s, path);
}

/* ------------------------------------------------------------------ */

static void on_signal(int sig)
{
    (void)sig;
    running = 0;
    flood_stop();
    _exit(0);
}

static void usage(const char *a0)
{
    fprintf(stderr,
            "usage: %s [-p port] [-r webroot] [-D dev] [-P prio]\n"
            "          [-d deadline_us] [-i period_us] [-n chat_threads]\n"
            "  -p port       listen port (default %d)\n"
            "  -r webroot    directory holding index.html (default .)\n"
            "  -D dev        device to ask (default %s)\n"
            "  -P prio       priority the car client runs at (default 40)\n"
            "  -d deadline_us  starting deadline (default %u)\n"
            "  -i period_us    how often to ask (default %u)\n"
            "  -n threads      chat threads when the flood is on (default 4)\n",
            a0, DEF_PORT, TRIAGE_DEV_PATH, DEF_DEADLINE, DEF_PERIOD);
}

int main(int argc, char **argv)
{
    struct sockaddr_in addr;
    pthread_t          car;
    char              *json;
    int                port = DEF_PORT, srv, one = 1, c, fd_stats;

    cps_us = SYSPAGE_ENTRY(qtime)->cycles_per_sec / 1000000u;
    if (cps_us == 0)
        cps_us = 1;

    while ((c = getopt(argc, argv, "p:r:D:P:d:i:n:h")) != -1) {
        switch (c) {
        case 'p': port = atoi(optarg); break;
        case 'r': web_root = optarg; break;
        case 'D': dev_path = optarg; break;
        case 'P': cfg_prio = atoi(optarg); break;
        case 'd': cfg_deadline_us = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'i': cfg_period_us = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'n': flood_threads = atoi(optarg); break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    if ((json = malloc(JBUF)) == NULL) {
        perror("triage_web: malloc");
        return 1;
    }

    /* A second descriptor, for counters only. The car thread's fd is
     * busy being blocked in a devctl most of the time, and sharing it
     * would make the dashboard's refresh rate depend on inference
     * latency -- which is one of the things it is trying to display. */
    fd_stats = triage_open(dev_path);
    if (fd_stats < 0)
        fprintf(stderr, "triage_web: warning: no stats fd: %s\n",
                strerror(errno));

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if ((srv = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("triage_web: socket");
        return 1;
    }
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);

    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("triage_web: bind");
        return 1;
    }
    if (listen(srv, 16) < 0) {
        perror("triage_web: listen");
        return 1;
    }

    if (pthread_create(&car, NULL, car_thread, NULL) != 0) {
        perror("triage_web: pthread_create");
        return 1;
    }

    fprintf(stderr, "triage_web: serving %s on port %d, asking %s every "
                    "%uus at prio %d with a %uus deadline\n",
            web_root, port, dev_path ? dev_path : TRIAGE_DEV_PATH,
            cfg_period_us, cfg_prio, cfg_deadline_us);

    while (running) {
        int s = accept(srv, NULL, NULL);

        if (s < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        {
            /* Do not let one stalled client wedge the accept loop: this
             * server is single-threaded on purpose, because its other
             * thread is the priority-40 client and must not be competing
             * with HTTP work. */
            struct timeval tv = { 2, 0 };

            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        }
        handle(s, fd_stats, json);
        polite_close(s);
    }

    flood_stop();
    return 0;
}
