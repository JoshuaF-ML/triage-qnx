#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>

#include "engine.h"
#include "nn.h"

static uint64_t cycles_per_us = 1;

/*
 * Work units per microsecond of uncontended CPU, measured at startup.
 *
 * WHY THIS EXISTS, AND WHY THE OBVIOUS VERSION WAS WRONG
 * ------------------------------------------------------
 * The first version of this engine burned a chunk by spinning until the
 * wall clock had advanced by chunk_us. That is a plausible-looking stand-in
 * for inference and it is useless, for a reason worth recording.
 *
 * If a chunk spins until the wall clock says "chunk_us has passed", then
 * being descheduled mid-chunk does not make the chunk take longer -- it
 * makes the chunk do LESS WORK in the same wall time. Elapsed comes out at
 * chunk_us whether we had the CPU to ourselves or lost 90% of it to a more
 * urgent request. So:
 *
 *   - preempted and stolen_us could never be non-zero. They were not
 *     measuring zero preemption; they were incapable of measuring any.
 *   - low-priority work never actually slowed down under contention, so a
 *     benchmark against an FCFS baseline would show no cost to being
 *     preempted, and would prove nothing.
 *
 * A real model does a fixed amount of arithmetic per layer; losing the CPU
 * makes it finish later, not do less. So a chunk here is a fixed number of
 * work units, calibrated once. Wall-clock elapsed per chunk then exceeds
 * the nominal cost exactly when we were off the CPU -- which is what the
 * preemption accounting in run_request() reads.
 */
static uint64_t work_per_us = 0;

/* What a nominal 2000us burn actually measured on the final estimate. The
 * banner prints it: if it is not close to 2000, every timing this server
 * reports is scaled wrong and the operator should know before trusting a
 * benchmark. */
static uint64_t calib_check_us = 0;

/* The unit of pretend-inference: dependency-chained integer arithmetic, so
 * each step waits on the previous one and the cost per unit is stable. No
 * memory traffic, so timing does not depend on cache state, and nothing the
 * compiler can hoist out or vectorise away. */
static void burn_work(uint64_t units)
{
    static volatile uint64_t sink;
    uint64_t acc = sink | 1u;
    uint64_t i;

    for (i = 0; i < units; i++)
        acc = acc * 6364136223846793005ull + 1442695040888963407ull;

    sink = acc;
}

/* Time one burn of `units` work, in microseconds, taking the fastest of
 * `trials` runs.
 *
 * Fastest, not mean, and that is the whole point. A trial that got
 * interrupted reports a lower throughput; folding it in would inflate every
 * later chunk's nominal cost and make preemption look like the normal case.
 * The quickest trial is the one that most nearly had the CPU to itself, so
 * it is the best available estimate of uncontended throughput. */
static uint64_t best_burn_us(uint64_t units, int trials)
{
    uint64_t best = UINT64_MAX;
    int i;

    for (i = 0; i < trials; i++) {
        uint64_t t0 = (uint64_t)ClockCycles();
        uint64_t dt;

        burn_work(units);
        dt = (uint64_t)ClockCycles() - t0;

        if (dt > 0 && dt < best)
            best = dt;
    }

    if (best == UINT64_MAX)
        return 0;
    return best / cycles_per_us;
}

/*
 * Calibrate work_per_us by closed-loop refinement.
 *
 * A single open-loop probe is not good enough, and the first version of
 * this was wrong in a way worth recording. It timed one fixed batch of work
 * in the first milliseconds after startup and divided. The figure came out
 * roughly 2x too low, so every chunk afterwards did about half the work it
 * claimed: a tier with a 12 ms nominal cost finished in 5 ms. Harmless
 * here, because it made the deadline logic pessimistic about how much it
 * could fit -- but in the other direction the same error would silently
 * blow every deadline, and either way the benchmark numbers would be
 * fiction.
 *
 * The cause is that throughput at startup is not throughput later: the host
 * has not ramped the vCPU, caches are cold, and under a hypervisor the
 * first run of a loop is the slowest it will ever be. So instead of
 * trusting one measurement, aim at a known target and correct:
 *
 *   guess -> burn what should take TARGET_US -> measure -> rescale
 *
 * Each round starts from a warmer machine than the last, so the estimate
 * converges upward to the real rate rather than being anchored to the cold
 * one. Three rounds is comfortably enough; the loop stops early once the
 * error is under a few percent.
 */
