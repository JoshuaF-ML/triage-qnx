# Triage vs a first-come-first-served inference server

Measured 16 Nov 2026 on QNX 8.0 x86_64 under QEMU, one CPU.
Harness: [`bench/run.sh`](../bench/run.sh). Baseline: [`bench/fcfs.c`](../bench/fcfs.c).
Raw output: [`bench-work300.txt`](bench-work300.txt),
[`bench-work500.txt`](bench-work500.txt), [`bench-work800.txt`](bench-work800.txt).

**This is the second run of this benchmark.** The first one is superseded,
and what changed is written up in [What the re-run
changed](#what-the-re-run-changed-and-why-the-first-one-was-wrong) rather
than quietly dropped — including two changes that cost Triage points.

## The question

Does making AI inference a QNX resource manager — so that a caller's
urgency crosses the process boundary through the kernel — actually protect
deadlines that a conventional inference server cannot?

## The result

150 detection requests at priority 40 with a 20 ms deadline, one every
50 ms, while four chat clients at priority 10 flood the same device.
Load intensity rises left to right.

**Deadline hit rate**

| server | load 300 | load 500 | load 800 |
|---|---|---|---|
| **triage** | **100%** | **100%** | **100%** |
| fcfs queue, matched tiers | 80% | 87% | 92% |
| fcfs queue, medium tier *(what people deploy)* | 49% | 67% | 79% |
| fcfs pool, medium tier *(the isolated control)* | 27% | 25% | 39% |
| fcfs queue, all-small *(confounded, see below)* | 6% | 28% | 50% |

**Worst-case latency against a 20 ms deadline — measured by the client**

| server | load 300 | load 500 | load 800 |
|---|---|---|---|
| **triage** | **16.1 ms** | **3.4 ms** | **12.1 ms** |
| fcfs pool, medium tier | 114 ms | 116 ms | 62 ms |
| fcfs queue, all-small | 303 ms | 286 ms | 375 ms |
| fcfs queue, medium tier | 398 ms | 737 ms | 931 ms |
| fcfs queue, matched tiers | 874 ms | 1362 ms | **2236 ms** |

**Mean latency, same runs**

| server | load 300 | load 500 | load 800 |
|---|---|---|---|
| **triage** | **0.96 ms** | **0.41 ms** | **4.0 ms** |
| fcfs pool, medium tier | 39 ms | 43 ms | 36 ms |
| fcfs queue, all-small | 99 ms | 110 ms | 120 ms |
| fcfs queue, medium tier | 127 ms | 137 ms | 148 ms |
| fcfs queue, matched tiers | 143 ms | 149 ms | 151 ms |

Every latency above is timed **by the client, around its own `devctl`**, so
it includes everything the server made the client wait for. That was not
true of the first run, and it is the single most important thing about this
one.

Four things to take from it, in descending order of confidence.

### 1. Priority inheritance is worth 61–75 points, isolated to one flag

The `fcfs pool, medium tier` row is the controlled single-variable
comparison. Same thread pool as Triage at the same size, same tier, same
engine, same work rate, same clients, same boot, and — new in this run —
the same unserialised handlers. One thing differs: its channel is created
with `_NTO_CHF_FIXED_PRIORITY`, so its threads run at the server's own
priority instead of the caller's.

That one flag costs 100% → 27%, 25%, 39%.

The trend matters more than any cell. Triage holds at 100% across a
2.7× sweep in load intensity, because it is not relying on spare CPU — it
is relying on the kernel taking the CPU away from the priority-10 work.

### 2. Concurrency without priority is worse than no concurrency

This is new, and it is the most interesting result in the re-run.

`fcfs pool` scored 93% in the first benchmark and scores 27% here. Triage
did not get better; **the baseline got a fair shot and lost by more.** In
the first run, pool mode was accidentally serialised by the resource
manager framework's own lock, so its four chat threads were forced through
one at a time and a detection request queued behind exactly one
computation. Unserialised, four chat computations run concurrently at the
server's fixed priority — and the detection request, which has no priority
advantage there, round-robins against all four at once.

So the lesson is not "add threads". A thread pool *multiplies* the work
competing with an urgent request, and only a priority mechanism turns that
from a liability into an advantage. Triage is the same pool with the
caller's priority attached, and that is the whole difference.

Note also that pool mode's tail (62–116 ms) is far better than any queue
mode's (286–2236 ms) while its hit rate is far worse. Concurrency fixes the
tail and ruins the average; priority fixes both.

### 3. First-come-first-served has a tail that no tuning fixes

Look at the worst-case table, not the averages. Every FCFS queue
configuration has a worst case between 286 ms and 2236 ms against a 20 ms
deadline — **14× to 112× over**.

`fcfs queue, matched tiers` is the one to look at. It runs exactly the
tiers Triage chose for itself (small for detection, large for chat), so
tier selection is held constant and only the scheduling differs. Its hit
rate is the best of any baseline — 80%, 87%, 92% — and its worst case is
the worst of any baseline, up to **2.24 seconds**. A detection request that
arrives behind several 300 ms chat requests waits behind all of them,
because that is what first-come-first-served means.

For anything attached to a brake, the tail *is* the specification. An
inference server that answers "is that a pedestrian?" within 20 ms nine
times out of ten and takes 2.2 seconds the tenth time has not nearly
solved the problem; it has moved it somewhere harder to see.

The mean table makes the same point without the tail: 0.4–4 ms against
36–151 ms, a gap of 9× to 370×.

### 4. The win is not "Triage just runs a smaller model"

That was the first thing to rule out, since Triage degrades to the small
tier under load and a cheaper model is trivially faster. Two controls
address it:

- `matched tiers` gives the baseline Triage's own tier choices. Triage
  still wins on hit rate at every load, and by 54–185× on the tail.
- `pool medium` holds the tier at medium for both. Triage wins there too,
  by 61–75 points.

So tier selection is not where the advantage comes from. It contributes,
and visibly: Triage's own worst case is *lowest* at load 500 (3.4 ms)
rather than at the lightest load, because more interference makes it pick
a cheaper tier sooner and finish earlier. But the deadline protection is
the scheduling.

### Why the baselines improve as load rises

`fcfs queue, matched` goes 80% → 87% → 92% as the work rate rises, which
looks backwards. It is the same effect flagged in the first run: the work
rate is not a pure difficulty knob. Raising it makes each chat request more
expensive, so fewer of them complete inside the fixed flood window, so the
queue a detection request lands behind is *shorter*.

The honest reading is that queue-mode hit rate is a function of how many
chat requests happen to be queued, which is why the tail is the figure to
read for those rows and why the sweep exists at all. Triage is flat at 100%
across the sweep, so there is nothing to explain away on that row.

## What the re-run changed, and why the first one was wrong

Three changes. Two of them hurt Triage's numbers and are kept for that
reason.

**1. Triage was serialising its own handlers (fixed).**
`iofunc_func_init()` installs `lock_ocb`/`unlock_ocb`, which lock the
shared device attribute around every I/O message, so exactly one
`io_devctl()` ran at a time however many pool threads existed. An urgent
detection request waited for the whole of whatever chat inference was in
flight. Full write-up in
[`architecture.md`](architecture.md#the-resource-manager-was-serialising-itself-16-nov-2026).

**2. Every latency in the first run came from the server's own clock
(fixed, and this one is the point).** `total_us` starts at the top of the
handler — *after* the framework hands the message to a thread — so the
entire wait was outside the measurement. `queue_us` was worse than useless:
measured from inside the handler, it reported 3 µs while the real queueing
was 190 ms. In one isolated measurement the server reported a worst case of
4.7 ms against a 20 ms deadline while the client had waited 193.5 ms.

`brake_reflex` now times its own round trip and reports both figures, and
the harness records both. The gap between the columns is itself a
diagnostic: for Triage it is now **1.3–2.6%**, which is the evidence that
the queueing is gone rather than merely unmeasured.

It is also what makes this run comparable for the first time. FCFS queue
mode always included its own queue, because it timestamps at receive and
defers with `_RESMGR_NOREPLY`; Triage did not. The first benchmark was
measuring the two sides with two different clocks.

**3. The baseline got the same fix, which cost Triage 66 points on the
controlled comparison.** `fcfs.c` now clears the same two hooks. Queue mode
barely notices — it defers immediately — but pool mode runs requests to
completion in the handler and was therefore fully serialised. Leaving it
that way would have made the control measure inheritance *and*
serialisation and credit both to inheritance. The cost of fixing it is
visible: pool mode went from 93% to 27% at load 300, so the claim "this is
what the flag is worth" rests on a baseline that is now strictly stronger
per request and happens to be worse overall for a reason worth
understanding (see finding 2).

Residual gap worth noting: pool mode still shows a 16–27% spread between
the client's and the server's worst case, the largest of any server here.
Something is still queueing there — most likely pool threads being created
on demand under a burst — and it is not chased further because it makes the
baseline look *better* than the client's experience, not worse.

## Why the baseline is the strongest honest version, not a straw man

A benchmark against a deliberately weakened opponent is worth less than no
benchmark, so every difference between the two servers is deliberate and
listed.

**Identical:** the engine (`server/engine.c`, linked into both), the
transport (QNX resource manager, `devctl`), the protocol, the reply struct,
the chunk loop, the preemption accounting, the timing code, the thread pool
sizing, the unserialised handlers, the client binaries, the priorities, the
deadline, the request count, the flood, the machine, and the boot. The
clients are pointed at a different device path with `-D` and nothing else
changes.

**Different, and why each is fair:**

1. **No priority inheritance** — `_NTO_CHF_FIXED_PRIORITY`. This is the
   honest mapping of a socket server onto QNX, not a handicap. llama.cpp's
   server, Triton and every HTTP model endpoint receive bytes rather than
   messages from threads: there is no caller priority available to inherit
   even in principle, and their workers run at whatever priority the
   process started at.
2. **First come, first served** — one worker, strict arrival order, because
   a conventional server has one model instance and queues against it. This
   took explicit work: QNX queues blocked senders by priority, so the
   kernel would otherwise have handed the baseline the highest-priority
   message first. `io_devctl` defers with `_RESMGR_NOREPLY` and the worker
   replies with `MsgReply` in arrival order. Skipping that would have given
   the baseline a scheduling advantage it has not earned and would have
   overstated Triage's win.
3. **No deadline awareness** — the deadline is recorded and reported, never
   acted on. One fixed tier runs to completion. This is the part of Triage
   with no counterpart in a standard server at all.
4. **Bounded queue** — 512 entries, rejecting with `EAGAIN` when full, like
   any real server. Unbounded buffering would have flattered it.

`pool` mode additionally keeps the full thread pool, so concurrency is held
constant and the remainder is attributable to inheritance.

## Threats to validity

Stated because they constrain what the numbers mean, and most of them were
found the hard way.

**The host steals CPU, so absolute latencies are not measurements.** QEMU
on a Windows laptop deschedules the guest: with nothing competing at all, a
priority-40 request was measured losing 9.3 ms, and 24 of 40 requests in an
idle run lost CPU. Nothing inside QNX can preempt priority 40 on an idle
system. That theft is invisible to the guest and comparable in size to the
20 ms deadline, which is why this is run as a differential on a single boot
— all servers in a run suffer it — and why no absolute figure here should
be quoted as a latency. Final absolute numbers need bare metal. Triage's
16.1 ms worst case at load 300 is very likely mostly host theft, given it
drops to 3.4 ms at a *heavier* load.

**A server cannot time the wait it imposed before it saw you.** See change
2 above. This is the one that invalidated the first benchmark, and the
general form is worth keeping: a latency reported by the component being
measured excludes whatever that component did to you before it started
counting.

**The two servers must do identical arithmetic, and at first they did
not.** Each server process calibrates its own work rate at startup, and
that calibration varies by over 2× on this host. So a "12 ms tier" was not
the same work in both servers, and the pool-mode baseline scored 98% purely
because its chat tier had calibrated cheap — 308 chat requests at a nominal
120 ms each in a window too short to hold them. The harness now measures
once and passes the same rate to every server with `-W`. Any comparison of
this kind that does not pin that parameter is measuring its own calibration
noise.

**A baseline that accidentally inherits priority is not a baseline.** The
first version set `_NTO_CHF_FIXED_PRIORITY` by passing it to
`dispatch_create_channel`, whose flags argument takes `DISPATCH_FLAG_*`
values — and `_NTO_CHF_FIXED_PRIORITY` is `0x0001`, the same value as
`DISPATCH_FLAG_NOLOCK`. The same number means two different things in two
namespaces and the compiler cannot tell. The baseline ran with inheritance
still enabled and "beat" Triage, 100% to 99%. It was caught only because
`brake_reflex` checks the priority pair in every reply and reported 0 of
120 requests not inherited, from a server whose entire purpose is not to
inherit.

The harness refuses to record a row unless the server's inheritance
behaviour matches its label, refuses to record any row where the flood
served zero requests — because an earlier run measured four baselines
against no load at all (`chat_load` parsed its device flag and then ignored
it) and the table looked entirely plausible — and now also refuses to run
at all against a client too old to report its own round trip.

**`all-small` is confounded and kept as a record.** Running detection at
the small tier also runs the chat flood at the small tier, which drains the
queue faster and helps detection for a reason that has nothing to do with
the tier detection used. It was the first control written and it was the
wrong one.

**One CPU, by choice.** With spare cores an urgent request runs on an idle
core, nothing is preempted, and the demo proves nothing. See
[`architecture.md`](architecture.md).

**Load intensity is a parameter, not a constant**, and not a monotone
difficulty knob either — see "Why the baselines improve as load rises". The
sweep covers 300 to 800 work units per microsecond. Outside that range
nothing is claimed.

**One run per cell.** These are single runs at each load, not averages over
repeats, so small differences between adjacent cells mean nothing. The
effects being claimed are 3× to 185×.

## Reproducing

On the target:

```sh
cd /data && WORK=500 ./run.sh -n 150 -s 20
```

From the host, via WSL2:

```bash
wsl -d Ubuntu -- bash scripts/wsl-env.sh bash scripts/qemu.sh bench -n 150 -s 20
```

The harness prints every server's counters and the flood's own throughput
per case, so each row can be checked rather than taken on trust.
