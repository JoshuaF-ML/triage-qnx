/*
 * step2_prio.c -- where the whole project comes from.
 *
 * Step 1's server answered. This one answers with two numbers:
 *
 *   ctp->info.priority     the priority of the thread that SENT us this
 *                          message -- the client's urgency
 *   pthread_getschedparam  the priority THIS thread is running at, right
 *                          now, while handling that message
 *
 * They are the same number. We did not make them the same. We did not
 * read one and apply the other. QNX made them equal when it delivered the
 * message, because a synchronous MsgSend() on a channel created without
 * _NTO_CHF_FIXED_PRIORITY runs the receiver at the sender's priority.
 *
 * Registers /dev/clab2.
 *
 *   ./step2_prio &
 *   ./ask -D /dev/clab2 -P 40
 *   ./ask -D /dev/clab2 -P 12
 *
 * Then break it on purpose, which is the point of this step:
 *
 *   ./step2_prio -f &
 *   ./ask -D /dev/clab2 -P 40 -f
 *
 * -f creates the channel WITH _NTO_CHF_FIXED_PRIORITY. One flag, and the
 * server now runs every client's work at its own priority instead. That
 * single flag is the difference between Triage and a conventional
 * inference server, and it is the variable the benchmark isolates.
 */
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

#include "clab.h"

static resmgr_connect_funcs_t connect_funcs;
static resmgr_io_funcs_t      io_funcs;
static iofunc_attr_t          dev_attr;

static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg,
                     RESMGR_OCB_T *ocb)
{
    clab_msg_t         m;
    struct sched_param param;
    int                policy = 0, status;

    status = iofunc_devctl_default(ctp, msg, ocb);
    if (status != _RESMGR_DEFAULT)
        return status;
    if (msg->i.dcmd != CLAB_DCMD_ASK)
        return ENOTTY;
    if (msg->i.nbytes < sizeof m)
        return EINVAL;

    memcpy(&m, _DEVCTL_DATA(msg->i), sizeof m);

    /* THE EVIDENCE, both halves of it. */
    m.client_prio = ctp->info.priority;
    m.server_prio = (pthread_getschedparam(pthread_self(), &policy, &param)
                     == EOK) ? param.sched_priority : -1;
    m.served_us   = 0;
    snprintf(m.text, sizeof m.text, "served at %d", m.server_prio);

    memset(&msg->o, 0, sizeof msg->o);
    msg->o.nbytes = sizeof m;
    memcpy(_DEVCTL_DATA(msg->o), &m, sizeof m);
    return _RESMGR_PTR(ctp, msg, sizeof msg->o + sizeof m);
}

/*
 * Make the channel with _NTO_CHF_FIXED_PRIORITY -- the experiment.
 *
 * Read this part even if you never want fixed priority, because getting
 * it wrong costs a day.
 *
 * The flag goes on the CHANNEL, via ChannelCreate(). It CANNOT go through
 * dispatch_create_channel(-1, _NTO_CHF_FIXED_PRIORITY): that call's flags
 * argument takes DISPATCH_FLAG_* values, and _NTO_CHF_FIXED_PRIORITY is
 * 0x0001 -- the same value as DISPATCH_FLAG_NOLOCK. Two namespaces, one
 * number, and the compiler cannot tell them apart. It compiles, it runs,
 * and inheritance stays on.
 *
 * In this project that mistake made the benchmark's no-inheritance
 * baseline quietly inherit priority and "beat" Triage 100% to 99%. It was
 * caught only because the client asserts on the priority pair.
 *
 * Which companion flags a resource manager's channel will accept is also
 * not guessable: the full set is rejected with EINVAL on QNX 8.0. So try
 * decreasing sets and print the one the kernel took. _NTO_CHF_FIXED_PRIORITY
 * is in every candidate and is never what gets dropped -- without it this
 * mode is not doing what it says.
 */
static dispatch_t *make_fixed_priority_channel(void)
{
    static const unsigned candidates[] = {
        _NTO_CHF_FIXED_PRIORITY | _NTO_CHF_UNBLOCK | _NTO_CHF_DISCONNECT |
            _NTO_CHF_COID_DISCONNECT | _NTO_CHF_SENDER_LEN,
        _NTO_CHF_FIXED_PRIORITY | _NTO_CHF_UNBLOCK | _NTO_CHF_DISCONNECT |
            _NTO_CHF_COID_DISCONNECT,
        _NTO_CHF_FIXED_PRIORITY | _NTO_CHF_DISCONNECT,
        _NTO_CHF_FIXED_PRIORITY,
    };
    size_t k, n = sizeof candidates / sizeof candidates[0];
    int    chid = -1;

    for (k = 0; k < n; k++) {
        chid = ChannelCreate(candidates[k]);
        if (chid != -1) {
            fprintf(stderr, "step2: channel flags 0x%04x accepted\n",
                    candidates[k]);
            break;
        }
    }
    if (chid == -1) {
        perror("step2: ChannelCreate (no flag set accepted)");
        return NULL;
    }
    return dispatch_create_channel(chid, 0);
}

int main(int argc, char **argv)
{
    int                 fixed = 0, c, id;
    dispatch_t         *dpp;
    dispatch_context_t *ctp;
    resmgr_attr_t       resmgr_attr;

    while ((c = getopt(argc, argv, "fh")) != -1) {
        switch (c) {
        case 'f': fixed = 1; break;
        default:
            fprintf(stderr, "usage: %s [-f]\n"
                            "  -f  create the channel WITH "
                            "_NTO_CHF_FIXED_PRIORITY (no inheritance)\n",
                    argv[0]);
            return c == 'h' ? 0 : 1;
        }
    }

    /* dispatch_create() makes the channel without _NTO_CHF_FIXED_PRIORITY.
     * That omission is the entire mechanism. Do not "fix" it. */
    dpp = fixed ? make_fixed_priority_channel() : dispatch_create();
    if (dpp == NULL) {
        perror("step2: dispatch_create");
        return 1;
    }

    memset(&resmgr_attr, 0, sizeof resmgr_attr);
    resmgr_attr.nparts_max   = 1;
    resmgr_attr.msg_max_size = sizeof(io_devctl_t) + sizeof(clab_msg_t) + 64;

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs,
                     _RESMGR_IO_NFUNCS, &io_funcs);
    io_funcs.devctl = io_devctl;

    iofunc_attr_init(&dev_attr, S_IFCHR | 0666, NULL, NULL);

    id = resmgr_attach(dpp, &resmgr_attr, "/dev/clab2", _FTYPE_ANY, 0,
                       &connect_funcs, &io_funcs, &dev_attr);
    if (id == -1) {
        perror("step2: resmgr_attach");
        return 1;
    }

    if ((ctp = dispatch_context_alloc(dpp)) == NULL) {
        perror("step2: dispatch_context_alloc");
        return 1;
    }

    fprintf(stderr, "step2: serving /dev/clab2, priority %s\n",
            fixed ? "FIXED (no inheritance)" : "inherited");

    for (;;) {
        if (dispatch_block(ctp) == NULL) {
            perror("step2: dispatch_block");
            return 1;
        }
        dispatch_handler(ctp);
    }
}