static void calibrate(void)
{
    const uint64_t target_us = 2000u;   /* same order as a real chunk */
    int round;

    /* Seed: one coarse open-loop probe, just to get the magnitude right. */
    burn_work(200000u);                 /* warm i-cache, branch predictor */
    {
        uint64_t us = best_burn_us(200000u, 5);
        work_per_us = us ? 200000u / us : 1000u;
        if (work_per_us == 0)
            work_per_us = 1;
    }

    for (round = 0; round < 3; round++) {
        uint64_t units = target_us * work_per_us;
        uint64_t got   = best_burn_us(units, 5);
        uint64_t next;

        if (got == 0)
            break;
        calib_check_us = got;

        /* If that took `got` us instead of `target_us`, the true rate is
         * higher than we thought by exactly that ratio. */
        next = (work_per_us * target_us) / got;
        if (next == 0)
            next = 1;

        /* Within 3% -- stop; further rounds only chase noise. */
        if (got * 100 >= target_us * 97 && got * 100 <= target_us * 103) {
            work_per_us = next;
            return;
        }
        work_per_us = next;
    }

    if (work_per_us == 0) {
        fprintf(stderr, "triage: warning: calibration failed; timings suspect\n");
        work_per_us = 1000u;
    }
}

uint64_t engine_work_per_us(void)
{
    return work_per_us;
}

int engine_last_class(void)
{
    return nn_scan_best_class();
}

int engine_last_truth(void)
{
    return nn_scan_truth_class();
}

uint64_t engine_calib_check_us(void)
{
    return calib_check_us;
}

void engine_set_work_per_us(uint64_t units_per_us)
{
    if (units_per_us > 0) {
        work_per_us    = units_per_us;
        calib_check_us = 0;   /* 0 == not measured, it was handed to us */
    }
}

/*
 * Tier cost model.
 *
 * chunks x chunk_us is the nominal cost of a tier. The numbers are chosen
 * so that a 20 ms detection deadline can afford SMALL or MEDIUM but not
 * LARGE, and so chat tiers are an order of magnitude more expensive --
 * which is exactly the situation that makes first-come-first-served
 * inference dangerous.
 *
 * chunk_us is NOT the preemption granularity -- preemption is the kernel's
 * and is immediate at any chunk size. It is the granularity of our own
 * accounting: how often we re-check the deadline, and the resolution at
 * which stolen CPU time becomes visible. See docs/architecture.md.
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

    calibrate();

    /* Measures how long a patch of real inference takes at each tier,
     * which is where DETECT's tier costs and chunk sizes now come from. */
    nn_init();
}

uint64_t engine_now_us(void)
{
    return (uint64_t)ClockCycles() / cycles_per_us;
}

/*
 * Do the amount of work that takes `us` microseconds on an idle CPU.
 *
 * Note what this does NOT promise: that it returns in `us`. It returns when
 * the work is done, which is later than that if something more urgent ran.
 * That gap is the measurement the whole project rests on -- see work_per_us
 * above.
 */
void engine_burn_us(uint64_t us)
{
    burn_work(us * work_per_us);
}

static int valid(triage_task_t task, triage_tier_t tier)
{
    return task < TRIAGE_TASK__COUNT && tier < TRIAGE_TIER__COUNT;
}

/*
 * TARGET_CHUNK_US -- how long a chunk of real inference should take.
 *
 * This is the resolution of the deadline logic, so it wants to be small;
 * it is also the overhead of re-checking the clock and the cancel flag,
 * so it does not want to be tiny. 2ms is the figure architecture.md
 * argued for, and the sliding-window scan can hit it exactly by choosing
 * how many patches go in a chunk.
 */
#define TARGET_CHUNK_US 2000u

/*
 * DETECT is real inference now, so its shape is measured rather than
 * declared: a tier costs however long its arithmetic takes on this
 * machine. CHAT is still a calibrated load and still uses the table.
 *
 * This is a better arrangement than the six hand-chosen numbers it
 * replaces. Those numbers had to be right for the deadline story to make
 * sense, and nothing checked them.
 */
unsigned engine_chunks(triage_task_t task, triage_tier_t tier)
{
    if (!valid(task, tier))
        return 0;
    if (task == TRIAGE_TASK_DETECT) {
        unsigned patches = nn_patches(tier);
        unsigned per     = nn_patches_per_chunk(tier, TARGET_CHUNK_US);

        if (patches == 0 || per == 0)
            return 0;
        return (patches + per - 1u) / per;     /* ceil */
    }
    return shapes[task][tier].chunks;
}

uint64_t engine_chunk_us(triage_task_t task, triage_tier_t tier)
{
    if (!valid(task, tier))
        return 0;
    if (task == TRIAGE_TASK_DETECT)
        return nn_patches_per_chunk(tier, TARGET_CHUNK_US)
               * nn_us_per_patch(tier);
    return shapes[task][tier].chunk_us;
}

uint64_t engine_nominal_us(triage_task_t task, triage_tier_t tier)
{
    if (!valid(task, tier))
        return 0;
    if (task == TRIAGE_TASK_DETECT)
        return (uint64_t)nn_patches(tier) * nn_us_per_patch(tier);
    return (uint64_t)shapes[task][tier].chunks * shapes[task][tier].chunk_us;
}

