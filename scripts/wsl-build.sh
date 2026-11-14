#!/bin/bash
#
# wsl-build.sh -- build Triage from a WSL2 Ubuntu host.
#
#   wsl -d Ubuntu -- bash /mnt/c/.../Triage/scripts/wsl-build.sh [make args]
#
# SDP 8.0's qnxsdp-env.sh does not export into non-interactive shells, so
# this sets QNX_HOST / QNX_TARGET itself. Override QNX_SDP if you installed
# somewhere other than ~/qnx800.

set -u

QNX_SDP="${QNX_SDP:-$HOME/qnx800}"

if [ ! -d "$QNX_SDP" ]; then
    echo "error: no SDP at $QNX_SDP (set QNX_SDP)" >&2
    exit 1
fi

export QNX_HOST="$QNX_SDP/host/linux/x86_64"
export QNX_TARGET="$QNX_SDP/target/qnx"
export MAKEFLAGS="-I$QNX_TARGET/usr/include"
export PATH="$QNX_HOST/usr/bin:$QNX_SDP/host/common/bin:$PATH"

if ! command -v qcc >/dev/null 2>&1; then
    echo "error: qcc not found under $QNX_HOST/usr/bin" >&2
    exit 1
fi

# The repo is the parent of this script's directory.
cd "$(dirname "$0")/.." || exit 1

echo "QNX_HOST=$QNX_HOST"
echo "repo=$(pwd)"
echo

exec make "$@"
