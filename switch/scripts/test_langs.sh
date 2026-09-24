#!/bin/bash
# Compila sourcetest una volta e prova in parallelo le fonti di piu' lingue (un log per lingua).
# Uso: test_langs.sh <cartella-progetto> "es pt fr de ..." [ricerca]
set -e
ROOT="$(cygpath -u "$1")"
LANGS="$2"
QUERY="${3:-one piece}"
export PATH=/ucrt64/bin:$PATH
pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-curl mingw-w64-ucrt-x86_64-nlohmann-json >/dev/null
cd "$ROOT/switch"
mkdir -p build_test "$ROOT/log-fonti"
if [ ! -f build_test/gumbo.a ] || [ library/gumbo -nt build_test/gumbo.a ]; then
  rm -f build_test/*.o
  for f in library/gumbo/*.c; do gcc -std=gnu99 -O2 -w -Ilibrary/gumbo -c "$f" -o "build_test/$(basename "$f" .c).o"; done
  ar rcs build_test/gumbo.a build_test/*.o
fi
# compilazione in parallelo (i file delle fonti sono grandi)
mkdir -p build_test/obj
pids=""
for f in tools/sourcetest.cpp src/net/http.cpp src/net/hls_proxy.cpp src/html/html.cpp src/sources/*.cpp src/util/crypto.cpp src/util/unpacker.cpp; do
  o="build_test/obj/$(basename "$f" .cpp).o"
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ src/sources/extractors.hpp -nt "$o" ] || [ src/sources/source.hpp -nt "$o" ]; then
    g++ -std=c++17 -O1 -w -Isrc -Ilibrary/borealis/library/include/borealis/extern -Ilibrary/gumbo -c "$f" -o "$o" &
    pids="$pids $!"
  fi
done
for p in $pids; do wait $p || { echo "ERRORE DI COMPILAZIONE"; exit 1; }; done
g++ build_test/obj/*.o build_test/gumbo.a -lcurl -lws2_32 -o build_test/sourcetest.exe
echo "compilato"
# salva le risposte dei siti per ogni fonte (per capire i fallimenti)
rm -rf "$ROOT/log-fonti/pagine"
export ANX_DUMP="$(cygpath -w "$ROOT/log-fonti/pagine")"
for l in $LANGS; do
  ./build_test/sourcetest.exe "$l" "$QUERY" resources/cacert.pem > "$ROOT/log-fonti/$l.log" 2>&1 &
done
wait
echo "== RIEPILOGHI =="
for l in $LANGS; do echo; echo "######## $l"; sed -n '/RIEPILOGO/,$p' "$ROOT/log-fonti/$l.log"; done
