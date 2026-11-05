#!/bin/sh
# Build everything into build/.
set -e
cd "$(dirname "$0")/.."
tools/build.sh build/jfping.dll src/jfping.c
tools/build.sh build/jflaunch.dll src/jflaunch.c
tools/build.sh build/jfplay.exe src/jfplay.c -DJF_EXE -ltoolhelp
tools/build.sh build/zfstream.dll src/zfstream.c
