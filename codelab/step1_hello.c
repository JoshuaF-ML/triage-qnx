/*
 * step1_hello.c -- the smallest useful QNX resource manager.
 *
 * Registers /dev/clab1. Any process can now open() it and ask it a
 * question with devctl(). There is no socket, no port number, no daemon
 * protocol, no serialisation library: the request is a struct, and it
 * arrives in the server's address space because the kernel put it there.
 *
 * This file is step 1 of the Triage codelab. It is ~70 lines of code and
 * every one of them is load-bearing; the rest of the codelab adds one
 * idea at a time to exactly this skeleton.
 *
 *   qcc -Vgcc_ntox86_64 -o step1_hello step1_hello.c
 *   ./step1_hello &
 *   ls -l /dev/clab1
 *   ./ask -D /dev/clab1
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>

#include "clab.h"

static resmgr_connect_funcs_t connect_funcs;
static resmgr_io_funcs_t      io_funcs;
static iofunc_attr_t          dev_attr;

/*
 * One request. This is the entire server.
 *
 * iofunc_devctl_default() first, always: it handles the devctl commands
 * every device is expected to answer (things like "are you a tty?") and
 * returns _RESMGR_DEFAULT to mean "not mine, it must be yours".
 */
static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg,
                     RESMGR_OCB_T *ocb)
{
    clab_msg_t m;
    int        status;

    status = iofunc_devctl_default(ctp, msg, ocb);
    if (status != _RESMGR_DEFAULT)
        return status;

    if (msg->i.dcmd != CLAB_DCMD_ASK)
        return ENOTTY;                 /* not a command we know */
    if (msg->i.nbytes < sizeof m)
        return EINVAL;

    /* Copy the request out BEFORE writing the reply: msg->i and msg->o
     * are a union, so filling in the reply destroys the request. */
    memcpy(&m, _DEVCTL_DATA(msg->i), sizeof m);

    m.client_prio = -1;
    m.server_prio = -1;
    m.served_us   = 0;
    snprintf(m.text, sizeof m.text, "hello from a resource manager");

    memset(&msg->o, 0, sizeof msg->o);
    msg->o.nbytes = sizeof m;
    memcpy(_DEVCTL_DATA(msg->o), &m, sizeof m);
    return _RESMGR_PTR(ctp, msg, sizeof msg->o + sizeof m);
}

int main(void)
{
    dispatch_t       *dpp;
    dispatch_context_t *ctp;
    resmgr_attr_t     resmgr_attr;
    int               id;

    if ((dpp = dispatch_create()) == NULL) {
        perror("step1: dispatch_create");
        return 1;
    }

    memset(&resmgr_attr, 0, sizeof resmgr_attr);
    resmgr_attr.nparts_max = 1;
    /* Must be big enough for the devctl header AND our struct, or the
     * kernel truncates the message and nbytes arrives short. */
    resmgr_attr.msg_max_size = sizeof(io_devctl_t) + sizeof(clab_msg_t) + 64;

    /* Fill both function tables with the library's defaults, then
     * override only what we actually implement. Everything else keeps
     * working -- stat(), open(), close() -- for free. */
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs,
                     _RESMGR_IO_NFUNCS, &io_funcs);
    io_funcs.devctl = io_devctl;

    iofunc_attr_init(&dev_attr, S_IFCHR | 0666, NULL, NULL);

    id = resmgr_attach(dpp, &resmgr_attr, "/dev/clab1", _FTYPE_ANY, 0,
                       &connect_funcs, &io_funcs, &dev_attr);
    if (id == -1) {
        perror("step1: resmgr_attach");
        return 1;
    }

    if ((ctp = dispatch_context_alloc(dpp)) == NULL) {
        perror("step1: dispatch_context_alloc");
        return 1;
    }

    fprintf(stderr, "step1: serving /dev/clab1\n");

    /* Single-threaded receive loop: block for a message, handle it,
     * repeat. Step 3 is about why this is not enough. */
    for (;;) {
        if (dispatch_block(ctp) == NULL) {
            perror("step1: dispatch_block");
            return 1;
        }
        dispatch_handler(ctp);
    }
}
