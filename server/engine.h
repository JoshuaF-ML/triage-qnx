/*
 * engine.h -- the compute side of Triage.
 *
 * The engine is deliberately behind a narrow interface so the inference
 * backend can be swapped without touching the resource manager or the
 * scheduler. Today it is a calibrated CPU load that burns a known number
 * of microseconds per chunk; week 1's open question is which real runtime
 * replaces it (llama.cpp for CHAT, a small detector for DETECT).
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

/* Burn CPU for approximately us microseconds at the caller's priority.
 * This is CPU-bound on purpose: a sleeping thread cannot demonstrate
 * preemption, and real inference is CPU-bound too. */
void     engine_burn_us(uint64_t us);

/* Shape of a tier's work. */
unsigned engine_chunks(triage_task_t task, triage_tier_t tier);
uint64_t engine_chunk_us(triage_task_t task, triage_tier_t tier);
uint64_t engine_nominal_us(triage_task_t task, triage_tier_t tier);

/* Run one chunk of a tier's work. */
void     engine_run_chunk(triage_task_t task, triage_tier_t tier);

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
