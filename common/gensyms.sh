#! /bin/sh

set -e

LC_ALL=C
export LC_ALL

# A set -o option the shell lacks would exit it, so try it in a subshell first.
( set -o pipefail ) 2>/dev/null && set -o pipefail

TMP1="$(mktemp)"
TMP2="$(mktemp)"
TMP3="$(mktemp)"
TMP4="$(mktemp)"

trap 'rm -f "$TMP1" "$TMP2" "$TMP3" "$TMP4"' EXIT

"$OBJDUMP_FOR_TARGET" -t "$1" | sort > "$TMP1"
"$GREP" "F $4" < "$TMP1" | cut -d ' ' -f 1 > "$TMP2"
"$GREP" "F $4" < "$TMP1" | "$AWK" 'NF{ print $NF }' > "$TMP3"

echo ".section .$2_map" > "$TMP4"
echo ".globl $2_map" >> "$TMP4"
echo "$2_map:" >> "$TMP4"

if [ "$3" = "32" ]; then
    paste -d '#' "$TMP2" "$TMP3" | "$SED" 's/^/.long 0x/g;s/$/"/g;s/#/\
.asciz "/g' >> "$TMP4"
    echo ".long 0xffffffff" >> "$TMP4"
elif [ "$3" = "64" ]; then
    paste -d '#' "$TMP2" "$TMP3" | "$SED" 's/^/.quad 0x/g;s/$/"/g;s/#/\
.asciz "/g' >> "$TMP4"
    echo ".quad 0xffffffffffffffff" >> "$TMP4"
fi

mv "$TMP4" "$2.map.S"
