#!/bin/sh
set -eu

as6800=${AS6800:-as6800}
ld6800=${LD6800:-ld6800}
for tool in "$as6800" "$ld6800"; do
    if ! command -v "$tool" >/dev/null; then
        printf 'Required tool not found: %s\n' "$tool" >&2
        exit 1
    fi
done

work=$(mktemp -d)
trap 'rm -f "$work"/program.s "$work"/program.o "$work"/program.bin; rmdir "$work"' EXIT
count=0
while IFS='|' read -r expected source; do
    printf '%s\n' "$source" | ./TL1 > "$work/program.s"
    "$as6800" "$work/program.s"
    "$ld6800" -b -C 0x2222 -o "$work/program.bin" "$work/program.o"
    actual=$(od -An -v -tx1 -j "$((0x2222))" "$work/program.bin" | tr -d '[:space:]')
    if [ "$actual" != "$expected" ]; then
        printf 'FAIL: %s\nexpected: %s\nactual:   %s\n' "$source" "$expected" "$actual" >&2
        exit 1
    fi
    count=$((count + 1))
    printf 'PASS: %s\n' "$source"
done < tl1-original-fixtures.txt
printf '%s byte-for-byte matches\n' "$count"
