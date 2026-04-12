#!/bin/bash
# Mechanically test gzip decompression end-to-end in QEMU.
# Boots the Limine test kernel via UEFI with both a plain file and
# its .gz counterpart as internal modules.  The test kernel loads
# the compressed module (decompressed by the bootloader) and compares
# it byte-for-byte against the plain copy.
# Usage:    bash contrib/test_gzip_qemu.sh
# Requires: qemu-system-x86_64, mtools, gzip
set -euo pipefail
cd "$(dirname "$0")/.."
TIMEOUT="${QEMU_TIMEOUT:-20}"
if ! command -v gzip >/dev/null 2>&1; then
  echo "where's your gzip?"
  exit 1
fi
TEST_CFLAGS="-DENABLE_QEMU_SHUTDOWN -DENABLE_GZIP_TEST"
make limine-bios limine-uefi-x86-64 2>&1 | tail -1
make edk2-ovmf 2>&1 | tail -1
make -C test -f test.mk ARCH=x86 EXTRA_CFLAGS="$TEST_CFLAGS" test.elf 2>&1 | tail -1
IMG=test_uefi.img
rm -f "$IMG"
mformat -i "$IMG" -C -F -T 131072 :: 2>/dev/null
mmd -i "$IMG" ::/boot ::/EFI ::/EFI/BOOT 2>/dev/null
mcopy -i "$IMG" bin/BOOTX64.EFI ::/EFI/BOOT/
mcopy -i "$IMG" bin/limine-bios.sys ::/boot/
mcopy -i "$IMG" test/test.elf ::/boot/
mcopy -i "$IMG" test/bg.jpg ::/boot/
mcopy -i "$IMG" test/limine.conf ::/boot/
GZ_TMP=$(mktemp)
gzip -c test/limine.conf > "$GZ_TMP"
mcopy -i "$IMG" "$GZ_TMP" ::/boot/limine.conf.gz
rm -f "$GZ_TMP"
QEMU_LOG=$(mktemp)
trap 'rm -f "$QEMU_LOG" "$IMG"' EXIT
timeout "$TIMEOUT" \
  qemu-system-x86_64 \
    -display none \
    -m 512M -M q35 \
    -drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-x86_64.fd,readonly=on \
    -net none -smp 4 \
    -drive format=raw,file="$IMG" \
    -debugcon file:"$QEMU_LOG" \
  || true  # timeout exits 124
if grep -q 'gzip: pass' "$QEMU_LOG"; then
  grep 'gzip:' "$QEMU_LOG"
  echo "pass: gzip decompression verified in QEMU"
  exit 0
elif grep -q 'gzip: FAIL' "$QEMU_LOG"; then
  grep 'gzip:' "$QEMU_LOG"
  echo "fail"
  exit 1
else
  echo "fail: gzip test marker not found in QEMU output"
  echo "last 20 lines of log:"
  tail -20 "$QEMU_LOG"
  exit 1
fi
