# Triage

**An AI inference service for QNX 8.0 where the operating system enforces urgency.**

Every request to Triage carries a priority and a deadline. A life-critical
question — *"is that a pedestrian?"* — preempts a chatbot question
mid-computation and still answers on time.

> Robots are getting AI brains. Triage makes sure the urgent thought gets
> the CPU first.

---

## The problem

A robot or a car has one AI brain and many askers. The brake controller
asks "is that a person?" while the voice assistant asks "what's the
weather?" Standard inference servers — llama.cpp's server, Triton, every
HTTP model endpoint — serve requests first come, first served. They have
no idea that one of those questions has 20 milliseconds to live and the
other has two seconds.

QNX's own 2026 survey of 1,000 robotics developers found that 89% consider
physical AI critical, while only 29% feel confident they can make safe,
predictable decisions in the real world. That gap is a scheduling problem
wearing an AI costume.

## The approach

Triage is not a priority queue bolted onto an inference server. It is a
**QNX resource manager**, and it leans on something the microkernel
already does.

When a client calls `devctl()` on `/dev/triage`, QNX turns it into a
`MsgSend()` and the server thread that receives it **runs at the priority
of the calling thread**. We do not set that priority. We do not look it
up. We could not prevent it if we tried. It is what the kernel does with a
synchronous message on a channel created without `_NTO_CHF_FIXED_PRIORITY`.

So the brake controller's question is not *tagged* urgent — inside Triage
it **is** priority-40 work, and the kernel will take the CPU away from the
chat inference running at priority 10 to do it. Urgency crosses the process
boundary for free.

On top of that, Triage adds the part the OS cannot know: **how much AI to
run.** Each request states its deadline, and Triage picks the largest model
tier whose measured cost still fits — a rough answer on time rather than a
perfect answer too late.

## Status

Stage 1 of 5. This is a hackathon project in progress (QNX Global
Hackathon, submission 14 Dec 2026).

| Piece | State |
|---|---|
| `/dev/triage` resource manager, thread pool, devctl protocol | built |
| Client library with priority + deadline | built |
| Chunked compute with preemption accounting | built |
| Deadline-driven tier selection with an adaptive cost model | built |
| Text stats / counters | built |
| Real inference backend (llama.cpp + small detector) | **stand-in load for now** |
| First-come-first-served baseline server for comparison | planned, week 2 |
| Car sim + web dashboard | planned, week 3 |
| CPU budget protection | redesigned — see below |
| Benchmark numbers on hardware | planned, week 4 |

The compute engine currently burns a calibrated number of microseconds per
chunk instead of running a real model. This is honest scaffolding, not a
hidden shortcut: the scheduling behaviour being demonstrated is identical,
and `server/engine.c` is the single file a real runtime plugs into. It will
be replaced before submission.

## Build

Needs **QNX SDP 8.0** and its toolchain on `PATH`.

```bash
source ~/qnx800/qnxsdp-env.sh
make info          # confirms qcc is found
make               # builds for x86_64 (the QEMU target)
make stage         # collects binaries into stage/x86_64/
```

For a Raspberry Pi 4/5 target:

```bash
make ARCH=aarch64le stage
```

### Building from Windows via WSL2

SDP 8.0's `qnxsdp-env.sh` does not export into non-interactive shells, so
driving the build from Windows needs the environment set explicitly.
[`scripts/wsl-build.sh`](scripts/wsl-build.sh) does that:

```bash
wsl -d Ubuntu -- bash scripts/wsl-build.sh
wsl -d Ubuntu -- bash scripts/wsl-build.sh ARCH=aarch64le
```

[`scripts/wsl-env.sh`](scripts/wsl-env.sh) runs any SDP command the same
way — `mkqnximage`, `qcc`, `mkifs`:

```bash
wsl -d Ubuntu -- bash scripts/wsl-env.sh mkqnximage --help
```

Booting a QEMU target also needs QEMU itself, which SDP does not install.
Once, as root:

