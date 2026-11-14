#!/bin/bash
#
# qemu.sh -- build, boot and load a QNX 8.0 QEMU target for Triage.
#
#   ./scripts/qemu.sh build     # create the VM image
#   ./scripts/qemu.sh run       # boot it (headless)
#   ./scripts/qemu.sh ip        # print the target's IP
#   ./scripts/qemu.sh push      # copy the built binaries to /data on target
#   ./scripts/qemu.sh ssh       # shell into the target
#   ./scripts/qemu.sh smoke     # push, then run the smoke test on target
#   ./scripts/qemu.sh stop
#
# Every flag here was read from `mkqnximage --help` on SDP 8.0, not guessed.
# Binaries get in over SSH after boot rather than baked into the image:
# mkqnximage's --extra-dirs is for its own content files, and rebuilding an
# image for every code change would be slow and painful.
#
# On a WSL2 host run this through scripts/wsl-env.sh so the SDP environment
# is set:
#   wsl -d Ubuntu -- bash scripts/wsl-env.sh ./scripts/qemu.sh build

set -eu

ARCH="${ARCH:-x86_64}"
IMGDIR="${IMGDIR:-$HOME/qnx-images/triage-$ARCH}"
HOST_NAME="${HOST_NAME:-triage}"
RAM="${RAM:-2G}"
TARGET_USER="${TARGET_USER:-root}"

# One CPU by default, on purpose. With several cores an urgent request can
# simply run on another core, and "it preempted the chat inference" becomes
# unfalsifiable -- nothing was preempted, there was just a spare CPU. On a
# single core the kernel MUST take the CPU away from the low-priority work,
# which is the behaviour Triage exists to demonstrate. Raise it to compare.
CPUS="${CPUS:-1}"

REPO="$(cd "$(dirname "$0")/.." && pwd)"
STAGE="$REPO/build/$ARCH"

need_sdp() {
    if ! command -v mkqnximage >/dev/null 2>&1; then
        echo "error: mkqnximage not on PATH." >&2
        echo "       run this through scripts/wsl-env.sh, or source" >&2
        echo "       ~/qnx800/qnxsdp-env.sh in an interactive shell." >&2
        exit 1
    fi
}

target_ip() {
    (cd "$IMGDIR" && mkqnximage --getip 2>/dev/null | tr -d '\r' | tail -1)
}

ssh_target() {
    ip="$(target_ip)"
    if [ -z "$ip" ]; then
        echo "error: no IP -- is the VM running? try: $0 run" >&2
        exit 1
    fi
    ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        "$TARGET_USER@$ip" "$@"
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
    echo
    echo "image built in $IMGDIR (cpu=$CPUS ram=$RAM)"
    ;;
run)
    need_sdp
    cd "$IMGDIR"
    mkqnximage --run=-h
    sleep 5
    echo "target IP: $(target_ip)"
    ;;
ip)
    need_sdp
    target_ip
    ;;
push)
    if [ ! -d "$STAGE" ]; then
        echo "error: $STAGE missing -- build first" >&2
        exit 1
    fi
    ip="$(target_ip)"
    echo "copying to $TARGET_USER@$ip:/data/"
    scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        "$STAGE/triage" "$STAGE/brake_reflex" "$STAGE/chat_load" \
        "$STAGE/triage_stat" "$REPO/scripts/smoke.sh" \
        "$TARGET_USER@$ip:/data/"
    ssh_target "chmod +x /data/triage /data/brake_reflex /data/chat_load /data/triage_stat /data/smoke.sh"
    echo "done"
    ;;
ssh)
    shift || true
    ssh_target "$@"
    ;;
smoke)
    "$0" push
    ssh_target "cd /data && ./smoke.sh"
    ;;
stop)
    need_sdp
    cd "$IMGDIR"
    mkqnximage --stop
    ;;
*)
    echo "usage: $0 {build|run|ip|push|ssh|smoke|stop}" >&2
    exit 1
    ;;
esac
