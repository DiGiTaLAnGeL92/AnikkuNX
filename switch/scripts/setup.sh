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
# Su Switch borealis usa il font cinese come font predefinito: le sue lettere accentate (à è ì ò ù,
# le stesse del pinyin) sono larghe e si vedono staccate ("Qualit à"). Usiamo il font standard della
# console; cinese, coreano e icone restano come font di riserva (aggiunti in main.cpp).
sed -i 's/static int regular = Application::getFont(FONT_CHINESE_SIMPLIFIED);/static int regular = Application::getFont(FONT_REGULAR);/' \
  library/borealis/library/lib/core/application.cpp
grep -q "getFont(FONT_REGULAR);" library/borealis/library/lib/core/application.cpp || { echo "patch del font non applicata"; exit 1; }
# glfw serve solo per la build desktop di prova
git -C library/borealis submodule update --init --depth 1 library/lib/extern/glfw || true
echo "borealis pronto"
