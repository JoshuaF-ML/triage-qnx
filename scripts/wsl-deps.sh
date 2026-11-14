#!/bin/bash
#
# wsl-deps.sh -- install what mkqnximage needs to run a QNX VM under WSL2.
#
# Run once, as root:
#   wsl -d Ubuntu -- sudo bash scripts/wsl-deps.sh
#
# QNX SDP itself needs none of this; QEMU is only needed to BOOT the images
# mkqnximage builds.

set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "error: run me with sudo" >&2
    exit 1
fi

TARGET_USER="${SUDO_USER:-user}"

echo "==> updating package lists"
apt-get update -qq

echo "==> installing QEMU and networking tools"
apt-get install -y --no-install-recommends \
    qemu-system-x86 \
    qemu-utils \
    bridge-utils \
    net-tools \
    openssh-client

echo "==> adding $TARGET_USER to the kvm group"
if getent group kvm >/dev/null 2>&1; then
    usermod -aG kvm "$TARGET_USER"
    echo "    done -- $TARGET_USER must start a NEW shell for this to apply"
else
    echo "    no kvm group on this system; skipping"
fi

echo
echo "==> versions"
qemu-system-x86_64 --version | head -1
echo
echo "Done. KVM acceleration needs /dev/kvm to be readable by $TARGET_USER;"
echo "without it QEMU still runs, just slower, which is fine for a first boot"
echo "but not for the benchmark numbers."
