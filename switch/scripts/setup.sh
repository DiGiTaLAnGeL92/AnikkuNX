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
# Arabo: NanoVG non collega le lettere ne' scrive da destra a sinistra. Label::setText passa il testo a
# brls_shapeArabic() (src/util/arabic.cpp, nell'app), che sceglie le forme giuste e riordina.
LABEL=library/borealis/library/lib/views/label.cpp
if ! grep -q brls_shapeArabic "$LABEL"; then
  sed -i '0,/^namespace brls/s//std::string brls_shapeArabic(const std::string\& text);\n\nnamespace brls/' "$LABEL"
  sed -i '/^void Label::setText(const std::string& text)$/,/^{$/{s/(const std::string& text)$/(const std::string\& brlsRawText)/;s/^{$/{\n    std::string text = brls_shapeArabic(brlsRawText);/}' "$LABEL"
fi
grep -q "brls_shapeArabic(brlsRawText)" "$LABEL" || { echo "patch dell'arabo non applicata"; exit 1; }
# glfw serve solo per la build desktop di prova
git -C library/borealis submodule update --init --depth 1 library/lib/extern/glfw || true
echo "borealis pronto"
