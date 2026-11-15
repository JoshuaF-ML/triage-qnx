/*
 * fcfs.c -- the baseline: an inference server that does what everyone
 *           else's inference server does.
 *
 * WHY THIS EXISTS
 * ---------------
 * Triage claims that making AI inference a QNX resource manager, so that
 * urgency crosses the process boundary through the kernel, protects
 * deadlines that a conventional inference server cannot. A claim like that
 * is worth nothing without something to compare against, and the
 * comparison is only worth anything if the thing being compared against is
 * the strongest honest version of the conventional approach rather than a
 * straw man built to lose.
 *
 * So this is not "Triage with the good parts removed to make a point". It
 * is the same engine, the same transport, the same protocol, the same
 * thread counts, the same preemption accounting and the same timing code.
 * Every difference is deliberate, is listed below, and corresponds to
 * something a real server genuinely does differently.
 *
 * WHAT IS DIFFERENT, AND WHY EACH ONE IS FAIR
 * -------------------------------------------
 * 1. NO PRIORITY INHERITANCE. The channel is created WITH
 *    _NTO_CHF_FIXED_PRIORITY, the one flag Triage deliberately omits. Its
 *    receive threads therefore run at this server's own priority instead of
 *    the caller's.
 *
 *    This is the honest mapping of a socket-based server onto QNX, not a
 *    handicap. llama.cpp's server, Triton, and every HTTP model endpoint
 *    receive bytes, not messages from threads: there is no caller priority
 *    for them to inherit even in principle, and their worker threads run at
 *    whatever priority the process was started at. This flag reproduces
 *    exactly that condition while holding everything else identical, which
 *    is what makes the experiment a controlled one.
 *
 * 2. FIRST COME, FIRST SERVED. In queue mode requests are served in strict
 *    arrival order, by one worker, because a conventional server has one
 *    model instance and queues requests against it. The kernel would
 *    otherwise hand us the highest-priority waiting message first -- QNX
 *    queues blocked senders by priority -- so getting true FCFS takes
 *    explicit work: receive, defer with _RESMGR_NOREPLY, and reply from the
 *    worker later. Without that the baseline would quietly inherit a
 *    scheduling advantage it has not earned and the comparison would
 *    understate the difference.
 *
 * 3. NO DEADLINE AWARENESS. The deadline in the request is recorded and
 *    reported, never acted on. One fixed tier runs to completion every
 *    time. No tier selection, no interference model, no stopping early,
 *    no degrading. This is the part of Triage that has no counterpart in
 *    a standard server at all.
 *
 * TWO MODES, BECAUSE ONE WOULD CONFLATE TWO EFFECTS
 * -------------------------------------------------
 *   -m queue  (default) One worker, strict arrival order. Models a real
 *             deployment: a single model instance with a request queue.
 *             Differs from Triage in BOTH concurrency and priority.
 *
 *   -m pool   The same thread pool as Triage, same size, requests handled
 *             concurrently -- but with _NTO_CHF_FIXED_PRIORITY set and no
 *             deadline logic. Concurrency is held constant, so whatever
 *             this mode loses by is attributable to priority inheritance
 *             and tier selection alone.
 *
 * Reporting both is the point. Queue mode answers "how much better is
 * Triage than what people actually deploy"; pool mode answers "how much of
 * that is the mechanism rather than the thread count". A single number
 * could not separate them, and a reader is entitled to ask.
 *
 * The fixed tier matters for the same reason and is settable with -k, so
 * the harness can run the baseline at the tier Triage picks when idle
 * (medium) and at the one it degrades to under load (small). If Triage
 * beats the baseline even at small, the win is scheduling rather than
 * simply running a cheaper model.
 *
 * Evidence of QNX use in this file:
 *   dispatch_create_channel()  with _NTO_CHF_FIXED_PRIORITY -- the single
 *                              flag that turns inheritance off
 *   _RESMGR_NOREPLY            deferring a reply to implement a real queue
 *   MsgReply()                 completing a devctl from another thread
 */
#define THREAD_POOL_PARAM_T dispatch_context_t

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>

#include "engine.h"
#include "triage_proto.h"

#define FCFS_DEV_PATH  "/dev/fcfs"

