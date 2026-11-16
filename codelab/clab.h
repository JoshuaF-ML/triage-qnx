/*
 * clab.h -- the whole wire protocol for the codelab, in one struct.
 *
 * Triage's real protocol (common/triage_proto.h) has versioning, a
 * payload, tiers, deadlines and twenty fields of telemetry. None of that
 * is needed to see the mechanism, and all of it is in the way while you
 * are learning it, so the codelab uses this instead.
 *
 * One struct is used for BOTH directions. __DIOTF means "transfer both
 * ways": the client's copy goes to the server, the server overwrites it,
 * and the same buffer comes back. That is why there is no separate reply
 * type -- and also why, in the server, msg->i and msg->o overlap and you
 * must copy the request out before touching the reply.
 */
#ifndef CLAB_H
#define CLAB_H

#include <stdint.h>
#include <devctl.h>

typedef struct {
    /* in */
    uint32_t burn_us;      /* how long the server should pretend to think */
    /* out */
    int32_t  client_prio;  /* the priority the CALLER was running at */
    int32_t  server_prio;  /* the priority the SERVER thread ran at */
    uint64_t served_us;    /* how long the server spent on this request */
    char     text[48];     /* the "answer" */
} clab_msg_t;

/*
 * __DIOTF(class, code, type) builds a devctl command that transfers `type`
 * in both directions. _DCMD_MISC is the class for "my own thing"; the code
 * is ours to pick. Both sides must agree on all three or the server sees a
 * command it does not recognise and returns ENOTTY -- which is the single
 * most common way a first resource manager appears to be broken.
 */
#define CLAB_DCMD_ASK  __DIOTF(_DCMD_MISC, 1, clab_msg_t)

#endif /* CLAB_H */
