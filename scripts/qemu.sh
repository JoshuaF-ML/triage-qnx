#!/bin/sh
#
# qemu.sh -- build and run a QNX 8.0 QEMU target with Triage inside it.
#
# Run this on the development host, after `make stage`. It wraps
# mkqnximage, the SDP tool that builds a bootable QNX image and can boot
# it under QEMU.
#
#   ./scripts/qemu.sh build     # create the image, with stage/ copied in
#   ./scripts/qemu.sh run       # boot it
#   ./scripts/qemu.sh ssh       # shell into the running target
#   ./scripts/qemu.sh stop
#
# NOTE (week 1, before anything else): mkqnximage flags have changed
# between SDP releases. Run `mkqnximage --help` once and reconcile it with
# the flags below before trusting this script. That check is part of the
# setup gate, not a detail to discover in week 3.

set -eu

ARCH="${ARCH:-x86_64}"
IMGDIR="${IMGDIR:-images/$ARCH}"
STAGE="stage/$ARCH"
HOSTNAME_="${HOSTNAME_:-triage}"

need_sdp() {
    if ! command -v mkqnximage >/dev/null 2>&1; then
        echo "error: mkqnximage not on PATH." >&2
        echo "       source your SDP environment first, e.g." >&2
        echo "         . ~/qnx800/qnxsdp-env.sh" >&2
        exit 1
    fi
}

case "${1:-}" in
build)
    need_sdp
    if [ ! -d "$STAGE" ]; then
        echo "error: $STAGE does not exist -- run 'make stage' first" >&2
        exit 1
    fi
    mkdir -p "$IMGDIR"
    cd "$IMGDIR"
    # --data copies a host directory into /data on the target, which is
    # where we will find the Triage binaries after boot.
    mkqnximage \
        --type=qemu \
        --arch="$ARCH" \
        --hostname="$HOSTNAME_" \
        --data-size=200 \
        --data="$OLDPWD/$STAGE" \
        --ssh-ident="$HOME/.ssh/id_rsa.pub" \
        --force
    echo
    echo "image built in $IMGDIR"
    ;;
run)
    need_sdp
    cd "$IMGDIR"
    mkqnximage --run
    ;;
ssh)
    # mkqnximage's default QEMU networking forwards 22 to 6022 on the host.
    ssh -p "${SSH_PORT:-6022}" -o StrictHostKeyChecking=no root@localhost
    ;;
stop)
    need_sdp
    cd "$IMGDIR"
    mkqnximage --stop || true
    ;;
*)
    echo "usage: $0 {build|run|ssh|stop}" >&2
    exit 1
    ;;
esac