enum { MODE_QUEUE = 0, MODE_POOL = 1 };

static resmgr_connect_funcs_t connect_funcs;
static resmgr_io_funcs_t      io_funcs;
static iofunc_attr_t          dev_attr;

static int verbose;
static int mode     = MODE_QUEUE;
static int srv_prio = 10;   /* QNX's default priority */

/*
 * The one tier this server runs, per task. Per-task rather than global
 * because a conventional deployment does have a separate detector model
 * and chat model at separate sizes -- and because the global version
 * confounded the experiment.
 *
 * Running everything at `small` to see whether Triage's win was "just a
 * cheaper model" made the chat flood cheaper too, which drains the queue
 * faster and helps detection for a completely unrelated reason. With
 * per-task tiers the harness can run the baseline at exactly the tiers
 * Triage chose -- small for detection, large for chat -- leaving
 * scheduling as the only remaining difference. See bench/run.sh.
 */
static triage_tier_t fixed_tier[TRIAGE_TASK__COUNT] = {
    TRIAGE_TIER_MEDIUM,   /* detect */
    TRIAGE_TIER_MEDIUM,   /* chat   */
};

static triage_tier_t tier_for(triage_task_t task)
{
    return task < TRIAGE_TASK__COUNT ? fixed_tier[task] : TRIAGE_TIER_SMALL;
}

/* ------------------------------------------------------------------ */
/* Stats. Same struct as Triage so triage_stat works against either    */
/* server unchanged -- one less thing that could differ between the     */
/* two sides of the comparison.                                        */
/* ------------------------------------------------------------------ */
static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;
static triage_stats_t  stats;
static unsigned long long rejected;   /* queue was full */

static void stats_init(void)
{
    triage_task_t t;
    triage_tier_t k;
    unsigned      b;

    pthread_mutex_lock(&stats_lock);
    memset(&stats, 0, sizeof stats);
    stats.min_prio_seen = -1;
    stats.max_prio_seen = -1;
    for (t = 0; t < TRIAGE_TASK__COUNT; t++)
        for (k = 0; k < TRIAGE_TIER__COUNT; k++)
            stats.est_us[t][k] = engine_nominal_us(t, k);
    /* Left at 100 and never updated: modelling interference is precisely
     * what this server does not do. */
    for (b = 0; b < TRIAGE_PRIO_BANDS; b++)
        stats.infl_pct[b] = 100u;
    pthread_mutex_unlock(&stats_lock);
}

static void stats_record(triage_task_t task, const triage_reply_t *rep)
{
    triage_tier_t tier = (triage_tier_t)rep->tier_used;
    unsigned      band = TRIAGE_PRIO_BAND(rep->client_prio);

    pthread_mutex_lock(&stats_lock);
    stats.requests++;
    if (rep->deadline_met)
        stats.deadlines_met++;
    else
        stats.deadlines_missed++;
    stats.preemptions += rep->preempted;
    stats.stolen_us   += rep->stolen_us;
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
    pthread_mutex_unlock(&stats_lock);
}

/* ------------------------------------------------------------------ */
/* The compute path                                                    */
/* ------------------------------------------------------------------ */

/*
 * Run the fixed tier to completion.
 *
 * Note what is missing compared with Triage's run_request(): there is no
 * deadline re-check between chunks and no early stop. The chunk loop is
 * kept only so that preemption accounting is measured the same way on both
 * sides -- if the baseline reported stolen time differently from Triage,
 * no comparison of stolen time would mean anything.
 */
