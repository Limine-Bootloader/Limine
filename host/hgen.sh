#! /bin/sh

set -e

cat <<EOF
const uint8_t binary_limine_hdd_bin_data[] = {
EOF

od -v -A n -t x1 <limine-bios-hdd.bin | "$SED" '/^$/d;s/  */ /g;s/ *$//g;s/ /, 0x/g;s/^, /    /g;s/$/,/g;s/^	, /    /g'

cat <<EOF
};
EOF
