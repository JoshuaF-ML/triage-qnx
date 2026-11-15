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
  |    3. sched_pick_tier(): clean cost x expected interference         |
  |    4. run_request(): chunk, measure, re-check deadline  |
  |    5. reply with the answer AND the evidence            |
  +---------------------------------------------------------+
         ^
         | A CPU budget ceiling for low-priority work, so chat
         | cannot starve control work even in pathological cases.
         | Still owed: Adaptive Partitioning is gone in QNX 8.0,
         | and the measurements below show tier selection needs
         | this to be bounded in order to be reliable.
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

## Budget protection, and the death of pillar 4 as designed

Priority inheritance protects the *urgent* work. It does nothing for the
opposite failure: a flood of AI requests starving the control loops that
are not using AI at all. Priority says "who first", not "how much in
total". Something has to cap the total.

The original design used QNX **Adaptive Partitioning** for this. That is
not available:

- QNX's SDP 8.0 component discontinuation notice lists the adaptive
  partitioning scheduler as **Discontinued**, with the replacement given
  as N/A.
- The 8.0 IDE user guide states plainly that QNX OS 8.0 does not support
  adaptive partitioning, so an 8.0 target sends no APS data at all.
- Corroborated locally: `mkqnximage --help` on our SDP 8.0 install offers
  no partitioning option, and the `com.qnx.qnx800.target.qos.group`
  packages are marked unavailable under the non-commercial licence.

Every APS tutorial still in circulation describes 6.x or 7.x. This was
listed as a medium risk in the build plan with "check on day 1" as the
mitigation; the check came back negative, so the fallback applies.

**What replaces it.** Two candidates, to be decided in week 2:

1. **POSIX sporadic scheduling (`SCHED_SPORADIC`)**, if QNX 8.0 supports
   it — to be verified, not assumed, given what just happened to APS. A
   sporadic thread runs at a high priority until it exhausts a CPU budget,
   then drops to a low priority until the budget replenishes. That is
   budget protection enforced by the kernel, which is a stronger claim
   than anything we could implement ourselves, and it is a genuine QNX
   real-time service for the bonus-prize criteria.
2. **Budget accounting at chunk boundaries**, in `server/sched.c`. We
   already measure CPU time per chunk and already have a decision point
   between chunks. A token bucket per priority class — refilled on a
   timer, debited per chunk, with low-priority work made to wait when its
   bucket is empty — gives a hard ceiling in our own code. Weaker than
   kernel enforcement, but entirely within our control and impossible to
   have removed from under us.

Option 1 is better if it exists. Option 2 is the guaranteed fallback and
can be built regardless. Whatever ships, the README states what actually
enforces the ceiling rather than implying an OS feature we are not using.

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
  as a budget. Needs whichever budget mechanism replaces APS -- and the
  interference measurements turned this from a nice-to-have into a
  dependency, since tier selection can predict interference but cannot
  put a ceiling on it.
- **"It meets its deadlines."** Measured at 92-100% across runs on QEMU,
  where the host itself steals CPU in deadline-sized quantities. The
  mechanism is demonstrated; the number is not yet a result. Bare metal
  and the FCFS baseline are what would make it one.

## Measuring on one CPU, on purpose

`scripts/qemu.sh` builds the target with `--cpu=1` by default. This is a
methodological choice, not a limitation.

With two or more cores, an urgent request can simply run on an idle core.
Nothing is preempted; there was just spare capacity. The headline numbers
would look excellent and would prove nothing, and a judge who thinks about
it for ten seconds would notice. On a single core the kernel *must* take
the CPU away from the priority-10 chat inference to serve the priority-40
detection — which is the behaviour Triage exists to demonstrate.

The benchmark should report both: one core to show the mechanism, more
cores to show it does not get worse.

## Open questions

1. **Which inference runtime.** llama.cpp has a QNX codelab and gives us
   the chat side. The detection side wants something small and
   chunkable. Decide in week 1, because the chunk-granularity requirement
   above constrains the choice.
2. ~~Is Adaptive Partitioning in the non-commercial image?~~ **Answered:
   no — it is discontinued in QNX 8.0 entirely.** See above. The open
   question is now which replacement to build.
