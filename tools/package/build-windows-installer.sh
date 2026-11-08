#!/usr/bin/env bash
# Gera dist/Dracula-X-PC-Setup-x64.exe (instalador NSIS) em cima do pacote do build-windows.sh. Precisa do makensis.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
here="$root/tools/package"
"$here/build-windows.sh"
(cd "$here" && makensis -V2 -DSRC="$root/dist/dracula-x-windows-x64" -DOUT="$root/dist/Dracula-X-PC-Setup-x64.exe" installer.nsi)
echo "instalador: $root/dist/Dracula-X-PC-Setup-x64.exe"