/*
 * The fastest a chunk of this tier has ever actually completed, in us.
 *
 * WHY A MEASURED FLOOR AND NOT THE NOMINAL NUMBER
 * -----------------------------------------------
 * Preemption is detected by noticing that a chunk took longer than it
 * should have. That needs a trustworthy "should have", and the nominal
 * chunk_us is not one. Measured on the QEMU target, the same chunk's
 * uncontended cost varied by more than 2x across a single run: the host is
 * a laptop running Windows, WSL2 and a hypervisor, and it does not hand out
 * CPU at a constant rate. Against a fixed nominal baseline that variance
 * both hides real preemptions and manufactures fake ones, which would make
 * every preemption number in the benchmark worthless.
 *
 * So the baseline is observed rather than declared: the minimum wall time
 * this chunk has ever taken is the best evidence available of what it costs
 * when nobody is competing for the CPU. It can only ratchet downward, and
 * it converges within the first few requests.
 *
 * Seeded with the nominal cost so the first request is not wild, and
 * floored at 1us so the comparison in run_request() cannot divide the
 * machine's noise into infinity.
 *
 * Concurrency: pool threads update this without a lock. The only possible
 * race is two threads both lowering the same floor at once and one update
 * being lost, which costs us a slightly high floor for one more chunk. A
 * mutex on the measurement path would be a worse trade.
 */
static uint64_t chunk_floor_us[TRIAGE_TASK__COUNT][TRIAGE_TIER__COUNT];

uint64_t engine_chunk_floor_us(triage_task_t task, triage_tier_t tier)
{
    uint64_t f;

    if (!valid(task, tier))
        return 1;
    f = chunk_floor_us[task][tier];
    if (f == 0)
        f = shapes[task][tier].chunk_us;   /* not measured yet */
    return f ? f : 1;
}

uint64_t engine_run_chunk(triage_task_t task, triage_tier_t tier)
{
    uint64_t t0, elapsed;

    if (!valid(task, tier))
        return 0;

    t0 = engine_now_us();
    if (task == TRIAGE_TASK_DETECT)
        nn_scan_step(nn_patches_per_chunk(tier, TARGET_CHUNK_US));
    else
        engine_burn_us(shapes[task][tier].chunk_us);
    elapsed = engine_now_us() - t0;

    if (elapsed > 0) {
        uint64_t cur = chunk_floor_us[task][tier];
        if (cur == 0 || elapsed < cur)
            chunk_floor_us[task][tier] = elapsed;
    }

    return elapsed;
}

/* CHAT has no trained model yet, so its confidence is still a declared
 * figure rather than a measured one. DETECT no longer uses this table:
 * it reports nn_accuracy(), which came off the MNIST test set. */
static const uint32_t tier_confidence[TRIAGE_TIER__COUNT] = {
    0, 720, 880, 960
};

/*
 * Start a request.
 *
 * For DETECT this picks a frame and resets the sliding-window scan, which
 * is what lets engine_run_chunk() be stateless from the caller's point of
 * view -- the scan lives in thread-local state in nn.c, and a pool thread
 * serves one request at a time.
 *
 * The frame is chosen from the payload so that the same question gets the
 * same frame: a client asking repeatedly gets a reproducible answer, which
 * makes an on-target accuracy test possible.
 */
void engine_begin_request(triage_task_t task, triage_tier_t tier,
                          const char *payload, uint32_t payload_len)
{
    uint32_t h = 2166136261u;
    uint32_t i;

    if (task != TRIAGE_TASK_DETECT)
        return;

    for (i = 0; i < payload_len; i++)   /* FNV-1a, for a stable choice */
        h = (h ^ (uint32_t)(unsigned char)payload[i]) * 16777619u;

    nn_scan_begin(tier, h);
}

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
        /*
         * The real detection result: what the network found, where it was,
         * how many of the frame's windows got scanned before time ran
         * out, and the ground truth.
         *
         * The truth is in the reply on purpose. It is what lets a client
         * score the server instead of believing it, and it is how the
         * on-target accuracy figure is produced -- the same principle as
         * putting both priorities in every reply.
         */
        n = snprintf(out, out_sz,
                     "class=%d truth=%d conf=%u.%u%% at=%d,%d "
                     "scanned=%u/%u tier=%s",
                     nn_scan_best_class(), nn_scan_truth_class(),
                     nn_scan_best_score() / 10, nn_scan_best_score() % 10,
                     nn_scan_best_x(), nn_scan_best_y(),
                     nn_scan_done_patches(), nn_patches(tier),
                     engine_tier_name(tier));
    } else {
        n = snprintf(out, out_sz,
                     "[%s model] answer to \"%s\"",
                     engine_tier_name(tier), safe);
    }

    *out_len = (n < 0) ? 0 : (uint32_t)((size_t)n < out_sz ? (size_t)n : out_sz - 1);
    /*
     * Confidence for DETECT is the tier's MEASURED accuracy on the MNIST
     * test set -- 93.6%, 95.3%, 97.3% -- and not the three numbers that
     * used to live in a table here. That is the difference between
     * claiming a bigger model is worth paying for and showing it.
     *
     * CHAT has no model yet, so it keeps the declared figure. The README
     * says which is which.
     */
    if (task == TRIAGE_TASK_DETECT)
        *confidence = nn_accuracy(tier);
    else
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
