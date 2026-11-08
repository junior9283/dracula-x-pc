#!/usr/bin/env bash
# Gera dist/dracula-x-linux-x86_64.tar.gz: executável + libSDL3 embutida (rpath $ORIGIN/lib). Nenhum dado do jogo vai no pacote.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
build="$root/build-dist-linux"
out="$root/dist/dracula-x-linux-x86_64"
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON -DCMAKE_INSTALL_RPATH='$ORIGIN/lib'
cmake --build "$build" --target dracula-x
rm -rf "$out" && mkdir -p "$out/lib"
cp "$build/dracula-x" "$out/"
sdl="$(ldd "$build/dracula-x" | awk '/libSDL3/ {print $3}')"
cp -L "$sdl" "$out/lib/"
cp "$root/tools/package/LEIAME.txt" "$out/"
tar -C "$root/dist" -czf "$out.tar.gz" "$(basename "$out")"
echo "pacote: $out.tar.gz"
