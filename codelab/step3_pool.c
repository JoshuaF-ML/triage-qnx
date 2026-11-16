/*
 * step3_pool.c -- the two things inheritance needs in order to matter.
 *
 * Step 2 proved that the thread handling your message runs at your
 * priority. That says nothing about WHEN your message gets handled, and
 * this step is about the two separate ways it can fail to get handled at
 * all. Both of them are reachable with a flag here, so you can measure
 * each one instead of believing it.
 *
 * 1. ONE RECEIVE THREAD IS ONE RECEIVE THREAD (-t).
 *    If the only thread is busy computing somebody else's 3-second
 *    request, your urgent message sits on the channel untouched. The
 *    kernel cannot promote a thread that is not yet handling your
 *    message: inheritance has nothing to inherit onto. So: a thread pool.
 *
 * 2. THE FRAMEWORK SERIALISES YOUR HANDLERS FOR YOU (-u).
 *    This is the one that is worth the price of the codelab, because it
 *    is invisible and it is the default. iofunc_func_init() installs
 *    iofunc_lock_ocb_default() / iofunc_unlock_ocb_default(), and the
 *    resource manager framework calls them around EVERY I/O message.
 *    They lock the device attribute -- which every client shares, because
 *    they all opened the same device. However many pool threads you
 *    created, exactly one io_devctl() runs at a time.
 *
 *    Measured here, 8 pool threads, urgent worst case against a lazy
 *    3-second request:
 *
 *        -t 8        2 008 783 us        (the pool buys nothing)
 *        -t 8 -u         1 355 us
 *        -t 1 -u     2 009 864 us        (the lock is not the only issue)
 *
 *    Clearing those two hooks is safe for a device like this one, and the
 *    reason is specific rather than general: it has no file position, no
 *    size, no read/write path and no mutable attribute state. Every
 *    request is a self-contained devctl. A resource manager that served
 *    real file contents would need that lock and would have to find its
 *    concurrency somewhere else.
 *
 * This bug was in Triage itself for weeks, and the project's own
 * telemetry could not see it -- the server's latency measurement starts
 * at the top of the handler, which is AFTER the wait. A server cannot
 * measure the time it made you wait before it knew you existed.
 *
 *   ./step3_pool -t 8 &                        # the serialised default
 *   ./ask -D /dev/clab3 -P 40 -w 1000 -n 4 -W 5000000 -q &
 *   ./ask -D /dev/clab3 -P 10 -w 3000000 -n 4 -q
 *
 * Registers /dev/clab3. verify.sh runs all three cases and asserts on
 * the result.
 *
 * Note THREAD_POOL_PARAM_T below. QNX's thread pool is generic over the
 * type its callbacks take; it defaults to resmgr_context_t, but we hand
 * the pool the dispatch_* functions, which deal in dispatch_context_t.
 * Defining this before the includes is the documented way to line the
 * prototypes up. Without it, the pool_attr assignments in main() are
 * incompatible-pointer errors whose message does not mention threads.
 */
#define THREAD_POOL_PARAM_T dispatch_context_t

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>

#include "clab.h"

static resmgr_connect_funcs_t connect_funcs;
static resmgr_io_funcs_t      io_funcs;
static iofunc_attr_t          dev_attr;

static uint64_t cps_us = 1;
static int      verbose;

static uint64_t now_us(void)
{
    return (uint64_t)ClockCycles() / cps_us;
}

/*
 * Pretend to think, by BURNING CPU rather than sleeping.
 *
 * This matters more than it looks. A sleeping thread gives the CPU up, so
 * a sleeping "slow request" would never block anything and step 3 would
 * appear to pass with one thread. Only work that actually holds the CPU
 * shows you what your scheduling does.
 *
 * (In Triage proper this is server/engine.c, which burns a fixed amount of
 * WORK rather than a fixed amount of wall-clock time -- see the codelab
 * text for why spinning to a deadline makes preemption impossible to
 * measure.)
 */
static void burn_us(uint32_t us)
{
    uint64_t until = now_us() + us;
    volatile uint64_t x = 1;

    while (now_us() < until) {
        int i;
        for (i = 0; i < 1000; i++)
            x = x * 6364136223846793005ull + 1442695040888963407ull;
    }
    (void)x;
}

