#!/bin/bash
#
# qemu.sh -- build, boot and load a QNX 8.0 QEMU target for Triage.
#
#   ./scripts/qemu.sh build     # create the VM image (mkqnximage)
#   ./scripts/qemu.sh run       # boot it headless
#   ./scripts/qemu.sh ssh       # shell into the target
#   ./scripts/qemu.sh push      # copy built binaries to /data on target
#   ./scripts/qemu.sh smoke     # push, then run the smoke test on target
#   ./scripts/qemu.sh bench     # push, then run the Triage-vs-FCFS bench
#   ./scripts/qemu.sh codelab   # push, then verify the codelab's steps
#   ./scripts/qemu.sh console   # tail the target's serial console
#   ./scripts/qemu.sh stop
#
# On a WSL2 host, run through scripts/wsl-env.sh so SDP is on PATH:
#   wsl -d Ubuntu -- bash scripts/wsl-env.sh bash scripts/qemu.sh build
#
# WHY WE DO NOT USE mkqnximage --run
# ----------------------------------
# SDP's own runimage script only offers bridged networking: both its 'nat'
# and 'bridge' modes end up at `-netdev bridge`, which needs
# qemu-bridge-helper to be setuid root and a host bridge interface to
# exist. Under WSL2 that means root for every boot and a bridge that does
# not survive much. QEMU's user-mode ("slirp") networking needs neither:
# it gives the guest DHCP and NAT in userspace, and we forward one host
# port to the target's SSH. So we invoke QEMU ourselves, mirroring the
# arguments runimage uses for x86_64, with the network backend swapped.
#
# The cost is that the target is not reachable from the LAN. We only ever
# need SSH from this host, so that costs us nothing.

set -eu

ARCH="${ARCH:-x86_64}"
IMGDIR="${IMGDIR:-$HOME/qnx-images/triage-$ARCH}"
HOST_NAME="${HOST_NAME:-triage}"
RAM="${RAM:-2G}"
TARGET_USER="${TARGET_USER:-root}"
SSH_PORT="${SSH_PORT:-6022}"

# The target's sshd has no password auth for root -- mkqnximage bakes a
# public key into /data/home/root/.ssh/authorized_keys and that is the only
# way in (--ssh-ident). A throwaway key is fine and is what we use: it
# guards a local VM reachable only from this host's loopback.
SSH_KEY="${SSH_KEY:-$HOME/.ssh/id_ed25519}"

# One CPU by default, on purpose. With several cores an urgent request can
# simply run on another core, and "it preempted the chat inference" becomes
# unfalsifiable -- nothing was preempted, there was just a spare CPU. On a
# single core the kernel MUST take the CPU away from the low-priority work,
# which is the behaviour Triage exists to demonstrate. Raise it to compare.
CPUS="${CPUS:-1}"

REPO="$(cd "$(dirname "$0")/.." && pwd)"
STAGE="$REPO/stage/$ARCH"   # where `make stage` puts the binaries

SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR
          -o IdentitiesOnly=yes -o BatchMode=yes -i $SSH_KEY"

need_sdp() {
    command -v mkqnximage >/dev/null 2>&1 || {
        echo "error: mkqnximage not on PATH -- run via scripts/wsl-env.sh" >&2
        exit 1
    }
}

ssh_target() {
    # shellcheck disable=SC2086
    ssh $SSH_OPTS -p "$SSH_PORT" "$TARGET_USER@localhost" "$@"
}

case "${1:-}" in
build)
    need_sdp
    if [ ! -f "$SSH_KEY.pub" ]; then
        echo "error: no public key at $SSH_KEY.pub" >&2
        echo "  generate one with: ssh-keygen -q -t ed25519 -N '' -f $SSH_KEY" >&2
        exit 1
    fi
    mkdir -p "$IMGDIR"
    cd "$IMGDIR"
    mkqnximage \
        --type=qemu \
        --arch="$ARCH" \
        --hostname="$HOST_NAME" \
        --cpu="$CPUS" \
        --ram="$RAM" \
        --ip=dhcp \
        --data-size=200 \
        --ssh-ident="$SSH_KEY.pub" \
        --noprompt \
        --force \
        --clean \
        --build
    echo "image built in $IMGDIR (cpu=$CPUS ram=$RAM)"
    ;;

