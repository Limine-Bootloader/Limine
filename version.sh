#! /bin/sh

LC_ALL=C
export LC_ALL

srcdir="$(dirname "$0")"
test -z "$srcdir" && srcdir=.

cd "$srcdir"

if test -f version; then
    printf '%s' "$(cat version)"
    exit 0
fi

if ! test -e .git || ! git log -n1 --pretty='%h' >/dev/null 2>&1; then
    printf 'UNVERSIONED'
    exit 0
fi

tmpfile="$(mktemp)"

if ! git describe --exact-match --tags $(git log -n1 --pretty='%h') >"$tmpfile" 2>/dev/null; then
    echo g$(git log -n1 --pretty='%h') >"$tmpfile"
fi

printf '%s' "$(sed 's/^v//g' <"$tmpfile")"

rm -f "$tmpfile"
