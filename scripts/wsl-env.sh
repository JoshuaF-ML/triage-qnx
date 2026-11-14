#!/bin/bash
#
# wsl-env.sh -- run any SDP 8.0 command with the toolchain environment set.
#
#   wsl -d Ubuntu -- bash .../scripts/wsl-env.sh mkqnximage --help
#   wsl -d Ubuntu -- bash .../scripts/wsl-env.sh qcc -V
#
# SDP 8.0's own qnxsdp-env.sh does not export into non-interactive shells,
# which is why this exists. Override QNX_SDP for a non-default install.

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

if [ "$#" -eq 0 ]; then
    echo "usage: $0 <command> [args...]" >&2
    exit 1
fi

exec "$@"
