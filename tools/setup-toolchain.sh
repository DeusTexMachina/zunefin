#!/bin/sh
# Download the prebuilt CeGCC 9.3 arm-mingw32ce toolchain (Linux x86_64 or aarch64)
# from brain-hackers/cegcc-build into $CEGCC (default /opt/cegcc).
set -e
CEGCC=${CEGCC:-/opt/cegcc}
TAG=2022-10-26-225811
case "$(uname -m)" in
  x86_64) ARCH=x86_64 ;;
  aarch64|arm64) ARCH=aarch64 ;;
  *) echo "unsupported host $(uname -m)"; exit 1 ;;
esac
[ "$(uname -s)" = Linux ] || echo "note: these builds are for Linux hosts (use Docker or a Linux VM on macOS)"
TMP=$(mktemp -d)
curl -fL -o "$TMP/cegcc.zip" "https://github.com/brain-hackers/cegcc-build/releases/download/$TAG/cegcc-$ARCH-$TAG.zip"
mkdir -p "$TMP/x" && unzip -q "$TMP/cegcc.zip" -d "$TMP/x"
mkdir -p "$(dirname "$CEGCC")" && rm -rf "$CEGCC" && mv "$TMP/x/cegcc" "$CEGCC"
rm -rf "$TMP"
"$CEGCC/bin/arm-mingw32ce-gcc" --version | head -1
