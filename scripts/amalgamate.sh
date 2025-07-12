#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$root/dist/cmdspawn/cmdspawn.h}
mkdir -p "$(dirname "$out")"

{
    cat "$root/include/cmdspawn/cmdspawn.h"
    echo
    echo "#if defined(SPW_IMPLEMENTATION) && !defined(SPW_IMPLEMENTATION_DONE)"
    echo "#define SPW_IMPLEMENTATION_DONE"
    for src in "$root"/src/*.h "$root"/src/*.c; do
        [ -f "$src" ] && grep -v '^#include "' "$src"
    done
    echo "#endif"
} > "$out"
echo "$out"
