# Codelab: make the OS enforce urgency in your AI service

**QNX 8.0 · x86_64 under QEMU or a Raspberry Pi 4/5 · about 90 minutes**

You are going to build an AI inference service where a life-critical
request preempts a chatbot request *mid-computation* and still answers on
time — and you are going to do it without writing a scheduler, a priority
queue, or a single line of urgency-handling logic.

The OS does that part. The interesting work is noticing that it does, and
then building the thing that the OS *cannot* know how to do.

By the end you will have:

- a resource manager that answers questions over `devctl()` (≈70 lines);
- hard evidence, from your own target, that QNX ran your server's work at
  the *client's* priority — and a one-flag experiment that turns that off
  so you can see the difference;
- an understanding of why that mechanism is worth nothing without a thread
  pool, and a measurement that shows it;
- a map of how the production version ([Triage](../README.md)) turns those
  three facts into an inference service with deadlines, and what each extra
  piece buys.

Everything in Part 1 is in this directory, builds standalone, and runs on
your target. Part 2 walks the real thing.

### Who this is for

You can write C and you know what a priority is. You do **not** need to
know QNX. Where this codelab uses a QNX API you have not seen, it says
what the call does and what breaks without it.

---

## Table of contents

**Part 1 — build it yourself**

| | | |
|---|---|---|
| [Step 0](#step-0--set-up) | Set up | get a target and a toolchain |
| [Step 1](#step-1--the-smallest-useful-resource-manager) | The smallest useful resource manager | `/dev/clab1` answers a question |
| [Step 2](#step-2--the-mechanism-priority-inheritance) | **Priority inheritance** | the whole idea, in two numbers |
| [Step 3](#step-3--why-inheritance-needs-a-thread-pool) | Why inheritance needs a thread pool | and how we measured it wrong |

**Part 2 — what a real service adds**

| | |
|---|---|
| [Step 4](#step-4--inference-you-can-stop-in-the-middle) | Inference you can stop in the middle |
| [Step 5](#step-5--let-the-deadline-choose-the-model) | Let the deadline choose the model |
| [Step 6](#step-6--the-cost-model-is-not-enough) | The cost model is not enough |
| [Step 7](#step-7--a-cpu-ceiling-the-kernel-enforces) | A CPU ceiling the kernel enforces |
| [Step 8](#step-8--stop-work-nobody-is-waiting-for) | Stop work nobody is waiting for |
| [Step 9](#step-9--prove-it-against-a-fair-baseline) | Prove it against a fair baseline |

[What to take away](#what-to-take-away) · [Where the claims are weak](#where-the-claims-are-weak)

---

## The problem, in one paragraph

A robot or a car has one AI brain and many askers. The brake controller
asks *"is that a person?"*; the voice assistant asks *"what's the
weather?"* Conventional inference servers — llama.cpp's server, Triton,
every HTTP model endpoint — serve requests first come, first served. They
cannot tell that one of those questions has 20 milliseconds to live and
the other has two seconds. QNX's own 2026 survey of 1,000 robotics
developers found 89% consider physical AI critical while only 29% feel
confident they can make safe, predictable decisions in the real world.
A good chunk of that gap is a scheduling problem wearing an AI costume.

The usual fix is a priority queue inside the server. That helps with
*ordering* and does nothing about the request already running: a detection
request that arrives 1 ms after a 300 ms chat request still waits 300 ms,
whatever the queue thinks of it. What you need is **preemption**, and
preemption belongs to the kernel.

---

## Step 0 — set up

You need **QNX SDP 8.0**. It is free for non-commercial use.

### A target

Either is fine for the whole codelab.

**QEMU (fastest to start).** SDP ships `mkqnximage`:

```bash
source ~/qnx800/qnxsdp-env.sh
ssh-keygen -q -t ed25519 -N '' -f ~/.ssh/id_ed25519   # if you have no key
mkqnximage --type=qemu --arch=x86_64 --cpu=1 --ram=1G --ip=dhcp \
           --data-size=200 --ssh-ident=$HOME/.ssh/id_ed25519.pub \
           --noprompt --force --clean --build
```

**Use one CPU.** This is not a performance choice, it is an
*observability* choice: with several cores an urgent request can simply
run on an idle core, and "it preempted the chat inference" becomes
unfalsifiable, because nothing was preempted. On one core the kernel
**must** take the CPU away from the low-priority work. Turn the cores up
later, to see how much of your result was really just headroom.

This repository wraps the above, including booting QEMU and copying files
in, in [`scripts/qemu.sh`](../scripts/qemu.sh).

**Raspberry Pi 4/5.** Build everything with `ARCH=aarch64le` and copy it
over; nothing in this codelab is x86-specific.

### The codelab's programs

```bash
source ~/qnx800/qnxsdp-env.sh
cd codelab
make                       # or: make ARCH=aarch64le
```

Four binaries land in `build/<arch>/`: three servers, one for each step,
and `ask`, the client used by all of them. Copy them to the target
(`/data` is writable on a `mkqnximage` image) and **log in as root** —
raising a thread's priority needs the `PROCMGR_AID_PRIORITY` ability.

There is also a script that checks every claim this codelab makes about
its own programs, on your target:

```bash
cd /data/codelab && ./verify.sh
```

If you are driving a QEMU target from this repository, one command does
the build, the copy and the check:

```bash
make -C codelab && ./scripts/qemu.sh codelab
```

Read [`verify.sh`](verify.sh) before you trust it. Two of its three checks
are falsifiable: one requires inheritance to happen on *every* request and
then requires it *not* to happen when a single flag is set, and one
requires a specific latency to get much worse when the thread pool is
shrunk. A tutorial whose output you cannot reproduce is a blog post.

---

## Step 1 — the smallest useful resource manager

**File: [`step1_hello.c`](step1_hello.c) · 70 lines**

On Linux you would reach for a socket, pick a port, invent a framing
protocol, and serialise. On QNX you register a path:

```c
id = resmgr_attach(dpp, &resmgr_attr, "/dev/clab1", _FTYPE_ANY, 0,
                   &connect_funcs, &io_funcs, &dev_attr);
```

Your process is now a device. Any program on the system can `open()` it
and talk to it with `devctl()`, and the request arrives as a **struct in
your address space**, because the kernel put it there. No broker, no
socket, no serialisation.

Three pieces make that work:

```c
dispatch_create();     /* a channel + the dispatch layer on top of it */
iofunc_func_init(...); /* fill both function tables with defaults ... */
io_funcs.devctl = io_devctl;   /* ... then override only what you do */
```

`iofunc_func_init()` is why this file is 70 lines instead of 700:
`open()`, `close()` and `stat()` on `/dev/clab1` already work correctly
and you did not write them.

The handler itself is the shape every resource manager has:

```c
static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg,
                     RESMGR_OCB_T *ocb)
{
    clab_msg_t m;
    int        status;

    status = iofunc_devctl_default(ctp, msg, ocb);
    if (status != _RESMGR_DEFAULT)
        return status;              /* a standard devctl; already handled */

    if (msg->i.dcmd != CLAB_DCMD_ASK)
        return ENOTTY;
    if (msg->i.nbytes < sizeof m)
        return EINVAL;

    /* msg->i and msg->o are a UNION. Copy the request out before you
     * touch the reply, or you will overwrite what you are reading. */
    memcpy(&m, _DEVCTL_DATA(msg->i), sizeof m);

    snprintf(m.text, sizeof m.text, "hello from a resource manager");

    memset(&msg->o, 0, sizeof msg->o);
    msg->o.nbytes = sizeof m;
    memcpy(_DEVCTL_DATA(msg->o), &m, sizeof m);
    return _RESMGR_PTR(ctp, msg, sizeof msg->o + sizeof m);
}
```

### Run it

```bash
./step1_hello &
ls -l /dev/clab1
./ask -D /dev/clab1 -n 3
```

```
crw-rw-rw-  1 root      root     0 Nov 16 05:53 /dev/clab1
ask: dev=/dev/clab1 my_prio=10 burn=0us n=3
  [0]     21us prio=-1/-1 served=0us  hello from a resource manager
  [1]      8us prio=-1/-1 served=0us  hello from a resource manager
  [2]      7us prio=-1/-1 served=0us  hello from a resource manager
```

**Seven microseconds, round trip, process to process.** That is the
message-passing path the rest of the codelab is built on: no copy through
a socket buffer, no scheduler round trip you did not ask for.

The `prio=-1/-1` is step 1 refusing to answer a question it has not been
taught yet. Step 2 teaches it.

> **If you get `ENOTTY`**, your client and server disagree about the
> `devctl` command number. `__DIOTF(_DCMD_MISC, 1, clab_msg_t)` encodes
> the struct's *size*, so the two sides must compile against the identical
> header. This is the single most common way a first resource manager
> appears broken — and in this project it also appeared as a stale object
> file, where the server and client were built from the same tree but
> against different versions of the struct.

---

## Step 2 — the mechanism: priority inheritance

**File: [`step2_prio.c`](step2_prio.c)**

This is the step the whole project comes from. Everything else is
consequences.

Add two lines to step 1's handler:

```c
m.client_prio = ctp->info.priority;    /* the priority of the SENDER */

struct sched_param param;
int                policy;
pthread_getschedparam(pthread_self(), &policy, &param);
m.server_prio = param.sched_priority;  /* the priority WE are running at */
```

The first is the priority of the thread that sent the message. The second
is the priority of the thread handling it, read right now, from inside the
handler.

### Run it

```bash
./step2_prio &
./ask -D /dev/clab2 -P 40 -n 5
./ask -D /dev/clab2 -P 12 -n 5
```

Measured on a QNX 8.0 x86_64 QEMU target:

```
ask: dev=/dev/clab2 my_prio=40 burn=0us n=5
  [0]     13us prio=40/40 served=0us  served at 40
  ...
ask summary: 5 inherited, 0 not inherited, 0 unreported, 0 errors
  OK   priority was inherited on every request

ask: dev=/dev/clab2 my_prio=12 burn=0us n=5
  [0]     12us prio=12/12 served=0us  served at 12
  ...
  OK   priority was inherited on every request
```

**The two numbers are the same, and they move together when the client
changes.** We did not set that. We did not read one and apply the other.
We could not prevent it if we wanted to. A synchronous `MsgSend()` — which
is what `devctl()` becomes — runs the *receiving* thread at the *sending*
thread's priority.

So the urgent client's question is not *tagged* urgent. Inside the server
it **is** priority-40 work, and the kernel will take the CPU away from
whatever priority-10 work is running to do it. Urgency crossed a process
boundary with no code on either side.

The client's only act of urgency, in full:

```c
struct sched_param p = { .sched_priority = prio };
pthread_setschedparam(pthread_self(), SCHED_RR, &p);
```

No priority field in the request. No `urgent: true`. The thread's priority
*is* the urgency, and the OS already knows how to propagate it.

### Now break it on purpose

A mechanism you have only ever seen working is a mechanism you do not
understand yet. `step2_prio -f` creates the channel **with**
`_NTO_CHF_FIXED_PRIORITY`:

```bash
slay step2_prio
./step2_prio -f &
./ask -D /dev/clab2 -P 40 -n 5 -f
```

```
step2: channel flags 0x006b accepted
step2: serving /dev/clab2, priority FIXED (no inheritance)

ask summary: 0 inherited, 5 not inherited, 0 unreported, 0 errors
  OK   priority was not inherited, as expected
```

One flag, and the server now runs every client's work at its own priority
instead. That flag is the entire difference between this design and a
conventional inference server, and in [Step 9](#step-9--prove-it-against-a-fair-baseline)
it is the variable the benchmark isolates — the baseline is this program's
`-f` mode, not a straw man.

Note that `dispatch_create()` gives you inheritance **by default**. The
mechanism is not something you switch on; it is something you must take
care not to switch off.

> ### The trap that costs a day
>
> `_NTO_CHF_FIXED_PRIORITY` has to go on the **channel**, through
> `ChannelCreate()`. It cannot go through
> `dispatch_create_channel(-1, _NTO_CHF_FIXED_PRIORITY)` — that argument
> takes `DISPATCH_FLAG_*` values, and `_NTO_CHF_FIXED_PRIORITY` is
> `0x0001`, **the same value as `DISPATCH_FLAG_NOLOCK`**. Two namespaces,
> one number, and the compiler cannot tell them apart. It compiles, it
> runs, and inheritance stays on.
>
> In this project that mistake made the benchmark's no-inheritance
> baseline quietly inherit priority, and it promptly "beat" the real
> server 100% to 99%. The only reason it was caught: the client *asserts*
> on the priority pair, so a server whose entire purpose was not to
> inherit reported 0 of 120 requests not inherited. **Report the evidence,
> do not assert the conclusion** — it is the single highest-value habit in
> this codelab.
>
> Which companion flags a resource manager's channel will accept is also
> not guessable: the full set is rejected with `EINVAL` on QNX 8.0. See
> `make_fixed_priority_channel()` in [`step2_prio.c`](step2_prio.c), which
> tries decreasing sets and prints the one the kernel took — `0x006b` on
> this target.

### Why this is the right place to put the mechanism

You could build a priority queue inside a conventional server. It would
reorder *waiting* requests and do nothing about the one already running:
a detection request arriving 1 ms into a 300 ms chat request waits 300 ms.

Here, the kernel preempts that chat computation at the next instruction.
The server's code contains no scheduling policy at all — and it gets this
for free for *every* client on the system, including ones written by
someone else who never heard of your service.

---

## Step 3 — why inheritance needs a thread pool

**File: [`step3_pool.c`](step3_pool.c)**

Step 2 proved that the thread handling your message runs at your priority.
It said nothing about **when** your message gets handled, and there are two
separate ways it can fail to get handled at all. Both are flags here, so
you can measure each one instead of believing it.

This step is also where this project found a bug in itself, so read the
measurement section even if the mechanism seems obvious.

### Problem 1: one receive thread is one receive thread

Step 1's server was a loop: block for a message, handle it, repeat. If the
only thread is busy computing somebody else's 3-second request, your urgent
message sits on the channel untouched. **The kernel cannot promote a thread
that is not yet handling your message** — inheritance has nothing to
inherit onto.

So, a pool:

```c
pool_attr.handle        = dpp;
pool_attr.context_alloc = dispatch_context_alloc;
pool_attr.block_func    = dispatch_block;
pool_attr.handler_func  = dispatch_handler;
pool_attr.context_free  = dispatch_context_free;
pool_attr.lo_water      = 2;    /* keep this many waiting */
pool_attr.hi_water      = 4;
pool_attr.maximum       = 8;    /* requests in flight at once */

tpp = thread_pool_create(&pool_attr, POOL_FLAG_USE_SELF);
thread_pool_start(tpp);         /* does not return */
```

Now run the measurement: a lazy client asks for 3 seconds of thinking at
priority 10; one second later an urgent client asks for 1 ms at priority
40. The urgent client's worst case, with 8 pool threads:

```
    8 threads, framework lock :   2008783us
```

**Two seconds**, with eight threads, seven of them idle. The pool bought
nothing at all.

### Problem 2: the framework serialises your handlers for you

`iofunc_func_init()` is the call that made step 1 seventy lines instead of
seven hundred. Among the defaults it installs are `lock_ocb` and
`unlock_ocb`, and **the resource manager framework calls them around every
I/O message**. They take a lock on the device *attribute* — which all your
clients share, because they all opened the same device.

So however many pool threads you created, exactly one `io_devctl()` runs at
a time, and an urgent request waits for the whole of whatever is in flight.

Two lines:

```c
io_funcs.lock_ocb   = NULL;
io_funcs.unlock_ocb = NULL;
```

Measured on the same target, same clients, same server source:

| server | urgent worst case |
|---|---|
| 8 threads, framework lock | 2 008 783 µs |
| 8 threads, **lock cleared** | **1 355 µs** |
| 1 thread, lock cleared | 2 009 864 µs |

**A factor of 1480** — and both conditions turn out to be necessary. The
pool without the lock removed is as bad as no pool; the lock removed
without the pool is as bad as the lock.

> **When you may do this.** The reason is specific rather than general.
> `/dev/clab3` has no file position, no size, no read/write path and no
> mutable attribute state; every request is a self-contained `devctl`, and
> any shared state the handlers touch does its own locking. A resource
> manager that served real file contents needs that lock and has to find
> its concurrency somewhere else — per-OCB state, finer-grained locks, or
> handing work to a back end and replying later with `_RESMGR_NOREPLY`
> (which is how [`bench/fcfs.c`](../bench/fcfs.c) implements a queue).

### The part worth taking away: we could not see any of this

This bug was in Triage itself for weeks, through a full benchmark, and the
project's own numbers never showed it. Twice, for two different reasons.

**The first blind spot was `open()`.** The urgent client was started while
the server was already buried, and it reported a 1.5 ms round trip against
a request it should have been stuck behind. The wait was entirely real — it
happened inside `open()`, because the connect message goes to the same
resource manager and queues on the same threads, and `open()` was not
inside the timed region. The server looked fast because the client had
already paid, somewhere nobody was looking. That is what `-W` in
[`ask.c`](ask.c) exists to fix: connect while the server is idle, then
start asking.

**The second blind spot was the server's own clock.** Triage timestamps
each request at the top of its handler and reports `total_us` from there,
and its urgent client believed that number. But the top of the handler is
*after* the framework has handed the message to a thread — so every
microsecond spent waiting on that attribute lock fell outside the
measurement. **A server cannot measure the time it made you wait before it
knew you existed.**

The fix was to time the round trip in the client, around the `devctl`
itself. Under a greedy chat client, on Triage, before the lock fix:

```
  worst     4722us                                   <- the server's figure
  rtt       worst 193520us (measured by the client)
```

The server reported meeting a 20 ms deadline on all eight requests. The
client had waited **193 ms**. After clearing the two hooks, same run:

```
  worst     8512us
  rtt       worst 8814us (measured by the client)
```

The two numbers now agree to within 4%, which is itself the evidence that
the gap was queueing and that it is gone.

Three habits come out of this, and they generalise well beyond QNX:

1. **Measure from the client.** The component being measured is the one
   that cannot see what it did to you.
2. **Measure the whole thing.** `open()` is part of the round trip even
   though it does not look like work.
3. **Let the measurement disagree with you.** Two independent numbers for
   one quantity — client and server, `client_prio` and `server_prio` —
   cost almost nothing, and they are the only reason either of these was
   found.

### Check it on your target

```bash
cd /data/codelab && ./verify.sh
```

[`verify.sh`](verify.sh) runs all three configurations and requires the
locked server to be at least 5× worse than the unlocked one, and the
one-thread server to be at least 5× worse as well. If your target
disagrees with the numbers above, it fails and prints both runs.

---

# Part 2 — what a real service adds

Everything up to here is the mechanism: **urgency crosses the process
boundary, and the server is built so that it can act on it.** It is about
250 lines.

What it does *not* do is decide how much AI to run. That is the part the OS
cannot know, and it is what [Triage](../README.md) is. Each step below is a
problem the previous one creates.

## Step 4 — inference you can stop in the middle

**[`server/engine.c`](../server/engine.c), [`server/nn.c`](../server/nn.c)**

Priority inheritance gets the urgent request *running*. It does not make
the answer arrive by any particular time — and an answer after the car has
hit the pedestrian is not a degraded answer, it is no answer.

So inference runs in **chunks**. Triage's detector is three MLPs trained on
MNIST, scanned over a 48×48 frame as a sliding window, and a chunk is a
batch of windows sized to about 2 ms:

```c
for (i = 0; i < total_chunks; i++) {
    engine_run_chunk(task, tier);           /* ~2ms of windows */
    if (cancelled) break;
    if (now + remaining_estimate > deadline_at) {
        rep->degraded = 1;
        break;                              /* answer with what we have */
    }
}
```

Between any two windows the scan can stop and still report the most
confident detection found so far. **That is what lets a deadline degrade
the model instead of merely preempting it** — and it is a real constraint
on your choice of runtime. A language model runtime that only returns after
a 300 ms generation is preemptible but not *degradable*: stopping it early
gives you nothing at all.

A tier is therefore a network **and** a scan density:

| tier | network | MNIST accuracy | windows | measured cost on target |
|---|---|---|---|---|
| small | 196-16-11 | 93.2% | 81 | 162 µs |
| medium | 196-64-11 | 96.3% | 324 | 2.59 ms |
| large | 196-128-64-11 | 97.8% | 1225 | 30.6 ms |

Those costs are **measured at startup**, not declared. An earlier version
had six hand-written numbers that had to be right for the deadline story to
work and that nothing checked.

> **A bug worth copying the fix for.** The first chunked engine spun to a
> wall-clock target rather than doing a fixed amount of work. So a
> preempted request quietly did *less* work in the same time and still
> finished "on time" — the preemption counters were structurally incapable
> of being non-zero, and a clean 100% deadline figure was an artifact.
> Burning fixed *work* dropped it to 92%, honestly. **If your benchmark
> cannot produce a bad number, it is not measuring.**

## Step 5 — let the deadline choose the model

**[`server/tier_sched.c`](../server/tier_sched.c)**

Each request states a deadline. Triage picks the largest tier whose
measured cost still fits:

```c
tier = sched_pick_tier(task, req.deadline_us, req.want_tier,
                       req.flags, rep.client_prio, &dec);
```

The estimate is an EWMA over what that tier actually took recently, so the
model adapts to the machine rather than to a developer's guess.

The trade, measured on target, 60 frames per row at priority 40:

| deadline | deadlines met | mean latency | detection accuracy |
|---|---|---|---|
| 200 ms | 100% | 12.5 ms | **78.3%** |
| 20 ms | 100% | 1.3 ms | 58.3% |
| 5 ms | 100% | 375 µs | 51.7% |
| 1 ms | 95% | 460 µs | 51.7% |

Tighten the deadline 200-fold and latency falls 27-fold while accuracy goes
from 78% to 52% — and the deadlines keep being met. **"A rough answer on
time beats a perfect answer too late", with a price tag on it.** Accuracy
is the tier's measured test-set accuracy, and the client scores the server
against ground truth carried in each reply rather than taking its word.

## Step 6 — the cost model is not enough

**[`server/tier_sched.c`](../server/tier_sched.c)**

A tier that costs 3 ms on an idle machine does not cost 3 ms on a busy one.
Tier selection using measured-but-uncontended costs missed deadlines under
load, because the cost model was blind to the one thing causing the misses.

So Triage tracks **interference** — the ratio of wall time to work time —
and inflates its estimate by it. Two design choices, both measured:

**Per priority band, not globally:**

| band | interference | what lives there |
|---|---|---|
| prio 32–47 | **259–328%** | detection, priority 40 |
| prio 0–15 | **149–184%** | chat, priority 10 |

Detection suffers proportionally *more* interference than chat, which is
the opposite of what priority alone suggests and obvious afterwards: a 5 ms
request that loses 10 ms is at 300%, while a 300 ms request that loses 84
ms is at 128%. Interference as a ratio is scale-relative, so one global
figure would have been wrong for both.

**A decaying maximum, not a mean.** A deadline is not met on average. It is
met or missed per request, and what breaks it is a bad moment, not a
typical one.

And the limit, stated plainly: interference can be **predicted** but not
**bounded**. Past roughly 340% nothing fits and the service degrades with
no floor under it. Which is why there is a step 7.

## Step 7 — a CPU ceiling the kernel enforces

**[`server/budget.c`](../server/budget.c), [`bench/sched_probe.c`](../bench/sched_probe.c)**

Low-priority AI work is switched to `SCHED_SPORADIC` for the duration: a
budget per replenishment period, after which the kernel **drops the
thread's priority itself**.

This was verified rather than assumed, and the distinction mattered —
`SCHED_SPORADIC` is declared in this SDP's `<sched.h>`, and so is
`SCHED_EXT_APS`, for a scheduler that no longer exists. A sporadic thread
at priority 40 with a 2 ms budget per 10 ms period, dropping to priority 5,
against a plain spinner at priority 10, on one CPU:

| | |
|---|---|
| CPU the sporadic thread actually got | **16.5%** |
| expected if enforced | ~20% |
| expected if ignored | ~100% (priority 40 beats 10) |

Those differ by a factor of five, so there is no boundary to argue about. A
ceiling the microkernel enforces is strictly stronger than a token bucket
in your own code: **your accounting can only throttle work you have not
started, while the kernel can take the CPU away from work already
running.**

> **Two traps.** Adaptive Partitioning, which the original design used for
> this, **was discontinued in QNX SDP 8.0** — the QoS packages are not
> available under the non-commercial licence and `mkqnximage` offers no
> partitioning option. And a project header named `sched.h` on the include
> path shadowed the system `<sched.h>`, hiding `SCHED_SPORADIC` and
> `SCHED_RR` entirely; the errors name the struct member rather than the
> feature, which sends you looking in the wrong place. You also need
> `-D_QNX_SOURCE` for the sporadic fields of `struct sched_param`.

## Step 8 — stop work nobody is waiting for

**[`server/cancel.c`](../server/cancel.c), `io_unblock` in [`server/main.c`](../server/main.c)**

If a client is killed or times out, QNX sends an **unblock pulse**. Finish
its request anyway and you burn a whole tier of CPU — at the dead client's
*inherited* priority — for an answer nobody will read.

```c
static int io_unblock(resmgr_context_t *ctp, io_pulse_t *msg,
                      RESMGR_OCB_T *ocb)
{
    if (cancel_mark(ctp->rcvid))
        return _RESMGR_NOREPLY;   /* the worker finishes the handshake */
    return iofunc_unblock_default(ctp, msg, ocb);
}
```

This runs on a *different* pool thread from the one doing the work, which
is the whole reason it can help — and that only works because of step 3.
The worker notices at its next chunk boundary and returns `EINTR`: the same
decision point the deadline check already uses, for a second purpose.

## Step 9 — prove it against a fair baseline

**[`bench/fcfs.c`](../bench/fcfs.c), [`docs/benchmark.md`](../docs/benchmark.md)**

A claim like this is worth nothing without a comparison, and the comparison
is worth nothing unless the thing compared against is the strongest
*honest* version of the conventional approach. So the baseline is the same
engine, the same transport, the same protocol, the same thread pool, the
same clients and the same boot — and the headline difference is step 2's
one flag: its channel is created **with** `_NTO_CHF_FIXED_PRIORITY`.

It also has to do real work to be first-come-first-served. QNX delivers the
*highest-priority* waiting message first, so true FCFS takes
`_RESMGR_NOREPLY` plus `MsgReply()` from a worker in arrival order.
Otherwise the baseline quietly inherits a scheduling advantage it has not
earned, and the comparison *understates* the difference.

100 detection requests, priority 40, 20 ms deadline, under a four-thread
chat flood:

| deadline hit rate | load 300 | load 500 | load 800 |
|---|---|---|---|
| **Triage** | **100%** | **99%** | **97%** |
| same pool + tier, no inheritance | 93% | 71% | 23% |
| FCFS queue, Triage's own tiers | 73% | 82% | 88% |
| FCFS queue, medium tier | 22% | 36% | 0% |

The baseline's hit rate tracks how much spare CPU happens to be lying
around — 93% down to 23% on identical code — while Triage's barely moves,
because it is not relying on headroom. And the win is not "Triage runs a
smaller model": it still wins when the baseline is handed Triage's own tier
choices.

> **Read [`docs/benchmark.md`](../docs/benchmark.md) for the three ways
> this comparison was wrong before it was right** — including a baseline
> that was accidentally inheriting priority and therefore "beat" Triage
> 100% to 99%, which is the `dispatch_create_channel` trap from step 2
> doing real damage.
>
> **These figures predate the step 3 fix** and use the server's own
> latency, so they are being re-measured with the client-side round trip.
> The relative result is not in doubt — the baseline's failure mode is
> queueing behind a 300 ms request, which no clock choice hides — but the
> absolute Triage numbers were flattering and will move.

---

## What to take away

If you remember four things:

1. **A synchronous `MsgSend()` runs the receiver at the sender's
   priority.** Make your service a resource manager and urgency crosses
   the process boundary with no code on either side — for every client on
   the system, including ones you did not write. `dispatch_create()` gives
   you this by default; `_NTO_CHF_FIXED_PRIORITY` is how you lose it.

2. **The mechanism needs somewhere to land.** A thread pool, *and*
   handlers the framework is not serialising behind your back. Either one
   missing costs you the entire effect, and the default hands you the
   second problem for free.

3. **Chunk your inference.** Preemption is the kernel's job; being able to
   *stop early and still answer* is yours, and it is what turns a deadline
   into a quality knob instead of a failure.

4. **Report the evidence, do not assert the conclusion.** Every reply here
   carries both priorities, the tier, the interference estimate and the
   detector's ground truth, and the clients check them. Every bug in this
   list was caught by a number that disagreed with another number.

## Where the claims are weak

- **The detector is real inference, but it is not a pedestrian detector.**
  A 14×14 MLP on digits stands in for the vision model a shipping system
  would run. The arithmetic, the cost, the chunk granularity and the
  accuracy are real; the task is a stand-in. **Chat has no model at all**
  and is still a calibrated load.
- **QEMU on a laptop steals CPU from the guest in ~9 ms lumps**, which is
  comparable to a 20 ms deadline. Absolute latencies from a QEMU target are
  not worth much, which is why claims are made *differentially*, with both
  sides of a comparison eating the same theft. Bare-metal numbers are still
  owed.
- **The CPU ceiling is verified enforced, but its end-to-end effect is
  not demonstrated.** On a single-CPU QEMU target, more AI load kept giving
  the low-priority control loop *more* CPU, not less. That is written down
  as an open item rather than tuned until it looked right.
- **The benchmark predates the step 3 fix.** See step 9.

## Where to go next

- [`docs/architecture.md`](../docs/architecture.md) — the design decisions
  and the post-mortems, including the ones above in full.
- [`docs/benchmark.md`](../docs/benchmark.md) — the comparison, its threats
  to validity, and how to reproduce it.
- [`server/main.c`](../server/main.c) — step 3's server, grown up.
