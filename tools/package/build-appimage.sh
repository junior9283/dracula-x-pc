#!/usr/bin/env bash
# Gera dist/Dracula-X-PC-x86_64.AppImage a partir do mesmo executável do tar.gz. Nenhum dado do jogo vai dentro.
# Precisa do appimagetool (APPIMAGETOOL=caminho, ou baixado para build-appimage/ na primeira execução).
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
here="$root/tools/package"
work="$root/build-appimage"
app="$work/DraculaX.AppDir"
"$here/build-linux.sh"
tool="${APPIMAGETOOL:-$work/appimagetool}"
if [ ! -x "$tool" ]; then
    mkdir -p "$work"
    curl -fsSL -o "$tool" https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
    chmod +x "$tool"
fi
rm -rf "$app" && mkdir -p "$app/usr/bin/lib"
cp "$root/dist/dracula-x-linux-x86_64/dracula-x" "$app/usr/bin/"
cp -L "$root/dist/dracula-x-linux-x86_64/lib/"* "$app/usr/bin/lib/"   # rpath $ORIGIN/lib
cp "$here/icon.png" "$app/dracula-x.png"
cp "$here/LEIAME.txt" "$app/usr/bin/"
cat > "$app/dracula-x.desktop" <<'DESK'
[Desktop Entry]
Type=Application
Name=Dracula X PC
Comment=Emulador dedicado de Akumajou Dracula X (Saturn); usa o seu próprio disco
Exec=dracula-x
Icon=dracula-x
Categories=Game;
Terminal=false
DESK
cat > "$app/AppRun" <<'RUN'
#!/bin/sh
exec "$(dirname "$(readlink -f "$0")")/usr/bin/dracula-x" "$@"
RUN
chmod +x "$app/AppRun"
# --appimage-extract-and-run: roda o appimagetool mesmo sem FUSE.
ARCH=x86_64 "$tool" --appimage-extract-and-run --no-appstream "$app" "$root/dist/Dracula-X-PC-x86_64.AppImage"
echo "pacote: $root/dist/Dracula-X-PC-x86_64.AppImage"
