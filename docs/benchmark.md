# Triage vs a first-come-first-served inference server

Measured 16 Nov 2026 on QNX 8.0 x86_64 under QEMU, one CPU.
Harness: [`bench/run.sh`](../bench/run.sh). Baseline: [`bench/fcfs.c`](../bench/fcfs.c).
Raw output: [`bench-work300.txt`](bench-work300.txt),
[`bench-work500.txt`](bench-work500.txt), [`bench-work800.txt`](bench-work800.txt).

> **These numbers predate a fix and are being re-measured (16 Nov 2026).**
> Triage's resource manager was serialising its own handlers on the lock
> that `iofunc_func_init()` installs, so an urgent request waited for the
> chat inference in flight — and the latencies below come from the
> server's own clock, which starts *after* that wait and therefore could
> not see it. On one isolated measurement the client was waiting 193 ms
> where the server reported 4.7 ms.
>
> The relative result is not in doubt: the baseline's failure mode is
> queueing behind a 300 ms request, which is hundreds of milliseconds no
> choice of clock conceals, and its queue-mode figures already include its
> own queue. But Triage's side was flattered and the gap will narrow.
> `brake_reflex` now times the round trip itself. See
> [`architecture.md`](architecture.md#the-resource-manager-was-serialising-itself-16-nov-2026).

## The question

Does making AI inference a QNX resource manager — so that a caller's
urgency crosses the process boundary through the kernel — actually protect
deadlines that a conventional inference server cannot?

## The result

100 detection requests at priority 40 with a 20 ms deadline, one every
50 ms, while four chat clients at priority 10 flood the same device.
Load intensity rises left to right.

**Deadline hit rate**

| server | load 300 | load 500 | load 800 |
|---|---|---|---|
| **triage** | **100%** | **99%** | **97%** |
| fcfs pool, medium tier | 93% | 71% | 23% |
| fcfs queue, matched tiers | 73% | 82% | 88% |
| fcfs queue, medium tier *(what people deploy)* | 22% | 36% | 0% |
| fcfs queue, all-small *(confounded, see below)* | 0% | 1% | 29% |

**Worst-case latency against a 20 ms deadline**

| server | load 300 | load 500 | load 800 |
|---|---|---|---|
| **triage** | **17.6 ms** | **20.1 ms** | **22.0 ms** |
| fcfs pool, medium tier | 23.0 ms | 31.6 ms | 49.7 ms |
| fcfs queue, matched tiers | 859 ms | 1094 ms | **1752 ms** |
| fcfs queue, medium tier | 329 ms | 670 ms | 796 ms |

Three things to take from this, in descending order of confidence.

### 1. Priority inheritance is worth 70 points, and the figure is isolated

The `fcfs pool, medium tier` row is the controlled single-variable
comparison. It has the same thread pool as Triage at the same size, the
same tier, the same engine, the same work rate, the same clients, the same
boot. One thing differs: its channel is created with
`_NTO_CHF_FIXED_PRIORITY`, so its threads run at the server's own priority
instead of the caller's.

That one flag costs 93% → 23% as load rises, against Triage's 100% → 97%.

The trend is the point rather than any single cell. The baseline's hit rate
is a function of how much spare CPU happens to be lying around: give it
headroom and it looks fine, take the headroom away and it collapses.
Triage's hit rate barely moves, because it is not relying on headroom — it
is relying on the kernel taking the CPU away from the priority-10 work.
A mechanism that holds under load and a coincidence that evaporates under
load produce the same number on an idle machine, which is exactly why the
sweep matters.

### 2. First-come-first-served has a tail that no tuning fixes

Look at the worst-case table, not the averages. Every FCFS queue
configuration has a worst case between 329 ms and 1752 ms against a 20 ms
deadline — 16× to 88× over.

`fcfs queue, matched tiers` is the interesting one. It runs exactly the
tiers Triage chose for itself (small for detection, large for chat), so
tier selection is held constant and only the scheduling differs. Its
average hit rate is respectable, 73–88%, and its worst case is up to
**1.75 seconds**. A detection request that arrives behind several 300 ms
chat requests waits behind all of them, because that is what
first-come-first-served means.

For anything attached to a brake, the tail *is* the specification. An
inference server that answers "is that a pedestrian?" within 20 ms
four times out of five and takes 1.75 seconds the fifth time has not
nearly solved the problem; it has moved it somewhere harder to see.

### 3. The win is not "Triage just runs a smaller model"

That was the first thing to rule out, since Triage degrades to the small
tier under load and a cheaper model is trivially faster. Two controls
address it:

- `matched tiers` gives the baseline Triage's own tier choices. Triage
  still wins on hit rate at every load, and by 40–80× on the tail.
- `pool medium` holds the tier at medium for both. Triage wins there too.

So tier selection is not where the advantage comes from. It contributes —
dropping to small is why Triage's *mean* is 6–11 ms rather than ~12 ms —
but the deadline protection is the scheduling.

`all-small` is kept in the table and labelled because it was the first
control written and it is confounded: running detection at the small tier
also runs the chat flood at the small tier, which drains the queue faster
and helps detection for a reason that has nothing to do with the tier
detection used. Its numbers are erratic (0%, 1%, 29%) and should not be
read as a measurement of anything. It is here as a record of a control
that looked right and was not.

## Why the baseline is the strongest honest version, not a straw man

A benchmark against a deliberately weakened opponent is worth less than no
benchmark, so every difference between the two servers is deliberate and
listed.

**Identical:** the engine (`server/engine.c`, linked into both), the
transport (QNX resource manager, `devctl`), the protocol, the reply struct,
the chunk loop, the preemption accounting, the timing code, the thread pool
sizing, the client binaries, the priorities, the deadline, the request
count, the flood, the machine, and the boot. The clients are pointed at a
different device path with `-D` and nothing else changes.

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

Stated because they constrain what the numbers mean, and two of them were
found the hard way.

**The host steals CPU, so absolute latencies are not measurements.** QEMU
on a Windows laptop deschedules the guest: with nothing competing at all, a
priority-40 request was measured losing 9.3 ms, and 24 of 40 requests in an
idle run lost CPU. Nothing inside QNX can preempt priority 40 on an idle
system. That theft is invisible to the guest and comparable in size to the
20 ms deadline, which is why this is run as a differential on a single boot
— all servers in a run suffer it — and why no absolute figure here should
be quoted as a latency. Final absolute numbers need bare metal.

**The two servers must do identical arithmetic, and at first they did
not.** Each server process calibrates its own work rate at startup, and
that calibration varies by over 2× on this host. So a "12 ms tier" was not
the same work in both servers, and the pool-mode baseline scored 98% purely
because its chat tier had calibrated cheap — 308 chat requests at a nominal
120 ms each in a window too short to hold them. The harness now measures
once and passes the same rate to every server with `-W`, and the same
baseline then scored 30%. Any comparison of this kind that does not pin
that parameter is measuring its own calibration noise.

**A baseline that accidentally inherits priority is not a baseline.** The
first version set `_NTO_CHF_FIXED_PRIORITY` by passing it to
`dispatch_create_channel`, whose flags argument takes `DISPATCH_FLAG_*`
values — and `_NTO_CHF_FIXED_PRIORITY` is `0x0001`, the same value as
`DISPATCH_FLAG_NOLOCK`. The same number means two different things in two
namespaces and the compiler cannot tell. The baseline ran with inheritance
still enabled and "beat" Triage, 100% to 99%. It was caught only because
`brake_reflex` checks the priority pair in every reply and reported 0 of
120 requests not inherited, from a server whose entire purpose is not to
inherit. Reporting evidence instead of asserting it paid for itself.

The harness now refuses to record a row unless the server's inheritance
behaviour matches its label, and refuses to record any row where the flood
served zero requests — because an earlier run measured four baselines
against no load at all (`chat_load` parsed its device flag and then ignored
it), and the resulting table looked entirely plausible.

**One CPU, by choice.** With spare cores an urgent request runs on an idle
core, nothing is preempted, and the demo proves nothing. See
[`architecture.md`](architecture.md).

**Load intensity is a parameter, not a constant.** The sweep covers 300 to
800 work units per microsecond. Triage holds across it; the baselines do
not. Outside that range nothing is claimed.

## Reproducing

On the target:

```sh
cd /data && WORK=500 ./run.sh -n 100 -s 16
```

From the host, via WSL2:

```bash
wsl -d Ubuntu -- bash scripts/wsl-env.sh bash scripts/qemu.sh bench -n 100 -s 16
```

The harness prints every server's counters and the flood's own throughput
per case, so each row can be checked rather than taken on trust.
