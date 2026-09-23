#!/usr/bin/env bash
# Compila AnikkuNX.nro. Va eseguito dentro l'immagine Docker devkitpro/devkita64:
#   docker run --rm -v "$PWD":/data devkitpro/devkita64 bash /data/switch/scripts/build_switch.sh
set -e
cd "$(dirname "$0")/.."
git config --global --add safe.directory '*' || true

bash scripts/setup.sh

# librerie usate da borealis e dal client HTTP (di solito gia' presenti nell'immagine)
dkp-pacman -S --needed --noconfirm switch-libwebp || true
dkp-pacman -S --needed --noconfirm switch-curl switch-glfw switch-mesa switch-libdrm_nouveau switch-pkg-config switch-libplacebo || true


# (dopo -S, altrimenti pacman li aggiornerebbe) ffmpeg + libmpv per Switch con decodifica hardware (pacchetti precompilati del progetto wiliwili)
BASE_URL="https://github.com/xfangfang/wiliwili/releases/download/v0.1.0/"
PKGS=(
  "switch-ffmpeg-7.1-1-any.pkg.tar.zst"
  "switch-libmpv-0.36.0-3-any.pkg.tar.zst"
)
mkdir -p /tmp/pkgs
for PKG in "${PKGS[@]}"; do
  [ -f "/tmp/pkgs/${PKG}" ] || curl -fL -o "/tmp/pkgs/${PKG}" "${BASE_URL}${PKG}"
  dkp-pacman -U --noconfirm "/tmp/pkgs/${PKG}"
done
cmake -B build-switch -DPLATFORM_SWITCH=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_DEPENDS_USE_COMPILER=OFF
make -C build-switch AnikkuNX.nro -j"$(nproc)"
mkdir -p ../dist
cp build-switch/AnikkuNX.nro ../dist/
echo "Fatto: dist/AnikkuNX.nro"
