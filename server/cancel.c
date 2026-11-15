#include <pthread.h>
#include <string.h>

#include "cancel.h"

/*
 * One slot per request that can be in flight, which is bounded by the
 * thread pool's maximum. 64 is comfortably above it.
 *
 * A flat array with a mutex rather than anything cleverer: the operations
 * are register, unregister, test and mark, all O(small), and two of them
 * sit on the per-chunk path where predictability matters more than
 * theoretical scalability. A lock-free structure here would be harder to
 * reason about for no measurable gain on a single core.
 */
#define SLOTS 64

static struct {
    int rcvid;
    int used;
    int cancelled;
} slots[SLOTS];

static pthread_mutex_t   lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t          total_cancelled;

int cancel_register(int rcvid)
{
    int i, found = -1;

    pthread_mutex_lock(&lock);
    for (i = 0; i < SLOTS; i++) {
        if (!slots[i].used) {
            slots[i].used      = 1;
            slots[i].rcvid     = rcvid;
            slots[i].cancelled = 0;
            found = i;
            break;
        }
    }
    pthread_mutex_unlock(&lock);
    return found;
}

void cancel_unregister(int slot)
{
    if (slot < 0 || slot >= SLOTS)
        return;
    pthread_mutex_lock(&lock);
    slots[slot].used      = 0;
    slots[slot].rcvid     = 0;
    slots[slot].cancelled = 0;
    pthread_mutex_unlock(&lock);
}

int cancel_requested(int slot)
{
    int v;

    if (slot < 0 || slot >= SLOTS)
        return 0;
    pthread_mutex_lock(&lock);
    v = slots[slot].cancelled;
    pthread_mutex_unlock(&lock);
    return v;
}

int cancel_mark(int rcvid)
{
    int i, hit = 0;

    pthread_mutex_lock(&lock);
    for (i = 0; i < SLOTS; i++) {
        if (slots[i].used && slots[i].rcvid == rcvid) {
            if (!slots[i].cancelled) {
                slots[i].cancelled = 1;
                total_cancelled++;
            }
            hit = 1;
            break;
        }
    }
    pthread_mutex_unlock(&lock);
    return hit;
}

uint64_t cancel_total(void)
{
    uint64_t v;

    pthread_mutex_lock(&lock);
    v = total_cancelled;
    pthread_mutex_unlock(&lock);
    return v;
}