static void run_fixed(const triage_request_t *req, triage_reply_t *rep,
                      uint64_t arrival_us)
{
    triage_task_t task = (triage_task_t)req->task;
    triage_tier_t tier = tier_for(task);
    uint64_t      start_us, now_us;
    unsigned      total_chunks, i;

    total_chunks = engine_chunks(task, tier);

    start_us      = engine_now_us();
    rep->queue_us = start_us - arrival_us;

    for (i = 0; i < total_chunks; i++) {
        uint64_t elapsed  = engine_run_chunk(task, tier);
        uint64_t floor_us = engine_chunk_floor_us(task, tier);

        if (elapsed > floor_us + floor_us / 2) {
            rep->preempted++;
            rep->stolen_us += elapsed - floor_us;
        }
        rep->chunks++;
    }

    now_us          = engine_now_us();
    rep->compute_us = now_us - start_us;
    rep->total_us   = now_us - arrival_us;
    rep->tier_used  = tier;
    rep->degraded   = 0;    /* never: we do not know how to degrade */

    /* The deadline is measured but was never an input to anything. That
     * is the entire difference this benchmark exists to quantify. */
    rep->deadline_met =
        (req->deadline_us == 0 || rep->total_us <= req->deadline_us);

    engine_answer(task, tier, req->payload, req->payload_len,
                  rep->result, sizeof rep->result,
                  &rep->result_len, &rep->confidence);
}

static void log_request(const triage_request_t *req, const triage_reply_t *rep)
{
    fprintf(stderr,
            "fcfs: %-6s prio=%d/%d deadline=%6uus tier=%-6s "
            "queue=%6lluus compute=%6lluus total=%6lluus "
            "chunks=%2u preempt=%u stolen=%lluus %s\n",
            engine_task_name((triage_task_t)req->task),
            rep->client_prio, rep->server_prio,
            req->deadline_us,
            engine_tier_name((triage_tier_t)rep->tier_used),
            (unsigned long long)rep->queue_us,
            (unsigned long long)rep->compute_us,
            (unsigned long long)rep->total_us,
            rep->chunks, rep->preempted,
            (unsigned long long)rep->stolen_us,
            rep->deadline_met ? "MET" : "MISS");
}

static int my_prio(void)
{
    struct sched_param param;
    int                policy = 0;

    if (pthread_getschedparam(pthread_self(), &policy, &param) == EOK)
        return param.sched_priority;
    return -1;
}

/*
 * Force this thread back to the server's own priority.
 *
 * A belt to the channel flag's braces. _NTO_CHF_FIXED_PRIORITY should mean
 * a receiving thread is never boosted in the first place, and when it
 * works this function finds nothing to do. It exists because getting that
 * flag wrong is silent -- it cost a whole invalid benchmark run once -- and
 * because a conventional inference server genuinely does decide its own
 * worker priority, so doing it explicitly is not a cheat.
 *
 * Returns the priority we ended up at, which goes into the reply as
 * server_prio so a client can check it rather than trust it.
 */
static int pin_prio(void)
{
    struct sched_param param;
    int                policy = 0;

    if (pthread_getschedparam(pthread_self(), &policy, &param) != EOK)
        return -1;
    if (param.sched_priority != srv_prio) {
        param.sched_priority = srv_prio;
        if (pthread_setschedparam(pthread_self(), policy, &param) != EOK)
            return my_prio();
    }
    return my_prio();
}

/* ------------------------------------------------------------------ */
/* The FCFS queue                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    int              rcvid;
    triage_request_t req;
    uint64_t         arrival_us;
    int32_t          client_prio;
} job_t;

/*
 * Bounded, like any real server's request queue. When it fills, requests
 * are rejected with EAGAIN rather than queued without limit -- which is
 * what a conventional server does too, and refusing to model that would
 * flatter the baseline by giving it infinite buffering.
 */
#define QUEUE_MAX 512
static job_t           q[QUEUE_MAX];
static unsigned        q_head, q_count;
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  q_cv   = PTHREAD_COND_INITIALIZER;

static int queue_push(const job_t *j)
{
    pthread_mutex_lock(&q_lock);
    if (q_count == QUEUE_MAX) {
        rejected++;
        pthread_mutex_unlock(&q_lock);
        return -1;
    }
    q[(q_head + q_count) % QUEUE_MAX] = *j;
    q_count++;
    pthread_cond_signal(&q_cv);
    pthread_mutex_unlock(&q_lock);
    return 0;
}

static void queue_pop(job_t *out)
{
    pthread_mutex_lock(&q_lock);
    while (q_count == 0)
        pthread_cond_wait(&q_cv, &q_lock);
    *out = q[q_head];
    q_head = (q_head + 1) % QUEUE_MAX;
    q_count--;
    pthread_mutex_unlock(&q_lock);
}

