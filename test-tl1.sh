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
trap '
    rm -f "$work"/program.tl1 "$work"/expected.s "$work"/program.s \
        "$work"/program.o "$work"/program.bin "$work"/source-link.tl1
    rmdir "$work"
' EXIT

printf '%s\n' 'BEGIN END' > "$work/program.tl1"
./TL1 < "$work/program.tl1" > "$work/expected.s"
./TL1 -o "$work/program.s" "$work/program.tl1"
cmp "$work/expected.s" "$work/program.s"
./TL1 "$work/program.tl1" > "$work/program.s"
cmp "$work/expected.s" "$work/program.s"
rm "$work/program.s"
./TL1 -o "$work/program.s" < "$work/program.tl1"
cmp "$work/expected.s" "$work/program.s"

ln "$work/program.tl1" "$work/source-link.tl1"
for path in "$work/program.tl1" "$work/source-link.tl1"; do
    if ./TL1 -o "$path" "$work/program.tl1" 2>/dev/null; then
        printf 'FAIL: accepted the input file as output: %s\n' "$path" >&2
        exit 1
    fi
done
printf '%s\n' 'BEGIN END' | cmp "$work/program.tl1" -

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
