#!/usr/bin/env bash
# Gera dist/dracula-x-windows-x64.zip por cross-compilação (mingw-w64): dracula-x.exe + SDL3.dll. Nenhum dado do jogo vai no pacote.
# Teste opcional com Wine: wine dist/dracula-x-windows-x64/dracula-x.exe --help
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
build="$root/build-dist-win"
out="$root/dist/dracula-x-windows-x64"
"$root/tools/package/fetch-sdl3-mingw.sh"
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$root/cmake/mingw-w64.cmake"
cmake --build "$build" --target dracula-x
rm -rf "$out" && mkdir -p "$out"
cp "$build/dracula-x.exe" "$root/deps-win/usr/x86_64-w64-mingw32/sys-root/mingw/bin/SDL3.dll" "$out/"
sed 's/$/\r/' "$root/tools/package/LEIAME.txt" > "$out/LEIAME.txt"
(cd "$root/dist" && rm -f "$(basename "$out").zip" && zip -qr "$(basename "$out").zip" "$(basename "$out")")
echo "pacote: $out.zip"
