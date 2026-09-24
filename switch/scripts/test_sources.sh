#!/bin/bash
# Compila ed esegue tools/sourcetest (prova delle fonti italiane) con MSYS2 UCRT64 su Windows.
# Uso: test_sources.sh <cartella-progetto> [id|tutte] [ricerca]
set -e
ROOT="$(cygpath -u "$1")"; shift
export PATH=/ucrt64/bin:$PATH
pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-curl mingw-w64-ucrt-x86_64-nlohmann-json >/dev/null
cd "$ROOT/switch"
mkdir -p build_test
if [ ! -f build_test/gumbo.a ] || [ library/gumbo -nt build_test/gumbo.a ]; then
  rm -f build_test/*.o
  for f in library/gumbo/*.c; do gcc -std=gnu99 -O2 -w -Ilibrary/gumbo -c "$f" -o "build_test/$(basename "$f" .c).o"; done
  ar rcs build_test/gumbo.a build_test/*.o
fi
g++ -std=c++17 -O1 -Isrc -Ilibrary/borealis/library/include/borealis/extern -Ilibrary/gumbo tools/sourcetest.cpp src/net/http.cpp src/net/hls_proxy.cpp src/html/html.cpp src/sources/*.cpp src/util/crypto.cpp src/util/unpacker.cpp \
  build_test/gumbo.a -lcurl -lws2_32 -o build_test/sourcetest.exe
./build_test/sourcetest.exe "${1:-tutte}" "${2:-one piece}" resources/cacert.pem
