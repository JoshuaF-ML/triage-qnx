/*
 * engine.h -- the compute side of Triage.
 *
 * The engine is deliberately behind a narrow interface so the inference
 * backend can be swapped without touching the resource manager or the
 * scheduler. Today it is a calibrated CPU load that performs a known
 * amount of arithmetic per chunk; week 1's open question is which real
 * runtime replaces it (llama.cpp for CHAT, a small detector for DETECT).
 *
 * The one property the rest of Triage depends on is CHUNKING: a tier's
 * work is executed as a sequence of bounded chunks, so between any two
 * chunks we can re-read the clock, notice we were preempted, and decide
 * whether the remaining work still fits the deadline.
 */
#ifndef TRIAGE_ENGINE_H_
#define TRIAGE_ENGINE_H_

#include <stdint.h>
#include <stddef.h>
#include "triage_proto.h"

/* Reads the system page for the cycle counter rate. Call once at startup. */
void     engine_init(void);

/* Monotonic microseconds from the hardware cycle counter (ClockCycles). */
uint64_t engine_now_us(void);

/* Do the amount of work that takes `us` microseconds on an idle CPU, at the
 * caller's priority. CPU-bound on purpose: a sleeping thread cannot
 * demonstrate preemption, and real inference is CPU-bound too.
 *
 * It does NOT return in `us` -- it returns when the work is finished, which
 * is later if something more urgent ran. A fixed amount of work rather than
 * a fixed amount of wall time is what makes stolen CPU measurable at all;
 * engine.c explains why the reverse silently defeats the measurement. */
void     engine_burn_us(uint64_t us);

/* Work units per microsecond of uncontended CPU, measured by engine_init.
 * Exposed for the startup banner, so a nonsense calibration is visible. */
uint64_t engine_work_per_us(void);

/*
 * Override the measured calibration with an explicit figure.
 *
 * This exists for the benchmark, and it is not a convenience -- without it
 * the comparison against bench/fcfs.c is not controlled. Each server
 * process calibrates itself at startup, and calibration on this target
 * varies by over 2x. Two servers started minutes apart therefore do
 * DIFFERENT amounts of arithmetic for the same nominal tier cost, so a
 * difference in their deadline hit rates might be nothing but a difference
 * in how much work each decided a millisecond was worth.
 *
 * bench/run.sh measures once and passes the same value to every server, so
 * that a "12 ms tier" is literally the same arithmetic on both sides.
 */
void engine_set_work_per_us(uint64_t units_per_us);

/* What the last calibration round measured for a burn that should have
 * taken 2000us. Far from 2000 means every timing here is scaled wrong. */
uint64_t engine_calib_check_us(void);

/* Shape of a tier's work. */
unsigned engine_chunks(triage_task_t task, triage_tier_t tier);
uint64_t engine_chunk_us(triage_task_t task, triage_tier_t tier);
uint64_t engine_nominal_us(triage_task_t task, triage_tier_t tier);

/* Run one chunk of a tier's work. Returns the wall-clock microseconds it
 * took, which is the figure the preemption accounting compares against the
 * floor below -- not the nominal chunk cost. */
uint64_t engine_run_chunk(triage_task_t task, triage_tier_t tier);

/* The fastest this tier's chunk has ever actually completed, in us: the
 * measured cost of the chunk when nothing was competing for the CPU.
 * Seeded from the nominal cost, ratchets down as evidence arrives.
 *
 * Preemption detection needs a trustworthy "how long should this have
 * taken". On real hardware under a hypervisor the nominal number is not
 * one -- engine.c records how far off it was measured to be. */
uint64_t engine_chunk_floor_us(triage_task_t task, triage_tier_t tier);

/* Fill in the answer text and a confidence in 0..1000 for a completed
 * request. Higher tiers are more confident -- that is the whole point of
 * paying for them. */
void     engine_answer(triage_task_t task, triage_tier_t tier,
                       const char *prompt, uint32_t prompt_len,
                       char *out, size_t out_sz, uint32_t *out_len,
                       uint32_t *confidence);

const char *engine_tier_name(triage_tier_t tier);
const char *engine_task_name(triage_task_t task);

#endif /* TRIAGE_ENGINE_H_ */