3. ~~QEMU or Raspberry Pi for final numbers?~~ **Answered: bare metal is
   required.** The host steals CPU from the guest in quantities comparable
   to the deadline being measured, and the guest cannot tell that apart
   from a real preemption. See the measurements section above.
4. **Unblock handling.** If a client is killed or times out while its
   request is computing, the pool thread keeps burning CPU for an answer
   nobody will read. Needs an `io_unblock` handler and a cancellation
   check at chunk boundaries. Week 2.

## First measurements on the QEMU target (14 Nov 2026)

The first on-target run did three useful things: it confirmed the central
claim, it falsified two of our own numbers, and it settled an open question.
Raw output is in [`smoke-x86_64.txt`](smoke-x86_64.txt).

### Confirmed: priority inheritance

100 of 100 requests came back with `client_prio == server_prio`, at both
priority 40 and priority 12, with four priority-10 chat clients hammering
the same device. `brake_reflex` now checks this pair on every reply and
exits non-zero if any pair differs, so it is an assertion rather than a
number printed for a human to skim.

### Falsified: the preemption counters were incapable of counting

The engine burned a chunk by spinning until the wall clock had advanced by
`chunk_us`. Under that scheme, losing the CPU mid-chunk does not make the
chunk take longer — it makes the chunk do *less work in the same wall time*.
Elapsed came out at `chunk_us` whether or not we had the CPU, so
`preempted` and `stolen_us` were structurally pinned at zero. The first run
duly reported 0 preemptions across 141 requests with four low-priority
threads and a high-priority client interleaved on a single core, which is
impossible.

A chunk is now a fixed amount of *work* — calibrated integer arithmetic,
`engine_burn_us()` — because a real model does fixed arithmetic per layer:
losing the CPU makes it finish later, not do less. The same run then
reported 919 preemptions and 814 ms of stolen CPU.

This also means the first run's **100% deadline adherence was partly an
artifact**: a preempted request quietly did less work and still "finished
on time". With real work the same test reports 92%. The lower number is the
true one.

### Falsified: a declared chunk cost is not a usable baseline

Preemption is detected by noticing a chunk took longer than it should have,
which needs a trustworthy "should have". The nominal `chunk_us` is not one:
measured on this target, the same chunk's uncontended cost varied by more
than 2x within a single run. Against a fixed baseline that variance both
hides real preemptions and manufactures false ones.

The baseline is now observed, not declared —
`engine_chunk_floor_us()` keeps the fastest wall time that chunk has ever
achieved, which is the best available evidence of its cost with nobody
competing. It ratchets downward and converges in the first few requests.

The deadline estimate deliberately does *not* use that floor: the floor is
the fastest a chunk has run, so budgeting remaining work with it would be
optimistic exactly when the machine is busy. Remaining work is still priced
at nominal, which errs towards stopping early.

### Settled: QEMU on this host cannot produce the headline numbers

In step 2 of the smoke test — one client, **no load at all** — a priority-40
request was preempted 6 times and lost 9.3 ms. Nothing inside QNX can
preempt a priority-40 thread on an otherwise idle system. That stolen time
is the host: Windows and WSL2 descheduling the QEMU vCPU, which the guest
cannot see and cannot distinguish from a real in-guest preemption.

The consequence is that on this host `stolen_us` conflates two different
things, and the host's contribution is the same order of magnitude as the
20 ms deadline being measured. So open question 3 is answered: **final
benchmark numbers must come from bare metal** (a Raspberry Pi 4/5), not
from QEMU on a laptop. QEMU remains the right place to demonstrate the
mechanism and to develop against; it is not a place to measure latency.

### Exposed: the cost model is blind to the interference that causes misses

The 8 missed deadlines have a specific cause worth stating. `sched_record()`
updates the cost model only from requests with `preempted == 0`, so the
model learns what a tier costs *cleanly* — 13.2 ms for detect/medium. That
is the right answer to "what does this tier cost" and the wrong answer to
"will it fit", because the stolen time that actually breaks the deadline is
exactly what was excluded. The scheduler therefore picks medium, fits 13.2
into a 16 ms budget, and then loses 3–10 ms to interference.

