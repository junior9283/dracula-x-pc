#!/usr/bin/env bash
# Gera, num Mac, os pacotes do macOS em dist/: um app UNIVERSAL (Intel x86_64 + Apple Silicon arm64) e as versões só-Intel e
# só-Apple Silicon, cada uma num .dmg. Nenhum dado do jogo vai nos pacotes.
# Requisitos: macOS 11+, Xcode Command Line Tools (xcode-select --install), cmake e ninja (brew install cmake ninja).
# O SDL3 é compilado do código-fonte oficial (hash conferido) e ligado de forma estática: não há .dylib para distribuir.
# O app recebe assinatura "ad hoc" (necessária para rodar no Apple Silicon); não é notarizado pela Apple (ver LEIAME).
set -euo pipefail

if [ "$(uname -s)" != "Darwin" ]; then
    echo "este script só roda no macOS (precisa do SDK da Apple); no GitHub use .github/workflows/macos.yml" >&2
    exit 1
fi

sdl_version="3.4.18"
sdl_sha256="9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3"
deployment_target="11.0"

root="$(cd "$(dirname "$0")/../.." && pwd)"
here="$root/tools/package"
work="$root/build-macos"
dist="$root/dist"
archs="x86_64;arm64"
mkdir -p "$work" "$dist"

# --- SDL3 universal, estático
sdl_prefix="$work/sdl3-prefix"
if [ ! -f "$sdl_prefix/lib/cmake/SDL3/SDL3Config.cmake" ]; then
    tarball="$work/SDL3-$sdl_version.tar.gz"
    [ -f "$tarball" ] || curl -fsSL -o "$tarball" "https://github.com/libsdl-org/SDL/releases/download/release-$sdl_version/SDL3-$sdl_version.tar.gz"
    echo "$sdl_sha256  $tarball" | shasum -a 256 -c -
    rm -rf "$work/sdl-src" && mkdir -p "$work/sdl-src" && tar -xzf "$tarball" -C "$work/sdl-src" --strip-components=1
    cmake -S "$work/sdl-src" -B "$work/sdl-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="$archs" -DCMAKE_OSX_DEPLOYMENT_TARGET="$deployment_target" \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
        -DCMAKE_INSTALL_PREFIX="$sdl_prefix"
    cmake --build "$work/sdl-build"
    cmake --install "$work/sdl-build"
fi

# --- Jogo universal
build="$work/game-build"
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$archs" -DCMAKE_OSX_DEPLOYMENT_TARGET="$deployment_target" \
    -DCMAKE_PREFIX_PATH="$sdl_prefix"
cmake --build "$build" --target dracula-x
file "$build/dracula-x"

# Testes: rodam na arquitetura da máquina (a outra só compila). Pule com SKIP_TESTS=1.
if [ "${SKIP_TESTS:-0}" != "1" ]; then
    cmake --build "$build" --target tests
    "$build/tests/tests"
fi

# --- .app (universal e thin) e .dmg
make_app() {   # make_app <pasta-do-app> <executável>
    local app="$1" exe="$2"
    rm -rf "$app" && mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
    cp "$exe" "$app/Contents/MacOS/dracula-x"
    cp "$here/icon.icns" "$app/Contents/Resources/icon.icns"
    cp "$here/LEIAME.txt" "$app/Contents/Resources/LEIAME.txt"
    cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>Dracula X PC</string>
    <key>CFBundleDisplayName</key><string>Dracula X PC</string>
    <key>CFBundleIdentifier</key><string>org.dracula-x-pc.player</string>
    <key>CFBundleExecutable</key><string>dracula-x</string>
    <key>CFBundleIconFile</key><string>icon</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>1.0</string>
    <key>CFBundleVersion</key><string>1</string>
    <key>LSMinimumSystemVersion</key><string>$deployment_target</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>GCSupportsControllerUserInteraction</key><true/>
</dict>
</plist>
PLIST
    codesign --force --deep --sign - "$app"   # assinatura ad hoc: o Apple Silicon recusa binário sem assinatura
}

make_dmg() {   # make_dmg <nome> <executável>
    local name="$1" exe="$2" stage="$work/dmg-$1"
    rm -rf "$stage" && mkdir -p "$stage"
    make_app "$stage/Dracula X PC.app" "$exe"
    ln -s /Applications "$stage/Applications"
    rm -f "$dist/Dracula-X-PC-macOS-$name.dmg"
    hdiutil create -volname "Dracula X PC" -srcfolder "$stage" -format UDZO -ov "$dist/Dracula-X-PC-macOS-$name.dmg" >/dev/null
    echo "pacote: $dist/Dracula-X-PC-macOS-$name.dmg"
}

make_dmg universal "$build/dracula-x"
lipo "$build/dracula-x" -thin x86_64 -output "$work/dracula-x-x86_64"
lipo "$build/dracula-x" -thin arm64 -output "$work/dracula-x-arm64"
make_dmg x86_64 "$work/dracula-x-x86_64"
make_dmg arm64 "$work/dracula-x-arm64"