/*
 * Complete a deferred devctl by hand.
 *
 * The resource manager framework normally builds this reply for us when
 * io_devctl returns _RESMGR_PTR. Having deferred with _RESMGR_NOREPLY, we
 * owe the client the same two-part message ourselves: the devctl reply
 * header followed by the payload.
 */
static void reply_job(int rcvid, const triage_reply_t *rep)
{
    struct {
        struct _io_devctl_reply o;
        triage_reply_t          data;
    } r;

    memset(&r, 0, sizeof r);
    r.o.nbytes = sizeof *rep;
    r.data     = *rep;

    if (MsgReply(rcvid, EOK, &r, sizeof r.o + sizeof *rep) == -1)
        MsgError(rcvid, errno);
}

static void *worker(void *arg)
{
    (void)arg;

    for (;;) {
        job_t          j;
        triage_reply_t rep;

        queue_pop(&j);

        memset(&rep, 0, sizeof rep);
        rep.abi_version = TRIAGE_ABI_VERSION;
        rep.client_prio = j.client_prio;
        /* The honest number, and the one that differs from Triage: this
         * worker runs at the server's priority, whoever asked. */
        rep.server_prio = pin_prio();
        rep.infl_pct    = 100u;
        rep.prio_band   = TRIAGE_PRIO_BAND(j.client_prio);
        rep.est_used_us = (uint32_t)engine_nominal_us(
                              (triage_task_t)j.req.task,
                              tier_for((triage_task_t)j.req.task));

        run_fixed(&j.req, &rep, j.arrival_us);
        stats_record((triage_task_t)j.req.task, &rep);

        if (verbose)
            log_request(&j.req, &rep);

        reply_job(j.rcvid, &rep);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */

static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg,
                     RESMGR_OCB_T *ocb)
{
    uint64_t arrival_us = engine_now_us();
    int      status;

    if ((status = iofunc_devctl_default(ctp, msg, ocb)) != _RESMGR_DEFAULT)
        return status;

    switch (msg->i.dcmd) {
    case TRIAGE_DCMD_INFER: {
        triage_request_t req;
        triage_reply_t   rep;

        if (msg->i.nbytes < sizeof req)
            return EINVAL;
        memcpy(&req, _DEVCTL_DATA(msg->i), sizeof req);

        if (req.abi_version != TRIAGE_ABI_VERSION)
            return ENOTSUP;
        if (req.task >= TRIAGE_TASK__COUNT)
            return EINVAL;
        if (req.payload_len > TRIAGE_PAYLOAD_MAX)
            return EINVAL;

        if (mode == MODE_QUEUE) {
            job_t j;

            memset(&j, 0, sizeof j);
            j.rcvid       = ctp->rcvid;
            j.req         = req;
            j.arrival_us  = arrival_us;
            j.client_prio = ctp->info.priority;

            if (queue_push(&j) != 0)
                return EAGAIN;

            /* Deferred. The worker owes this client a reply, in the order
             * the request arrived -- which is the whole point. */
            return _RESMGR_NOREPLY;
        }

        /* MODE_POOL: handle it right here, concurrently, exactly as
         * Triage does -- but on a fixed-priority channel and with no
         * deadline logic. */
        memset(&rep, 0, sizeof rep);
        rep.abi_version = TRIAGE_ABI_VERSION;
        rep.client_prio = ctp->info.priority;
        /* Pinned, not merely read: see pin_prio(). In pool mode this is
         * the thread that will do the work, so if the channel flag failed
         * to prevent a boost, this is where it gets undone -- before any
         * of the compute happens, not after. */
        rep.server_prio = pin_prio();
        rep.infl_pct    = 100u;
        rep.prio_band   = TRIAGE_PRIO_BAND(rep.client_prio);
        rep.est_used_us = (uint32_t)engine_nominal_us(
                              (triage_task_t)req.task,
                              tier_for((triage_task_t)req.task));

        run_fixed(&req, &rep, arrival_us);
        stats_record((triage_task_t)req.task, &rep);

        if (verbose)
            log_request(&req, &rep);

        memset(&msg->o, 0, sizeof msg->o);
        msg->o.nbytes = sizeof rep;
        memcpy(_DEVCTL_DATA(msg->o), &rep, sizeof rep);
        return _RESMGR_PTR(ctp, msg, sizeof msg->o + sizeof rep);
    }

    case TRIAGE_DCMD_STATS: {
        triage_stats_t st;

        pthread_mutex_lock(&stats_lock);
        st = stats;
        pthread_mutex_unlock(&stats_lock);

        memset(&msg->o, 0, sizeof msg->o);
        msg->o.nbytes = sizeof st;
        memcpy(_DEVCTL_DATA(msg->o), &st, sizeof st);
        return _RESMGR_PTR(ctp, msg, sizeof msg->o + sizeof st);
    }

    case TRIAGE_DCMD_RESET:
        stats_init();
        memset(&msg->o, 0, sizeof msg->o);
        msg->o.nbytes = 0;
        return _RESMGR_PTR(ctp, msg, sizeof msg->o);

    default:
        return ENOTTY;
    }
}

static void on_signal(int sig)
{
    triage_stats_t st;

    pthread_mutex_lock(&stats_lock);
    st = stats;
    pthread_mutex_unlock(&stats_lock);

    fprintf(stderr,
            "\nfcfs: caught signal %d\n"
            "fcfs: mode=%s tier=detect:%s/chat:%s prio=%d requests=%llu "
            "met=%llu missed=%llu preemptions=%llu worst_total=%lluus "
            "rejected=%llu\n",
            sig,
            mode == MODE_QUEUE ? "queue" : "pool",
            engine_tier_name(fixed_tier[TRIAGE_TASK_DETECT]),
            engine_tier_name(fixed_tier[TRIAGE_TASK_CHAT]), srv_prio,
            (unsigned long long)st.requests,
            (unsigned long long)st.deadlines_met,
            (unsigned long long)st.deadlines_missed,
            (unsigned long long)st.preemptions,
            (unsigned long long)st.worst_total_us,
            rejected);
    _exit(0);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [-p path] [-m queue|pool] [-k tier] [-P prio]\n"
            "          [-t threads] [-v]\n"
            "  -p path     device path to register (default %s)\n"
            "  -m mode     queue = one worker, strict arrival order (default)\n"
            "              pool  = same thread pool as Triage, fixed priority\n"
            "  -k spec     the one tier this server runs, as <tier> for all\n"
            "              tasks or <detect>,<chat> to set them separately:\n"
            "              e.g. -k medium  or  -k small,large (default medium)\n"
            "  -P prio     priority the server's threads run at (default 10,\n"
            "              which is QNX's default priority)\n"
            "  -t threads  max pool threads in pool mode (default 16)\n"
            "  -W units    work units per microsecond, overriding the\n"
            "              startup calibration. The benchmark MUST pass the\n"
            "              same value here as it gives Triage: otherwise the\n"
            "              two servers do different amounts of arithmetic\n"
            "              for the same nominal tier and the comparison is\n"
            "              not controlled. See engine_set_work_per_us().\n"
            "  -v          log every request to stderr\n",
            argv0, FCFS_DEV_PATH);
}

