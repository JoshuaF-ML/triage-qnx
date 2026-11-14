# Triage — architecture

## One sentence

Triage is a QNX resource manager that serves AI inference requests, where
the kernel's priority inheritance decides *who* runs and a deadline-driven
cost model decides *how much* model to run.

## The request path

```
  brake_reflex (prio 40)              chat_load (prio 10, x4)
         |                                      |
         | devctl(fd, TRIAGE_DCMD_INFER, ...)   |
         |  = MsgSend() to the channel          |
         v                                      v
  +---------------------------------------------------------+
  |  /dev/triage   (one process, pool of receive threads)   |
  |                                                         |
  |  io_devctl()                                            |
  |    1. read client priority from ctp->info.priority      |
  |    2. read OUR priority -- they are the same            |
  |    3. sched_pick_tier(task, deadline, estimates)         |
  |    4. run_request(): chunk, measure, re-check deadline  |
  |    5. reply with the answer AND the evidence            |
  +---------------------------------------------------------+
         ^
         | Adaptive Partitioning (week 3): a CPU budget ceiling
         | for the low-priority partition, so chat cannot starve
         | control work even in pathological cases
```

The important thing about this diagram is how little of it is ours. Steps
1 and 2 are reads. The scheduling that makes the whole demo work happens
in the kernel, between the lines.

## Why a resource manager, not a server process

We could have written a socket server. It would have been portable and
wrong.

A socket server receives bytes. It has no idea who sent them, and the
thread that handles them runs at whatever priority that server chose for
itself. Urgency would have to be a *field in the message*, and we would
have to implement a scheduler to honour it — reimplementing, badly, inside
one process, what the kernel already does correctly for the whole system.

A QNX resource manager receives *messages from threads*. The kernel knows
the sender, and on a channel created without `_NTO_CHF_FIXED_PRIORITY` it
runs the receiving thread at the sender's priority. So the priority is not
data we interpret; it is the state the work actually runs in. Priority
inversion, the classic failure of hand-rolled priority queues, cannot
happen here — there is no queue for it to happen in.

This is also why the project cannot be ported to Linux as an exercise.
There is no `ctp->info.priority` on Linux. The mechanism is the platform.

## What the chunk loop is actually for

It would be easy to claim that chunking is what makes preemption work. It
is not, and the distinction is worth getting right before a judge asks.

**Preemption is the kernel's.** While a priority-10 chat request is
burning CPU inside `run_request()`, a priority-40 detection request
arriving on the channel wakes a pool thread at priority 40, and the kernel
takes the CPU away mid-instruction. No cooperation from our code is
needed or possible. If chunks were an hour long, preemption would still be
immediate.

**What chunking gives us is three things the kernel cannot:**

1. **Deadline re-evaluation.** Between chunks we re-read the clock and ask
   whether the remaining work still fits. If it does not, we stop early
   and answer with what we have. A truncated answer at 19 ms beats a
   complete one at 60 ms for anything connected to a brake.
2. **Preemption accounting.** A chunk has a known nominal cost. If wall
   clock says it took three times that, we were off the CPU — something
   more urgent ran. That is where `preempted` and `stolen_us` in every
   reply come from. It is how the dashboard will show preemption happening
   rather than asserting it.
3. **Honest cost estimates.** Only chunks that ran clean feed the cost
   model (see below), which keeps a busy machine from teaching us that
   every tier is expensive.

The practical consequence is that **chunk size is the resolution of our
deadline logic, not of preemption.** A 2 ms chunk means we can overshoot a
deadline decision by at most ~2 ms. That is why `server/engine.c` uses
1–2 ms chunks, and why a real inference backend must be chunkable at
roughly that granularity — per layer, or per small token batch. A runtime
that only returns at the end of a 300 ms generation would still be
preemptible, but Triage could not degrade it intelligently.

## Tier selection

`sched_pick_tier()` in `server/sched.c`:

- Start from the largest tier the caller will accept.
- Walk down until a tier's **estimated cost** fits in 80% of the time
  remaining. The 80% is a safety margin; a deadline met half the time is
  not a deadline.
- If nothing fits: either return `ETIME` (if the caller set
  `TRIAGE_F_REQUIRE_TIER` — some callers would rather know than be
  guessed at) or run the smallest tier and mark the reply `degraded`.
- `deadline_us == 0` means best effort, and gets whatever it asked for.

Estimates start from the engine's nominal numbers and then track reality
with an EWMA (`est = (3·est + observed) / 4`) in `sched_record()`. Only
requests with `preempted == 0` update the model — a preempted request's
wall-clock time measures system load, not model cost, and folding it in
would make Triage pessimistic exactly when it is busiest.

## Why both priority numbers are in every reply

`client_prio` and `server_prio` are in `triage_reply_t` for one reason:
the central claim of this project should be falsifiable by a user, in one
command, without a tracer. Run `brake_reflex -P 40`, then `-P 12`, and
watch the pair move together.

The stronger evidence is a kernel trace (`tracelogger` /
`traceprinter`, week 2) showing the priority-10 thread losing the CPU to
the priority-40 thread mid-inference. That is the artifact for the video.
The reply fields are the version anyone can reproduce in five seconds.

## Adaptive Partitioning (week 3, and why it is not redundant)

Priority inheritance protects the *urgent* work. It does nothing for the
opposite failure: a flood of high-priority AI requests starving the
control loops that are not using AI at all. Priority says "who first", not
"how much in total".

Adaptive Partitioning puts a CPU budget around the whole Triage process,
or around partitions within it, so low-priority AI is capped by
construction rather than by good behaviour. Availability on the
non-commercial image is an open question being checked during the setup
gate; if it is not available, the design degrades to priority-only and the
README will say so instead of implying otherwise.

## Claims we have not earned yet

Kept here deliberately, so the list shrinks visibly rather than being
quietly dropped.

- **"It runs real models."** It does not yet. `server/engine.c` burns
  calibrated CPU. Scheduling behaviour is unaffected; accuracy claims
  would be fiction, so none are made.
- **"It beats a standard inference server."** No baseline has been built
  or measured. `bench/` is empty. Week 2.
- **"Numbers are hardware-accurate."** All timing so far is on QEMU,
  which is noisy. Final numbers go on a Raspberry Pi if one is available,
  and will be reported as *measured*, never *guaranteed*.
- **"Chat can never starve safety."** True for priority; not yet enforced
  as a budget. Needs Adaptive Partitioning.

## Open questions

1. **Which inference runtime.** llama.cpp has a QNX codelab and gives us
   the chat side. The detection side wants something small and
   chunkable. Decide in week 1, because the chunk-granularity requirement
   above constrains the choice.
2. **Is Adaptive Partitioning in the non-commercial image?** Decides
   whether pillar 4 exists.
3. **QEMU or Raspberry Pi for final numbers.** Affects how strongly the
   benchmark can be stated.
4. **Unblock handling.** If a client is killed or times out while its
   request is computing, the pool thread keeps burning CPU for an answer
   nobody will read. Needs an `io_unblock` handler and a cancellation
   check at chunk boundaries. Week 2.
