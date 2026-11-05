#!/bin/sh
# ZuneFin build helper.
#
# Builds for Windows CE on the Zune HD with GCC (arm-mingw32ce) and rejects any
# binary containing a misaligned word access. Windows CE runs with alignment
# faults on, and a GCC "-march=armv6" build can emit unaligned str/ldr that
# reboot the device (see docs/NOTES.md).
#
# usage: tools/build.sh out.{dll|exe} src.c [extra gcc args...]
#   CEGCC  toolchain root (default /opt/cegcc), see tools/setup-toolchain.sh
set -e
CEGCC=${CEGCC:-/opt/cegcc}
CC=$CEGCC/bin/arm-mingw32ce-gcc
OBJDUMP=$CEGCC/bin/arm-mingw32ce-objdump
[ -x "$CC" ] || { echo "no toolchain at $CEGCC (run tools/setup-toolchain.sh or set CEGCC)"; exit 1; }
OUT=$1; shift
case "$OUT" in *.dll) KIND=-shared ;; *) KIND= ;; esac
mkdir -p "$(dirname "$OUT")"
"$CC" -O2 -Wall -march=armv5te -mno-unaligned-access -static-libgcc $KIND -o "$OUT" "$@" -lws2
BAD=$("$OBJDUMP" -d "$OUT" | grep -E "\s(ldr|str)\s+r[0-9]+, \[[a-z0-9]+, #-?[0-9]+\]" \
      | awk -F'#' '{split($2,a,"]"); o=a[1]+0; if (o<0) o=-o; if (o%4) print}' | wc -l)
if [ "$BAD" -ne 0 ]; then echo "REJECTED $OUT: $BAD misaligned accesses"; rm -f "$OUT"; exit 1; fi
echo "ok $OUT"
