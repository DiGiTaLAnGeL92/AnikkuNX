#!/usr/bin/env bash
# Compila AnikkuNX.nro su Windows dentro MSYS2 con devkitPro (usato da compila-switch.bat).
# Uso: bash build_windows.sh <fase> <cartella-progetto>
set -e
PHASE="$1"
ROOT="$(cygpath -u "$2")"
cd "$ROOT"

case "$PHASE" in
update)
  pacman --noconfirm -Syuu || true
  ;;
tools)
  pacman -S --needed --noconfirm git make cmake wget zstd tar
  if ! grep -q '^\[dkp-libs\]' /etc/pacman.conf; then
    echo ">> Aggiungo i repository devkitPro"
    # I pacchetti devkitPro sono firmati con chiavi che MSYS2 non conosce:
    # disattiviamo la verifica solo per i due repository devkitPro.
    SIG="SigLevel = Never"
    cat >> /etc/pacman.conf <<CONF

[dkp-libs]
$SIG
Server = https://pkg.devkitpro.org/packages

[dkp-windows]
$SIG
Server = https://pkg.devkitpro.org/packages/windows/\$arch/
CONF
  fi
  # se i server non rispondono ma gli strumenti ci sono gia', si prosegue con quelli installati
  if ! pacman -Sy --noconfirm && [ -x /opt/devkitpro/devkitA64/bin/aarch64-none-elf-g++ ]; then
    echo ">> Server devkitPro non raggiungibili: uso i pacchetti gia' installati"
    exit 0
  fi
  pacman -S --needed --noconfirm switch-dev switch-glfw switch-mesa switch-libdrm_nouveau switch-curl switch-libplacebo switch-libass switch-lua51 \
    switch-pkg-config switch-cmake dkp-toolchain-vars || \
  pacman -S --needed --noconfirm switch-dev switch-glfw switch-mesa switch-libdrm_nouveau switch-curl switch-pkg-config switch-libplacebo
  # ffmpeg + libmpv con decodifica hardware (stessi pacchetti del progetto wiliwili)
  BASE_URL="https://github.com/xfangfang/wiliwili/releases/download/v0.1.0"
  pacman -S --needed --noconfirm switch-libwebp || true
  for PKG in switch-ffmpeg-7.1-1-any.pkg.tar.zst switch-libmpv-0.36.0-3-any.pkg.tar.zst; do
    [ -f "/tmp/$PKG" ] || wget -q -O "/tmp/$PKG" "$BASE_URL/$PKG"
    pacman -U --noconfirm "/tmp/$PKG"
  done
  ;;
build)
  export DEVKITPRO=/opt/devkitpro
  export DEVKITA64=/opt/devkitpro/devkitA64
  export PATH="$DEVKITPRO/tools/bin:$DEVKITA64/bin:$DEVKITPRO/portlibs/switch/bin:$PATH"
  bash switch/scripts/setup.sh
  cmake -S switch -B switch/build-switch -G "Unix Makefiles" -DPLATFORM_SWITCH=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_DEPENDS_USE_COMPILER=OFF
  make -C switch/build-switch AnikkuNX.nro -j"$(nproc)"
  mkdir -p dist
  cp switch/build-switch/AnikkuNX.nro dist/
  echo ">> FATTO: dist/AnikkuNX.nro"
  ;;
esac
