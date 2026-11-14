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

# An apt run killed partway through leaves dpkg half-configured, and every
# later install refuses until it is repaired. Cheap to run when it is not
# needed, so always run it.
echo "==> repairing any interrupted package state"
dpkg --configure -a || true

echo "==> updating package lists"
apt-get update -qq

# This distro arrived with a half-finished upgrade: vim, vim-runtime and
# vim-tiny at mismatched versions. apt refuses to install ANYTHING while
# unmet dependencies exist, so the repair has to happen before QEMU.
echo "==> fixing broken dependencies"
apt-get --fix-broken install -y || true
apt-get install -y --only-upgrade vim vim-runtime vim-tiny || true
dpkg --configure -a || true

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