Tier selection needs cost *plus* expected interference, as two tracked
quantities rather than one. That is the substantive piece of week 2, and it
is the real content of "deadline-aware" — not the tier table.

## Tier selection with interference (15 Nov 2026)

The 8 missed deadlines above had a structural cause, not an unlucky one, so
this is the fix for it. The scheduler now predicts with two terms instead
of one:

```
    predicted cost  =  clean cost  x  expected interference
                       (est_us)       (infl_pct)
```

`est_us` stays what it was — a clean measurement of what a tier costs,
fed only by unpreempted requests, which is the right answer to "what does
this model cost" and the number the benchmark should report. `infl_pct` is
new and carries the environment. They are kept apart deliberately:
collapsing them into one figure would answer neither question.

### Interference is per priority band

`infl_pct` is tracked per band of 16 priorities, not globally, and the
measurements show why that was not over-engineering:

| band | interference | what lives there |
|---|---|---|
| prio 32–47 | **259–328%** | detection, priority 40 |
| prio 0–15 | **149–184%** | chat, priority 10 |

Detection suffers *proportionally* far more interference than chat, which
is the opposite of what priority alone suggests and obvious in hindsight: a
5 ms request that loses 10 ms is at 300%, while a 300 ms request that loses
84 ms is at 128%. Interference as a ratio is scale-relative. A single
global figure would have been dominated by whichever band had more traffic
and been badly wrong for both.

### A decaying maximum, not a mean

The mean is the wrong statistic. A deadline is not met on average — it is
met or missed per request, and what breaks it is a bad moment, not a
typical one. Mean interference of 150% with spikes to 400% reproduces
exactly the original failure: the model says it fits, and sometimes it does
not.

So the stored figure jumps immediately to any worse interference observed
and decays by 63/64 per request in that band, floored at 100%. Quick to
react to a busy machine, slow to forget — the right asymmetry when being
wrong costs a missed deadline rather than a wasted millisecond. Step 5 of
the smoke test exists to prove the decay actually happens: 40 requests
after the flood stops, all deadlines met, no permanent pessimism.

The ratio is computed self-relatively, `compute_us / (compute_us -
stolen_us)`, so it needs no idea what the tier *should* have cost. That
matters because on this target the clean cost estimate is itself unreliable
— the interference model survives the cost model being wrong.

### What it bought

Detection now runs `small` under flood where it used to insist on
`medium`, and the reply carries `est_used_us`, `infl_pct` and `prio_band`
so each decision can be audited from the client — the same principle as
reporting both priorities rather than asserting inheritance.

Across three consecutive runs the smoke test went green, best run **183 of
183 deadlines met (100%) while 3.76 seconds of CPU were stolen** across
1061 preemptions. Earlier runs on the same code: 92%, 99%, 95%, 100%.

That spread is the point of the next section.

### What it did not buy, and cannot

The deadline percentage on this host is not a measurement of Triage. Step 5
runs the detection workload with nothing competing and still sees 24 of 40
requests preempted and 165 ms stolen. The host takes CPU from the guest in
quantities comparable to the 20 ms deadline, at random, invisibly.

Two consequences, both already acted on:

1. **The smoke test's step 4 is a regression tripwire at 90%, not a
   claim.** Tuning that threshold until it passes would be fitting the test
   to the host's mood. The claim is carried by step 3 instead, which
   asserts priority inheritance on every single request and never flakes,
   and will be carried by the FCFS comparison in `bench/`, which is
   differential and so cancels the host term.
2. **Interference can be predicted but not bounded.** When it spikes past
   about 340%, nothing fits, `small` runs anyway, and the reply says
   `degraded`. No amount of estimating fixes that, because the quantity
   being estimated has no ceiling. A ceiling is a different mechanism —
   which is precisely the budget enforcement owed since Adaptive
   Partitioning turned out to be discontinued. The two pillars are not
   independent after all: tier selection needs the budget to be bounded in
   order to be reliable, and that is now an argument from measurement
   rather than from the original plan.
