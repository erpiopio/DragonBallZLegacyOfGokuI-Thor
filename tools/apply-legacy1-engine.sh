#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENGINE="$ROOT/gbarecomp"
PATCH="$ROOT/patches/gbarecomp/legacy1-changes.patch"
EXTRA="$ROOT/patches/gbarecomp/new"

if git -C "$ENGINE" apply --reverse --check "$PATCH" 2>/dev/null; then
    echo "Parche del motor ya aplicado."
else
    git -C "$ENGINE" apply --check "$PATCH"
    git -C "$ENGINE" apply "$PATCH"
    echo "Parche del motor aplicado."
fi

while IFS= read -r -d '' file; do
    relative="${file#"$EXTRA/"}"
    target="$ENGINE/$relative"
    if [[ -e "$target" ]]; then
        cmp -s "$file" "$target" || {
            echo "STOP: archivo diferente: $relative"
            exit 1
        }
    else
        mkdir -p "$(dirname "$target")"
        cp "$file" "$target"
    fi
done < <(find "$EXTRA" -type f -print0)

echo "Legacy I: motor preparado correctamente."