```bash
wsl -d Ubuntu -- sudo bash scripts/wsl-deps.sh
```

## Run

Build and boot a QNX 8.0 QEMU image with the binaries inside it:

```bash
./scripts/qemu.sh build
./scripts/qemu.sh run
./scripts/qemu.sh ssh
```

Then, on the target:

```bash
cd /data
./smoke.sh
```

`smoke.sh` is the proof-of-life script: it checks that `/dev/triage` was
created, that requests are answered, that the server ran the work at the
*client's* priority, and that high-priority deadlines survive a
low-priority flood.

To drive it by hand:

```bash
./triage -v &                              # the service
./brake_reflex -P 40 -d 20000 -p 50000     # urgent client, 20ms deadline
./chat_load -n 4 -P 10 -s 30               # four greedy chat clients
./triage_stat -w 1                         # counters, once a second
```

Raising a thread's priority needs the `PROCMGR_AID_PRIORITY` ability, so
run the demos as root on a stock image.

## Evidence of QNX use

Triage is not a portable program with a QNX build target. It cannot be
written on Linux — the mechanism it is built on does not exist there.

| QNX feature | Where | What it does for us |
|---|---|---|
| **Resource manager** | `resmgr_attach()` in [`server/main.c`](server/main.c) `main()` | Creates `/dev/triage`. The AI becomes a device any process can open. |
| **Native message passing** | `devctl()` in [`client/libtriage.c`](client/libtriage.c) `triage_infer()`, received by `io_devctl()` in [`server/main.c`](server/main.c) | Request and reply with no broker, no socket, no serialisation. |
| **Priority inheritance** | `ctp->info.priority` vs `pthread_getschedparam()` in `io_devctl()`, [`server/main.c`](server/main.c) | The core of the project. Both numbers are reported in every reply, so the inheritance is observable from the client, not just asserted in a README. |
| **Channel semantics** | `dispatch_create()` in `main()`, deliberately *without* `_NTO_CHF_FIXED_PRIORITY` | The one line that makes inheritance happen. There is a comment there telling future contributors not to "fix" it. |
| **Thread pool** | `thread_pool_create()` in `main()` | Inheritance only buys anything if an urgent message can be received while a lazy request is still computing. |
| **Cycle-accurate timing** | `ClockCycles()` and `SYSPAGE_ENTRY(qtime)->cycles_per_sec` in [`server/engine.c`](server/engine.c) | Microsecond measurement without syscall overhead in the measurement path. |
| **CPU budget enforcement** | planned — see note below | A ceiling on low-priority AI, so chat can never starve control loops even in pathological cases. |

> **Note on Adaptive Partitioning.** The original design used QNX Adaptive
> Partitioning for this. It cannot: **APS was discontinued in QNX SDP 8.0**
> and QNX OS 8.0 does not support it. `mkqnximage` on SDP 8.0 offers no
> partitioning option, and the QoS packages are not available under the
> non-commercial licence. The budget ceiling is therefore being rebuilt on
> what 8.0 does have — see [`docs/architecture.md`](docs/architecture.md).

Every reply contains `client_prio` and `server_prio`
([`common/triage_proto.h`](common/triage_proto.h)). Run `./brake_reflex -P 40`
and then `./brake_reflex -P 12` and watch both numbers move together.
That is the mechanism, visible from userspace, in one command.

## Repository layout

```
common/     wire protocol shared by client and server
server/     the resource manager, the chunked engine, the tier scheduler
client/     the client library: three functions, one header
demo/       urgent client, greedy client, counters
bench/      baseline server and benchmark scripts   (week 2)
dashboard/  live web UI                             (week 3)
docs/       architecture notes and the codelab
scripts/    QEMU image build, on-target smoke test
```

## Design notes

[`docs/architecture.md`](docs/architecture.md) covers why preemption is the
kernel's job and not ours, what the chunk loop is actually for, how the
cost model adapts, and which claims we have not earned yet.

## License

MIT — see [LICENSE](LICENSE).