static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg,
                     RESMGR_OCB_T *ocb)
{
    clab_msg_t         m;
    struct sched_param param;
    int                policy = 0, status;
    uint64_t           t0;

    status = iofunc_devctl_default(ctp, msg, ocb);
    if (status != _RESMGR_DEFAULT)
        return status;
    if (msg->i.dcmd != CLAB_DCMD_ASK)
        return ENOTTY;
    if (msg->i.nbytes < sizeof m)
        return EINVAL;

    memcpy(&m, _DEVCTL_DATA(msg->i), sizeof m);

    m.client_prio = ctp->info.priority;
    m.server_prio = (pthread_getschedparam(pthread_self(), &policy, &param)
                     == EOK) ? param.sched_priority : -1;

    t0 = now_us();
    burn_us(m.burn_us);
    m.served_us = now_us() - t0;

    snprintf(m.text, sizeof m.text, "thought for %lluus at prio %d",
             (unsigned long long)m.served_us, m.server_prio);

    if (verbose)
        fprintf(stderr, "step3: tid=%d prio=%d/%d asked=%uus took=%lluus\n",
                (int)pthread_self(), m.client_prio, m.server_prio, m.burn_us,
                (unsigned long long)m.served_us);

    memset(&msg->o, 0, sizeof msg->o);
    msg->o.nbytes = sizeof m;
    memcpy(_DEVCTL_DATA(msg->o), &m, sizeof m);
    return _RESMGR_PTR(ctp, msg, sizeof msg->o + sizeof m);
}

int main(int argc, char **argv)
{
    int                threads = 8, nolock = 0, c, id;
    dispatch_t        *dpp;
    resmgr_attr_t      resmgr_attr;
    thread_pool_attr_t pool_attr;
    thread_pool_t     *tpp;

    cps_us = SYSPAGE_ENTRY(qtime)->cycles_per_sec / 1000000u;
    if (cps_us == 0)
        cps_us = 1;

    while ((c = getopt(argc, argv, "t:uvh")) != -1) {
        switch (c) {
        case 't': threads = atoi(optarg); break;
        case 'u': nolock = 1; break;
        case 'v': verbose = 1; break;
        default:
            fprintf(stderr,
                    "usage: %s [-t threads] [-u] [-v]\n"
                    "  -t threads  max pool threads (default 8)\n"
                    "  -u          clear lock_ocb/unlock_ocb so requests\n"
                    "              are not serialised by the framework\n"
                    "  -v          log every request\n", argv[0]);
            return c == 'h' ? 0 : 1;
        }
    }
    if (threads < 1)
        threads = 1;

    if ((dpp = dispatch_create()) == NULL) {
        perror("step3: dispatch_create");
        return 1;
    }

    memset(&resmgr_attr, 0, sizeof resmgr_attr);
    resmgr_attr.nparts_max   = 1;
    resmgr_attr.msg_max_size = sizeof(io_devctl_t) + sizeof(clab_msg_t) + 64;

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs,
                     _RESMGR_IO_NFUNCS, &io_funcs);
    io_funcs.devctl = io_devctl;

    /*
     * -u: stop the framework serialising our handlers. See the top of
     * this file -- this is two lines and it is worth a factor of 1480
     * here. Note which way round the default is: what you get by writing
     * the obvious thing is the SLOW one.
     */
    if (nolock) {
        io_funcs.lock_ocb   = NULL;
        io_funcs.unlock_ocb = NULL;
    }

    iofunc_attr_init(&dev_attr, S_IFCHR | 0666, NULL, NULL);

    id = resmgr_attach(dpp, &resmgr_attr, "/dev/clab3", _FTYPE_ANY, 0,
                       &connect_funcs, &io_funcs, &dev_attr);
    if (id == -1) {
        perror("step3: resmgr_attach");
        return 1;
    }

    /*
     * The pool. lo_water/hi_water/increment are how many threads sit
     * waiting; `maximum` is the one that matters here, because it is the
     * number of requests that can be IN FLIGHT at once. At maximum=1 this
     * is step 1's receive loop again, with all of step 1's blocking.
     */
    memset(&pool_attr, 0, sizeof pool_attr);
    pool_attr.handle        = dpp;
    pool_attr.context_alloc = dispatch_context_alloc;
    pool_attr.block_func    = dispatch_block;
    pool_attr.handler_func  = dispatch_handler;
    pool_attr.context_free  = dispatch_context_free;
    pool_attr.lo_water      = (threads > 1) ? 2 : 1;
    pool_attr.hi_water      = (threads > 1) ? (unsigned)threads / 2 : 1;
    pool_attr.increment     = 1;
    pool_attr.maximum       = (unsigned)threads;

    if ((tpp = thread_pool_create(&pool_attr, POOL_FLAG_USE_SELF)) == NULL) {
        perror("step3: thread_pool_create");
        return 1;
    }

    fprintf(stderr, "step3: serving /dev/clab3, max %d pool threads, "
                    "handlers %s\n",
            threads, nolock ? "concurrent" : "serialised by the framework");

    thread_pool_start(tpp);   /* does not return */
    return 0;
}