static int parse_tier(const char *s, triage_tier_t *out)
{
    if (strcmp(s, "small") == 0)       *out = TRIAGE_TIER_SMALL;
    else if (strcmp(s, "medium") == 0) *out = TRIAGE_TIER_MEDIUM;
    else if (strcmp(s, "large") == 0)  *out = TRIAGE_TIER_LARGE;
    else return -1;
    return 0;
}

/* "medium" sets both tasks; "small,large" sets detect and chat apart. */
static int parse_tier_spec(const char *spec)
{
    char          buf[64];
    char         *comma;
    triage_tier_t a, b;

    if (strlen(spec) >= sizeof buf)
        return -1;
    strcpy(buf, spec);

    if ((comma = strchr(buf, ',')) != NULL) {
        *comma = '\0';
        if (parse_tier(buf, &a) != 0 || parse_tier(comma + 1, &b) != 0)
            return -1;
        fixed_tier[TRIAGE_TASK_DETECT] = a;
        fixed_tier[TRIAGE_TASK_CHAT]   = b;
        return 0;
    }

    if (parse_tier(buf, &a) != 0)
        return -1;
    fixed_tier[TRIAGE_TASK_DETECT] = a;
    fixed_tier[TRIAGE_TASK_CHAT]   = a;
    return 0;
}

