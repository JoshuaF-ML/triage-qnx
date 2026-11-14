#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include <devctl.h>

#include "triage.h"

static const char *tier_names[TRIAGE_TIER__COUNT] = {
    "none", "small", "medium", "large"
};
static const char *task_names[TRIAGE_TASK__COUNT] = {
    "detect", "chat"
};

int triage_open(const char *path)
{
    return open(path ? path : TRIAGE_DEV_PATH, O_RDWR);
}

int triage_close(int fd)
{
    return close(fd);
}

/*
 * One devctl() is the entire round trip. Under the hood QNX turns this
 * into MsgSend() to the resource manager and blocks this thread until the
 * reply comes back -- which is also the moment our priority is handed to
 * the thread doing the work on the other side.
 */
int triage_infer(int fd, const triage_request_t *req, triage_reply_t *rep)
{
    triage_xfer_t xfer;
    int rc;

    if (req == NULL || rep == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(&xfer, 0, sizeof xfer);
    xfer.req = *req;
    xfer.req.abi_version = TRIAGE_ABI_VERSION;

    rc = devctl(fd, TRIAGE_DCMD_INFER, &xfer, sizeof xfer, NULL);
    if (rc != EOK) {
        errno = rc;
        return -1;
    }

    *rep = xfer.rep;
    if (rep->abi_version != TRIAGE_ABI_VERSION) {
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}

int triage_ask_tier(int fd, triage_task_t task, const char *prompt,
                    uint32_t deadline_us, triage_tier_t tier,
                    uint32_t flags, triage_reply_t *rep)
{
    triage_request_t req;
    size_t len;

    memset(&req, 0, sizeof req);
    req.abi_version = TRIAGE_ABI_VERSION;
    req.task        = (uint32_t)task;
    req.deadline_us = deadline_us;
    req.want_tier   = (uint32_t)tier;
    req.flags       = flags;

    if (prompt != NULL) {
        len = strlen(prompt);
        if (len > TRIAGE_PAYLOAD_MAX - 1)
            len = TRIAGE_PAYLOAD_MAX - 1;
        memcpy(req.payload, prompt, len);
        req.payload_len = (uint32_t)len;
    }

    return triage_infer(fd, &req, rep);
}

int triage_ask(int fd, triage_task_t task, const char *prompt,
               uint32_t deadline_us, triage_reply_t *rep)
{
    return triage_ask_tier(fd, task, prompt, deadline_us,
                           TRIAGE_TIER_NONE, 0, rep);
}

int triage_stats(int fd, triage_stats_t *stats)
{
    int rc = devctl(fd, TRIAGE_DCMD_STATS, stats, sizeof *stats, NULL);

    if (rc != EOK) {
        errno = rc;
        return -1;
    }
    return 0;
}

int triage_reset_stats(int fd)
{
    int rc = devctl(fd, TRIAGE_DCMD_RESET, NULL, 0, NULL);

    if (rc != EOK) {
        errno = rc;
        return -1;
    }
    return 0;
}

int triage_set_priority(int prio)
{
    struct sched_param param;
    int policy;

    if (pthread_getschedparam(pthread_self(), &policy, &param) != EOK)
        return -1;
    param.sched_priority = prio;
    if (pthread_setschedparam(pthread_self(), policy, &param) != EOK)
        return -1;
    return 0;
}

int triage_get_priority(void)
{
    struct sched_param param;
    int policy;

    if (pthread_getschedparam(pthread_self(), &policy, &param) != EOK)
        return -1;
    return param.sched_priority;
}

const char *triage_tier_name(triage_tier_t tier)
{
    return tier < TRIAGE_TIER__COUNT ? tier_names[tier] : "?";
}

const char *triage_task_name(triage_task_t task)
{
    return task < TRIAGE_TASK__COUNT ? task_names[task] : "?";
}
