#!/bin/sh
set -eu

: "${PS2DEV:?Set PS2DEV to the PS2 toolchain directory}"
: "${PS2SDK:?Set PS2SDK to the PS2SDK directory}"

PATH="$PS2DEV/bin:$PS2DEV/ee/bin:$PS2DEV/iop/bin:$PS2SDK/bin:$PATH"
export PATH PS2DEV PS2SDK

cd "$(dirname "$0")/.."
make clean all
