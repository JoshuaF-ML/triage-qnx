#include <stdio.h>
#include <string.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>

#include "engine.h"

static uint64_t cycles_per_us = 1;

/*
 * Tier cost model.
 *
 * chunks x chunk_us is the nominal cost of a tier. The numbers are chosen
 * so that a 20 ms detection deadline can afford SMALL or MEDIUM but not
 * LARGE, and so chat tiers are an order of magnitude more expensive --
 * which is exactly the situation that makes first-come-first-served
 * inference dangerous.
 *
 * chunk_us is also the preemption granularity: a request can only be
 * interrupted by our own accounting at a chunk boundary, so keeping
 * chunk_us small (~1-2 ms) is what makes "it preempts mid-inference" an
 * honest claim rather than a slogan. See docs/architecture.md.
 */
typedef struct {
    unsigned chunks;
    uint64_t chunk_us;
} tier_shape_t;

static const tier_shape_t shapes[TRIAGE_TASK__COUNT][TRIAGE_TIER__COUNT] = {
    /* TRIAGE_TASK_DETECT */
    {
        { 0,  0     },   /* NONE   */
        { 5,  1000  },   /* SMALL  ->   5 ms */
        { 6,  2000  },   /* MEDIUM ->  12 ms */
        { 13, 2000  },   /* LARGE  ->  26 ms */
    },
    /* TRIAGE_TASK_CHAT */
    {
        { 0,  0     },   /* NONE   */
        { 10, 4000  },   /* SMALL  ->  40 ms */
        { 24, 5000  },   /* MEDIUM -> 120 ms */
        { 30, 10000 },   /* LARGE  -> 300 ms */
    },
};

static const char *tier_names[TRIAGE_TIER__COUNT] = {
    "none", "small", "medium", "large"
};
static const char *task_names[TRIAGE_TASK__COUNT] = {
    "detect", "chat"
};

void engine_init(void)
{
    uint64_t cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;

    if (cps < 1000000u) {
        /* Implausible; fall back to something that will not divide by
         * zero. Timings will be wrong and the log says so. */
        fprintf(stderr, "triage: warning: cycles_per_sec=%llu looks wrong\n",
                (unsigned long long)cps);
        cps = 1000000u;
    }
    cycles_per_us = cps / 1000000u;
    if (cycles_per_us == 0)
        cycles_per_us = 1;
}

uint64_t engine_now_us(void)
{
    return (uint64_t)ClockCycles() / cycles_per_us;
}

void engine_burn_us(uint64_t us)
{
    uint64_t end = (uint64_t)ClockCycles() + us * cycles_per_us;
    volatile uint64_t sink = 0;

    /* A tight loop that the compiler cannot elide. Checking the cycle
     * counter every iteration is itself work, which is fine: we are
     * standing in for inference, not measuring the loop. */
    while ((uint64_t)ClockCycles() < end)
        sink += 1;
    (void)sink;
}

static int valid(triage_task_t task, triage_tier_t tier)
{
    return task < TRIAGE_TASK__COUNT && tier < TRIAGE_TIER__COUNT;
}

unsigned engine_chunks(triage_task_t task, triage_tier_t tier)
{
    return valid(task, tier) ? shapes[task][tier].chunks : 0;
}

uint64_t engine_chunk_us(triage_task_t task, triage_tier_t tier)
{
    return valid(task, tier) ? shapes[task][tier].chunk_us : 0;
}

uint64_t engine_nominal_us(triage_task_t task, triage_tier_t tier)
{
    if (!valid(task, tier))
        return 0;
    return (uint64_t)shapes[task][tier].chunks * shapes[task][tier].chunk_us;
}

void engine_run_chunk(triage_task_t task, triage_tier_t tier)
{
    if (!valid(task, tier))
        return;
    engine_burn_us(shapes[task][tier].chunk_us);
}

static const uint32_t tier_confidence[TRIAGE_TIER__COUNT] = {
    0, 720, 880, 960
};

void engine_answer(triage_task_t task, triage_tier_t tier,
                   const char *prompt, uint32_t prompt_len,
                   char *out, size_t out_sz, uint32_t *out_len,
                   uint32_t *confidence)
{
    char safe[TRIAGE_PAYLOAD_MAX];
    int n;

    if (tier >= TRIAGE_TIER__COUNT)
        tier = TRIAGE_TIER_SMALL;

    if (prompt_len >= sizeof safe)
        prompt_len = sizeof safe - 1;
    memcpy(safe, prompt, prompt_len);
    safe[prompt_len] = '\0';

    if (task == TRIAGE_TASK_DETECT) {
        n = snprintf(out, out_sz,
                     "pedestrian=%s conf=%u.%u%% tier=%s q=\"%s\"",
                     tier >= TRIAGE_TIER_MEDIUM ? "yes" : "likely",
                     tier_confidence[tier] / 10, tier_confidence[tier] % 10,
                     engine_tier_name(tier), safe);
    } else {
        n = snprintf(out, out_sz,
                     "[%s model] answer to \"%s\"",
                     engine_tier_name(tier), safe);
    }

    *out_len = (n < 0) ? 0 : (uint32_t)((size_t)n < out_sz ? (size_t)n : out_sz - 1);
    *confidence = (tier < TRIAGE_TIER__COUNT) ? tier_confidence[tier] : 0;
}

const char *engine_tier_name(triage_tier_t tier)
{
    return tier < TRIAGE_TIER__COUNT ? tier_names[tier] : "?";
}

const char *engine_task_name(triage_task_t task)
{
    return task < TRIAGE_TASK__COUNT ? task_names[task] : "?";
}
