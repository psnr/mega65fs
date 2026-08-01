#!/bin/sh
#
# Refresh lib/libm65ftp/ from the upstream MEGA65 tools sources (Linux).
#
# The files under lib/libm65ftp/ are a full copy of the upstream MEGA65 tools
# library (MEGA65/mega65-tools).  This script re-copies the files we vendor and
# regenerates the etherload byte arrays from their assembly sources, so that
# bug fixes from upstream can be picked up.  It fails loudly if the upstream
# tree does not match the SHA pinned in lib/libm65ftp/README.md so the local
# deltas get re-reviewed.
#
# Requires: git, cc (gcc) and python3 on PATH.
#
# Usage:
#     scripts/sync-upstream.sh            # clone upstream + Ophis as needed
#     scripts/sync-upstream.sh <dir>      # use an existing upstream checkout
#
# Environment:
#     UPSTREAM_URL   git URL of the upstream repo (default: MEGA65/mega65-tools)
#     UPSTREAM_BRANCH  branch to clone (default: development)
#     OPHIS_URL      git URL of the Ophis assembler (default: gardners/Ophis)
#     OPHIS_BIN      path to an existing Ophis bin/ophis script to skip the clone
#     PYTHON         python interpreter used to run Ophis (default: python3)

set -eu

UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/MEGA65/mega65-tools.git}"
UPSTREAM_BRANCH="${UPSTREAM_BRANCH:-development}"
OPHIS_URL="${OPHIS_URL:-https://github.com/gardners/Ophis.git}"
PINNED_SHA="$(sed -n 's/^Pinned upstream: //p' lib/libm65ftp/README.md)"

PYTHON="${PYTHON:-python3}"
if ! command -v "$PYTHON" >/dev/null 2>&1; then
    PYTHON=python
fi
if ! command -v "$PYTHON" >/dev/null 2>&1; then
    echo "error: need python3 (or python) to run Ophis" >&2
    exit 1
fi

here=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$here"

# Work directory for the generated files and the Ophis clone.
work=$(mktemp -d "${TMPDIR:-/tmp}/mega65fs-sync.XXXXXX")
trap 'rm -rf "$work"' EXIT

if [ $# -ge 1 ]; then
    upstream=$1
elif [ -d upstream/.git ]; then
    upstream=upstream
else
    upstream=$work/upstream
    git clone --quiet --no-recurse-submodules "$UPSTREAM_URL" "$upstream"
fi

# Honor the SHA pinned in lib/libm65ftp/README.md (a "a...b" range marker
# skips the check).
if [ -n "${PINNED_SHA}" ] && [ -z "${PINNED_SHA##*...*}" ]; then
    :
elif [ -n "${PINNED_SHA}" ]; then
    git -C "$upstream" checkout --quiet "$PINNED_SHA"
    echo "pinned at ${PINNED_SHA}"
else
    echo "warning: no pin in lib/libm65ftp/README.md; syncing ${UPSTREAM_BRANCH} HEAD" >&2
fi

# Verbatim upstream files (upstream path -> vendored path).  These are copied
# as-is; the local deltas documented in lib/libm65ftp/README.md are re-applied
# on top.
set -- \
    src/tools/mega65_ftp.c:lib/libm65ftp/mega65_ftp.c \
    src/tools/m65common.c:lib/libm65ftp/m65common.c \
    src/tools/logging.c:lib/libm65ftp/logging.c \
    src/tools/filehost.c:lib/libm65ftp/filehost.c \
    src/tools/filehost.h:lib/libm65ftp/filehost.h \
    src/tools/diskman.c:lib/libm65ftp/diskman.c \
    src/tools/diskman.h:lib/libm65ftp/diskman.h \
    src/tools/bit2mcs.c:lib/libm65ftp/bit2mcs.c \
    src/tools/etherload/etherload_common.c:lib/libm65ftp/etherload/etherload_common.c \
    include/dirtymock.h:lib/libm65ftp/include/dirtymock.h \
    include/etherload_common.h:lib/libm65ftp/include/etherload_common.h \
    include/fpgajtag.h:lib/libm65ftp/include/fpgajtag.h \
    include/logging.h:lib/libm65ftp/include/logging.h \
    include/m65common.h:lib/libm65ftp/include/m65common.h

for pair in "$@"; do
    src=${pair%%:*}
    dst=${pair#*:}
    if [ ! -f "$upstream/$src" ]; then
        echo "error: missing upstream file $upstream/$src" >&2
        exit 1
    fi
    mkdir -p "$(dirname "$dst")"
    cp "$upstream/$src" "$dst"
    echo "  copied $dst"
done

# Rebuild the etherload byte arrays from the assembly sources using Ophis and
# the vendored copies of upstream's bin2c/map2h (see scripts/).  The .a65
# sources are Ophis assembly; they are assembled with -4 (45GS10) exactly as
# the upstream Makefile does.  All five ethlets are regenerated: the Makefile
# builds every one of them.
if [ -z "${OPHIS_BIN:-}" ] && command -v ophis >/dev/null 2>&1; then
    OPHIS_BIN=$(command -v ophis)
fi
if [ -z "${OPHIS_BIN:-}" ]; then
    git clone --quiet "$OPHIS_URL" "$work/ophis"
    OPHIS_BIN="$work/ophis/bin/ophis"
fi

cc -O2 -o "$work/bin2c" scripts/bin2c.c
cc -O2 -o "$work/map2h" scripts/map2h.c

for name in dma_load echo all_done_basic2 all_done_basic65 all_done_jump; do
    asm="$upstream/src/tools/etherload/ethlet_$name.a65"
    if [ ! -f "$asm" ]; then
        echo "error: missing upstream ethlet source $asm" >&2
        exit 1
    fi
    "$PYTHON" "$OPHIS_BIN" -4 --no-warn "$asm" -l "$work/$name.list" \
        -m "$work/$name.map" -o "$work/$name.bin"
    "$work/bin2c" "$work/$name.bin" "ethlet_$name" \
        "lib/libm65ftp/etherload/ethlet_$name.c"
    "$work/map2h" "$work/$name.map" "ethlet_$name" \
        "lib/libm65ftp/etherload/ethlet_${name}_map.h"
    echo "  regenerated lib/libm65ftp/etherload/ethlet_$name.c (+_map.h)"
done

# lib/libm65ftp/ftphelper.c and ftphelper_eth.c are NOT synced: they are
# generated upstream from src/utilities/remotesd.c / remotesd_eth.c via the
# cc65 cross-compiler (the Makefile rules `bin2c remotesd.prg helperroutine
# ftphelper.c` and `bin2c remotesd_eth.prg helperroutine_eth ftphelper_eth.c`).
# Regenerating them needs a cc65 + mega65-libc toolchain, so they stay pinned
# as checked in.
for helper in ftphelper.c ftphelper_eth.c; do
    if [ ! -f "lib/libm65ftp/$helper" ]; then
        echo "error: lib/libm65ftp/$helper is missing and cannot be regenerated" >&2
        echo "        it is built upstream via cc65 (remotesd.c/remotesd_eth.c); restore it from git." >&2
        exit 1
    fi
done

echo
echo "DONE.  Re-apply the local deltas documented in lib/libm65ftp/README.md:"
sed -n '/^## Local deltas/,$p' lib/libm65ftp/README.md
