#!/usr/bin/env bash
# Baixa o SDL3 do Windows (rpm mingw64-SDL3 do repositório da distribuição) e o extrai em deps-win/, sem root.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$root/deps-win"
cd "$root/deps-win"
if [ ! -f usr/x86_64-w64-mingw32/sys-root/mingw/bin/SDL3.dll ]; then
    dnf download mingw64-SDL3
    rpm2cpio mingw64-SDL3-*.rpm | cpio -idm --quiet
fi
echo "SDL3 para Windows em $root/deps-win"
