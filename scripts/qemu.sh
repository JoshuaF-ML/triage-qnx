#!/bin/bash
#
# qemu.sh -- build, boot and load a QNX 8.0 QEMU target for Triage.
#
#   ./scripts/qemu.sh build     # create the VM image (mkqnximage)
#   ./scripts/qemu.sh run       # boot it headless
#   ./scripts/qemu.sh ssh       # shell into the target
#   ./scripts/qemu.sh push      # copy built binaries to /data on target
#   ./scripts/qemu.sh smoke     # push, then run the smoke test on target
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

# One CPU by default, on purpose. With several cores an urgent request can
# simply run on another core, and "it preempted the chat inference" becomes
# unfalsifiable -- nothing was preempted, there was just a spare CPU. On a
# single core the kernel MUST take the CPU away from the low-priority work,
# which is the behaviour Triage exists to demonstrate. Raise it to compare.
CPUS="${CPUS:-1}"

REPO="$(cd "$(dirname "$0")/.." && pwd)"
STAGE="$REPO/build/$ARCH"

SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"

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
    qemu-system-x86_64 \
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
    [ -d "$STAGE" ] || { echo "error: $STAGE missing -- build first" >&2; exit 1; }
    # shellcheck disable=SC2086
    scp $SSH_OPTS -P "$SSH_PORT" \
        "$STAGE/triage" "$STAGE/brake_reflex" "$STAGE/chat_load" \
        "$STAGE/triage_stat" "$REPO/scripts/smoke.sh" \
        "$TARGET_USER@localhost:/data/"
    ssh_target "chmod +x /data/triage /data/brake_reflex /data/chat_load /data/triage_stat /data/smoke.sh"
    echo "pushed to /data"
    ;;

smoke)
    "$0" push
    ssh_target "cd /data && ./smoke.sh"
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
    echo "usage: $0 {build|run|console|ssh|push|smoke|stop}" >&2
    exit 1
    ;;
esac
