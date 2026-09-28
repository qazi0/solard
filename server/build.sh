#!/bin/sh
# Build solard (the dashboard page is compiled into the binary).
#
#   server/build.sh              native build for this machine        -> dist/solard
#   server/build.sh release      all release files (needs cosmocc)    -> dist/
#       solard-linux-x86_64, solard-linux-arm64   static, any distro  (Cosmopolitan + assimilate)
#       solard-windows-x86_64.exe                                      (Cosmopolitan)
#       solard-macos             universal arm64 + x86_64              (only when built on a Mac)
set -e
cd "$(dirname "$0")"
mkdir -p ../dist
xxd -i index.html > index_html.h
if [ "$1" != release ]; then
  ${CC:-cc} -O2 -Wall -Wextra -o ../dist/solard solard.c
  echo "built dist/solard"; exit 0
fi
cosmocc -O2 -Wall -Wextra -o ../dist/solard.com solard.c
cp ../dist/solard.com ../dist/solard-windows-x86_64.exe
cp ../dist/solard.com ../dist/solard-linux-x86_64 && assimilate -x -e ../dist/solard-linux-x86_64
cp ../dist/solard.com ../dist/solard-linux-arm64 && assimilate -a -e ../dist/solard-linux-arm64
rm -f ../dist/solard.com ../dist/*.bak ../dist/*.dbg ../dist/*.elf
if [ "$(uname -s)" = Darwin ]; then
  cc -O2 -Wall -Wextra -arch arm64 -arch x86_64 -mmacosx-version-min=11.0 -o ../dist/solard-macos solard.c
fi
ls -la ../dist