int main(int argc, char **argv)
{
    const char        *path        = FCFS_DEV_PATH;
    int                max_threads = 16;
    int                c, id;
    dispatch_t        *dpp;
    resmgr_attr_t      resmgr_attr;
    thread_pool_attr_t pool_attr;
    thread_pool_t     *tpp;
    struct sched_param param;
    int                policy = 0;
    int                chid;

    engine_init();

    while ((c = getopt(argc, argv, "p:m:k:P:t:W:vh")) != -1) {
        switch (c) {
        case 'p': path = optarg; break;
        case 'W': engine_set_work_per_us(strtoull(optarg, NULL, 10)); break;
        case 'm':
            if (strcmp(optarg, "queue") == 0)      mode = MODE_QUEUE;
            else if (strcmp(optarg, "pool") == 0)  mode = MODE_POOL;
            else { usage(argv[0]); return 1; }
            break;
        case 'k':
            if (parse_tier_spec(optarg) != 0) {
                usage(argv[0]);
                return 1;
            }
            break;
        case 'P': srv_prio = atoi(optarg); break;
        case 't': max_threads = atoi(optarg); break;
        case 'v': verbose = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }
    if (max_threads < 2)
        max_threads = 2;

    stats_init();

    /*
     * Run at a fixed priority of our own choosing, like any ordinary
     * server. Everything this process does afterwards -- receiving,
     * queueing, computing -- inherits this priority, including threads
     * created later.
     */
    /* Change the priority only, keeping whatever policy we inherited.
     * Switching policy as well would be a second uncontrolled difference
     * from Triage, and this experiment is supposed to have one. */
    if (pthread_getschedparam(pthread_self(), &policy, &param) != EOK) {
        perror("fcfs: pthread_getschedparam");
        return 1;
    }
    param.sched_priority = srv_prio;
    if (pthread_setschedparam(pthread_self(), policy, &param) != EOK) {
        fprintf(stderr, "fcfs: cannot set priority %d: %s\n"
                        "  (lowering is free; raising needs "
                        "PROCMGR_AID_PRIORITY -- run as root)\n",
                srv_prio, strerror(errno));
        return 1;
    }

    /*
     * THE ONE FLAG. _NTO_CHF_FIXED_PRIORITY tells the kernel not to hand a
     * receiving thread the priority of the thread whose message it just
     * took. Triage's dispatch_create() omits it, which is why Triage gets
     * inheritance for free; setting it here is what makes this server
     * behave like a conventional one, and it is the single variable this
     * whole comparison turns on.
     *
     * It has to go on the CHANNEL, through ChannelCreate, not on the
     * dispatch handle. The first version of this passed
     * dispatch_create_channel(-1, _NTO_CHF_FIXED_PRIORITY), which compiles,
     * runs, and is wrong: that function's flags argument takes
     * DISPATCH_FLAG_* values, and _NTO_CHF_FIXED_PRIORITY is 0x0001, the
     * same value as DISPATCH_FLAG_NOLOCK. The same number means two
     * different things in two namespaces and the compiler cannot tell.
     *
     * So the baseline silently ran with inheritance still enabled and
     * promptly "beat" Triage in the benchmark, 100% to 99%. The only
     * reason that was caught is that brake_reflex checks the priority pair
     * in every reply: a server whose entire purpose is not to inherit
     * priority reported 0 of 120 requests not inherited. Reporting the
     * evidence rather than asserting it paid for itself here.
     *
     * The remaining flags are the ones a resource manager's channel needs,
     * which dispatch_create() would otherwise have set for us.
     */
    /*
     * Which companion flags a resource manager's channel will accept is
     * not something to guess at: the full set above is rejected with
     * EINVAL on QNX 8.0. So try progressively smaller sets and say which
     * one the kernel took. _NTO_CHF_FIXED_PRIORITY is in every candidate
     * and is never the thing dropped -- without it this program is not a
     * baseline and should not run at all.
     */
    {
        static const unsigned candidates[] = {
            _NTO_CHF_FIXED_PRIORITY | _NTO_CHF_UNBLOCK |
                _NTO_CHF_DISCONNECT | _NTO_CHF_COID_DISCONNECT |
                _NTO_CHF_SENDER_LEN | _NTO_CHF_REPLY_LEN,
            _NTO_CHF_FIXED_PRIORITY | _NTO_CHF_UNBLOCK |
                _NTO_CHF_DISCONNECT | _NTO_CHF_COID_DISCONNECT |
                _NTO_CHF_SENDER_LEN,
            _NTO_CHF_FIXED_PRIORITY | _NTO_CHF_UNBLOCK |
                _NTO_CHF_DISCONNECT | _NTO_CHF_COID_DISCONNECT,
            _NTO_CHF_FIXED_PRIORITY | _NTO_CHF_UNBLOCK |
                _NTO_CHF_DISCONNECT,
            _NTO_CHF_FIXED_PRIORITY | _NTO_CHF_DISCONNECT,
            _NTO_CHF_FIXED_PRIORITY,
        };
        size_t n = sizeof candidates / sizeof candidates[0];
        size_t k;

        chid = -1;
        for (k = 0; k < n; k++) {
            chid = ChannelCreate(candidates[k]);
            if (chid != -1) {
                fprintf(stderr, "fcfs: channel flags 0x%04x accepted\n",
                        candidates[k]);
                break;
            }
        }
        if (chid == -1) {
            perror("fcfs: ChannelCreate (no flag set accepted)");
            return 1;
        }
    }

    if ((dpp = dispatch_create_channel(chid, 0)) == NULL) {
        perror("fcfs: dispatch_create_channel");
        return 1;
    }

    memset(&resmgr_attr, 0, sizeof resmgr_attr);
    resmgr_attr.nparts_max   = 1;
    resmgr_attr.msg_max_size = sizeof(io_devctl_t) + sizeof(triage_xfer_t) + 64;

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs,
                     _RESMGR_IO_NFUNCS, &io_funcs);
    io_funcs.devctl = io_devctl;

    iofunc_attr_init(&dev_attr, S_IFCHR | 0666, NULL, NULL);

    id = resmgr_attach(dpp, &resmgr_attr, path, _FTYPE_ANY, 0,
                       &connect_funcs, &io_funcs, &dev_attr);
    if (id == -1) {
        perror("fcfs: resmgr_attach");
        return 1;
    }

    if (mode == MODE_QUEUE) {
        pthread_t tid;

        if (pthread_create(&tid, NULL, worker, NULL) != 0) {
            perror("fcfs: pthread_create");
            return 1;
        }
    }

    /*
     * In queue mode the pool only receives and enqueues, so it is never
     * the bottleneck; the single worker is, deliberately. In pool mode it
     * does the compute too, with the same sizing as Triage.
     */
    memset(&pool_attr, 0, sizeof pool_attr);
    pool_attr.handle        = dpp;
    pool_attr.context_alloc = dispatch_context_alloc;
    pool_attr.block_func    = dispatch_block;
    pool_attr.handler_func  = dispatch_handler;
    pool_attr.context_free  = dispatch_context_free;
    pool_attr.lo_water      = 2;
    pool_attr.hi_water      = (unsigned)max_threads / 2;
    pool_attr.increment     = 1;
    pool_attr.maximum       = (unsigned)max_threads;

    if ((tpp = thread_pool_create(&pool_attr, POOL_FLAG_USE_SELF)) == NULL) {
        perror("fcfs: thread_pool_create");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    fprintf(stderr,
            "fcfs: serving %s  mode=%s prio=%d\n"
            "fcfs: calibrated at %llu work units/us\n"
            "fcfs: tiers detect=%s chat=%s\n"
            "fcfs: no priority inheritance, no deadline awareness"
            " (this is the baseline)\n",
            path,
            mode == MODE_QUEUE ? "queue (1 worker, FCFS)" : "pool (concurrent)",
            srv_prio,
            (unsigned long long)engine_work_per_us(),
            engine_tier_name(fixed_tier[TRIAGE_TASK_DETECT]),
            engine_tier_name(fixed_tier[TRIAGE_TASK_CHAT]));

    thread_pool_start(tpp);
    return 0;
}
