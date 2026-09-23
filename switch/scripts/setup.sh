#!/usr/bin/env bash
# Scarica borealis (stessa versione usata da wiliwili) in switch/library/borealis
set -e
cd "$(dirname "$0")/.."
BOREALIS_COMMIT=5f08b286f3df737f3321d2247a6fe633fcead03c
if [ ! -f library/borealis/library/CMakeLists.txt ]; then
  mkdir -p library
  git clone https://github.com/xfangfang/borealis.git library/borealis
  git -C library/borealis checkout "$BOREALIS_COMMIT"
fi
# glfw serve solo per la build desktop di prova
git -C library/borealis submodule update --init --depth 1 library/lib/extern/glfw || true
echo "borealis pronto"