run)
    cd "$IMGDIR"
    if [ -f output/qemu.pid ] && kill -0 "$(cat output/qemu.pid)" 2>/dev/null; then
        echo "already running (pid $(cat output/qemu.pid))"
        exit 0
    fi

    if [ -w /dev/kvm ]; then
        accel=(--enable-kvm --cpu host)
    else
        echo "warning: no write access to /dev/kvm -- emulating, will be slow"
        accel=(--cpu max)
    fi

    rm -f output/qemu.out
    # setsid + nohup: without them QEMU is a child of this shell, and when
    # the shell that started us exits -- which it does immediately when the
    # script is driven from Windows via `wsl -- bash ...` -- the VM dies with
    # it. The target has to outlive the command that booted it.
    setsid nohup qemu-system-x86_64 \
        -smp "$CPUS" "${accel[@]}" \
        -m "$RAM" \
        -drive file=output/disk-qemu.vmdk,if=ide,id=drv0 \
        -netdev "user,id=net0,hostfwd=tcp::$SSH_PORT-:22" \
        -device virtio-net-pci,netdev=net0 \
        -object rng-random,filename=/dev/urandom,id=rng0 \
        -device virtio-rng-pci,rng=rng0 \
        -kernel output/ifs.bin \
        -pidfile output/qemu.pid \
        -nographic \
        -serial file:output/qemu.out \
        >/dev/null 2>&1 </dev/null &

    echo "booting; serial console -> $IMGDIR/output/qemu.out"
    for i in $(seq 1 40); do
        sleep 2
        if ssh_target true 2>/dev/null; then
            echo "target up: ssh -p $SSH_PORT $TARGET_USER@localhost"
            exit 0
        fi
    done
    echo "error: target did not answer SSH on port $SSH_PORT within 80s" >&2
    echo "last console output:" >&2
    tail -30 output/qemu.out >&2 || true
    exit 1
    ;;

console)
    tail -f "$IMGDIR/output/qemu.out"
    ;;

ssh)
    shift || true
    ssh_target "$@"
    ;;

push)
    [ -d "$STAGE" ] || { echo "error: $STAGE missing -- run 'make stage' first" >&2; exit 1; }
    # shellcheck disable=SC2086
    scp $SSH_OPTS -P "$SSH_PORT" \
        "$STAGE/triage" "$STAGE/fcfs" "$STAGE/brake_reflex" \
        "$STAGE/chat_load" "$STAGE/triage_stat" "$STAGE/control_loop" \
        "$REPO/scripts/smoke.sh" "$REPO/bench/run.sh" \
        "$TARGET_USER@localhost:/data/"
    ssh_target "chmod +x /data/triage /data/fcfs /data/brake_reflex \
                         /data/chat_load /data/triage_stat /data/control_loop \
                         /data/smoke.sh /data/run.sh"
    echo "pushed to /data"
    ;;

codelab)
    # The codelab's three step programs build standalone, so they are
    # pushed and verified separately from the project proper -- a reader
    # following the tutorial has not built Triage yet.
    CLAB="$REPO/codelab/build/$ARCH"
    [ -d "$CLAB" ] || { echo "error: $CLAB missing -- run 'make -C codelab' first" >&2; exit 1; }
    ssh_target "mkdir -p /data/codelab"
    # shellcheck disable=SC2086
    scp $SSH_OPTS -P "$SSH_PORT"         "$CLAB/step1_hello" "$CLAB/step2_prio" "$CLAB/step3_pool"         "$CLAB/ask" "$REPO/codelab/verify.sh"         "$TARGET_USER@localhost:/data/codelab/"
    ssh_target "chmod +x /data/codelab/*"
    ssh_target "cd /data/codelab && ./verify.sh"
    ;;

smoke)
    "$0" push
    ssh_target "cd /data && ./smoke.sh"
    ;;

bench)
    shift || true
    "$0" push
    # shellcheck disable=SC2086,SC2048
    ssh_target "cd /data && ./run.sh $*"
    ;;

stop)
    cd "$IMGDIR"
    if [ -f output/qemu.pid ]; then
        kill "$(cat output/qemu.pid)" 2>/dev/null || true
        rm -f output/qemu.pid
        echo "stopped"
    else
        echo "not running"
    fi
    ;;

*)
    echo "usage: $0 {build|run|console|ssh|push|smoke|bench|codelab|stop}" >&2
    exit 1
    ;;
esac
