.PHONY: test-clean
test-clean:
	$(MAKE) -C test -f test.mk clean
	rm -rf test_image test.hdd test.iso

ovmf-x64:
	$(MKDIR_P) ovmf-x64
	curl -Lo ovmf-x64/OVMF_CODE.fd https://github.com/osdev0/edk2-ovmf-nightly/releases/latest/download/ovmf-code-x86_64.fd
	curl -Lo ovmf-x64/OVMF_VARS.fd https://github.com/osdev0/edk2-ovmf-nightly/releases/latest/download/ovmf-vars-x86_64.fd

ovmf-aa64:
	$(MKDIR_P) ovmf-aa64
	curl -Lo ovmf-aa64/OVMF_CODE.fd https://github.com/osdev0/edk2-ovmf-nightly/releases/latest/download/ovmf-code-aarch64.fd
	curl -Lo ovmf-aa64/OVMF_VARS.fd https://github.com/osdev0/edk2-ovmf-nightly/releases/latest/download/ovmf-vars-aarch64.fd
	dd if=/dev/zero of=ovmf-aa64/OVMF_CODE.fd bs=1 count=0 seek=67108864 2>/dev/null
	dd if=/dev/zero of=ovmf-aa64/OVMF_VARS.fd bs=1 count=0 seek=67108864 2>/dev/null

ovmf-rv64:
	$(MKDIR_P) ovmf-rv64
	curl -Lo ovmf-rv64/OVMF_CODE.fd https://github.com/osdev0/edk2-ovmf-nightly/releases/latest/download/ovmf-code-riscv64.fd
	curl -Lo ovmf-rv64/OVMF_VARS.fd https://github.com/osdev0/edk2-ovmf-nightly/releases/latest/download/ovmf-vars-riscv64.fd
	dd if=/dev/zero of=ovmf-rv64/OVMF_CODE.fd bs=1 count=0 seek=33554432 2>/dev/null
	dd if=/dev/zero of=ovmf-rv64/OVMF_VARS.fd bs=1 count=0 seek=33554432 2>/dev/null

ovmf-ia32:
	$(MKDIR_P) ovmf-ia32
	curl -Lo ovmf-ia32/OVMF_CODE.fd https://github.com/osdev0/edk2-ovmf-nightly/releases/latest/download/ovmf-code-ia32.fd
	curl -Lo ovmf-ia32/OVMF_VARS.fd https://github.com/osdev0/edk2-ovmf-nightly/releases/latest/download/ovmf-vars-ia32.fd

ovmf-loongarch64:
	$(MKDIR_P) ovmf-loongarch64
	curl -Lo ovmf-loongarch64/OVMF_CODE.fd https://github.com/osdev0/edk2-ovmf-nightly/releases/latest/download/ovmf-code-loongarch64.fd
	curl -Lo ovmf-loongarch64/OVMF_VARS.fd https://github.com/osdev0/edk2-ovmf-nightly/releases/latest/download/ovmf-vars-loongarch64.fd

.PHONY: test.hdd
test.hdd:
	rm -f test.hdd
	dd if=/dev/zero bs=1M count=0 seek=64 of=test.hdd
	PATH=$$PATH:/usr/sbin:/sbin parted -s test.hdd mklabel msdos
	PATH=$$PATH:/usr/sbin:/sbin parted -s test.hdd mkpart primary 2048s 100%

.PHONY: mbrtest.hdd
mbrtest.hdd:
	rm -f mbrtest.hdd
	dd if=/dev/zero bs=1M count=0 seek=64 of=mbrtest.hdd
	echo -e "o\nn\np\n1\n2048\n\nt\n6\na\nw\n" | fdisk mbrtest.hdd -H 16 -S 63

.PHONY: fat12-test
fat12-test:
	$(MAKE) test-clean
	$(MAKE) test.hdd
	$(MAKE) limine-bios
	$(MAKE) limine
	$(MAKE) -C test -f test.mk ARCH=x86
	rm -rf test_image/
	mkdir test_image
	sudo losetup -Pf --show test.hdd > loopback_dev
	sudo partprobe `cat loopback_dev`
	sudo mkfs.fat -F 12 `cat loopback_dev`p1
	sudo mount `cat loopback_dev`p1 test_image
	sudo mkdir test_image/boot
	sudo cp -rv $(BINDIR)/* test_image/boot/
	sudo cp -rv test/* test_image/boot/
	sync
	sudo umount test_image/
	sudo losetup -d `cat loopback_dev`
	rm -rf test_image loopback_dev
	$(BINDIR)/limine bios-install test.hdd
	qemu-system-x86_64 -net none -smp 4   -hda test.hdd -debugcon stdio

.PHONY: fat16-test
fat16-test:
	$(MAKE) test-clean
	$(MAKE) test.hdd
	$(MAKE) limine-bios
	$(MAKE) limine
	$(MAKE) -C test -f test.mk ARCH=x86
	rm -rf test_image/
	mkdir test_image
	sudo losetup -Pf --show test.hdd > loopback_dev
	sudo partprobe `cat loopback_dev`
	sudo mkfs.fat -F 16 `cat loopback_dev`p1
	sudo mount `cat loopback_dev`p1 test_image
	sudo mkdir test_image/boot
	sudo cp -rv $(BINDIR)/* test_image/boot/
	sudo cp -rv test/* test_image/boot/
	sync
	sudo umount test_image/
	sudo losetup -d `cat loopback_dev`
	rm -rf test_image loopback_dev
	$(BINDIR)/limine bios-install test.hdd
	qemu-system-x86_64 -net none -smp 4   -hda test.hdd -debugcon stdio

.PHONY: legacy-fat16-test
legacy-fat16-test:
	$(MAKE) test-clean
	$(MAKE) mbrtest.hdd
	fdisk -l mbrtest.hdd
	$(MAKE) limine-bios
	$(MAKE) limine
	$(MAKE) -C test -f test.mk ARCH=x86
	rm -rf test_image/
	mkdir test_image
	sudo losetup -Pf --show mbrtest.hdd > loopback_dev
	sudo partprobe `cat loopback_dev`
	sudo mkfs.fat -F 16 `cat loopback_dev`p1
	sudo mount `cat loopback_dev`p1 test_image
	sudo mkdir test_image/boot
	sudo cp -rv $(BINDIR)/* test_image/boot/
	sudo cp -rv test/* test_image/boot/
	sync
	sudo umount test_image/
	sudo losetup -d `cat loopback_dev`
	rm -rf test_image loopback_dev
	$(BINDIR)/limine bios-install mbrtest.hdd
	qemu-system-i386 -cpu pentium2 -m 16M -M isapc -net none   -hda mbrtest.hdd -debugcon stdio

.PHONY: fat32-test
fat32-test:
	$(MAKE) test-clean
	$(MAKE) test.hdd
	$(MAKE) limine-bios
	$(MAKE) limine
	$(MAKE) -C test -f test.mk ARCH=x86
	rm -rf test_image/
	mkdir test_image
	sudo losetup -Pf --show test.hdd > loopback_dev
	sudo partprobe `cat loopback_dev`
	sudo mkfs.fat -F 32 `cat loopback_dev`p1
	sudo mount `cat loopback_dev`p1 test_image
	sudo mkdir test_image/boot
	sudo cp -rv $(BINDIR)/* test_image/boot/
	sudo cp -rv test/* test_image/boot/
	sync
	sudo umount test_image/
	sudo losetup -d `cat loopback_dev`
	rm -rf test_image loopback_dev
	$(BINDIR)/limine bios-install test.hdd
	qemu-system-x86_64 -net none -smp 4   -hda test.hdd -debugcon stdio

.PHONY: iso9660-test
iso9660-test:
	$(MAKE) test-clean
	$(MAKE) test.hdd
	$(MAKE) limine-bios
	$(MAKE) -C test -f test.mk ARCH=x86
	rm -rf test_image/
	$(MKDIR_P) test_image/boot
	cp -rv $(BINDIR)/* test_image/boot/
	cp -rv test/* test_image/boot/
	xorriso -as mkisofs -b boot/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table test_image/ -o test.iso
	qemu-system-x86_64 -net none -smp 4   -cdrom test.iso -debugcon stdio

.PHONY: full-hybrid-test
full-hybrid-test:
	$(MAKE) ovmf-x64
	$(MAKE) ovmf-ia32
	$(MAKE) test-clean
	$(MAKE) all
	$(MAKE) -C test -f test.mk ARCH=x86
	rm -rf test_image/
	$(MKDIR_P) test_image/boot
	cp -rv $(BINDIR)/* test_image/boot/
	cp -rv test/* test_image/boot/
	$(MKDIR_P) test_image/EFI/BOOT
	cp -v $(BINDIR)/BOOT*.EFI test_image/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus -apm-block-size 2048 --efi-boot boot/limine-uefi-cd.bin -efi-boot-part --efi-boot-image --protective-msdos-label test_image/ -o test.iso
	$(BINDIR)/limine bios-install test.iso
	qemu-system-x86_64 -m 512M -M q35 -drive if=pflash,unit=0,format=raw,file=ovmf-x64/OVMF_CODE.fd,readonly=on -net none -smp 4   -cdrom test.iso -debugcon stdio
	qemu-system-x86_64 -m 512M -M q35 -drive if=pflash,unit=0,format=raw,file=ovmf-x64/OVMF_CODE.fd,readonly=on -net none -smp 4   -hda test.iso -debugcon stdio
	qemu-system-x86_64 -m 512M -M q35 -drive if=pflash,unit=0,format=raw,file=ovmf-ia32/OVMF_CODE.fd,readonly=on -net none -smp 4   -cdrom test.iso -debugcon stdio
	qemu-system-x86_64 -m 512M -M q35 -drive if=pflash,unit=0,format=raw,file=ovmf-ia32/OVMF_CODE.fd,readonly=on -net none -smp 4   -hda test.iso -debugcon stdio
	qemu-system-x86_64 -m 512M -M q35 -net none -smp 4   -cdrom test.iso -debugcon stdio
	qemu-system-x86_64 -m 512M -M q35 -net none -smp 4   -hda test.iso -debugcon stdio

.PHONY: pxe-test
pxe-test:
	$(MAKE) test-clean
	$(MAKE) limine-bios
	$(MAKE) -C test -f test.mk ARCH=x86
	rm -rf test_image/
	$(MKDIR_P) test_image/boot
	cp -rv $(BINDIR)/* test_image/boot/
	cp -rv test/* test_image/boot/
	qemu-system-x86_64  -smp 4  -netdev user,id=n0,tftp=./test_image,bootfile=boot/limine-bios-pxe.bin -device rtl8139,netdev=n0,mac=00:00:00:11:11:11 -debugcon stdio

.PHONY: uefi-x86-64-test
uefi-x86-64-test:
	$(MAKE) ovmf-x64
	$(MAKE) test-clean
	$(MAKE) test.hdd
	$(MAKE) limine-uefi-x86-64
	$(MAKE) -C test -f test.mk ARCH=x86
	rm -rf test_image/
	mkdir test_image
	sudo losetup -Pf --show test.hdd > loopback_dev
	sudo partprobe `cat loopback_dev`
	sudo mkfs.fat -F 32 `cat loopback_dev`p1
	sudo mount `cat loopback_dev`p1 test_image
	sudo mkdir test_image/boot
	sudo cp -rv $(BINDIR)/* test_image/boot/
	sudo cp -rv test/* test_image/boot/
	sudo $(MKDIR_P) test_image/EFI/BOOT
	sudo cp $(BINDIR)/BOOTX64.EFI test_image/EFI/BOOT/
	sync
	sudo umount test_image/
	sudo losetup -d `cat loopback_dev`
	rm -rf test_image loopback_dev
	qemu-system-x86_64 -m 512M -M q35 -drive if=pflash,unit=0,format=raw,file=ovmf-x64/OVMF_CODE.fd,readonly=on -drive if=pflash,unit=1,format=raw,file=ovmf-x64/OVMF_VARS.fd -net none -smp 4   -hda test.hdd -debugcon stdio

.PHONY: uefi-aa64-test
uefi-aa64-test:
	$(MAKE) ovmf-aa64
	$(MAKE) test-clean
	$(MAKE) test.hdd
	$(MAKE) limine-uefi-aarch64
	$(MAKE) -C test -f test.mk ARCH=aarch64
	rm -rf test_image/
	mkdir test_image
	sudo losetup -Pf --show test.hdd > loopback_dev
	sudo partprobe `cat loopback_dev`
	sudo mkfs.fat -F 32 `cat loopback_dev`p1
	sudo mount `cat loopback_dev`p1 test_image
	sudo mkdir test_image/boot
	sudo cp -rv $(BINDIR)/* test_image/boot/
	sudo cp -rv test/* test_image/boot/
	sudo $(MKDIR_P) test_image/EFI/BOOT
	sudo cp $(BINDIR)/BOOTAA64.EFI test_image/EFI/BOOT/
	sync
	sudo umount test_image/
	sudo losetup -d `cat loopback_dev`
	rm -rf test_image loopback_dev
	qemu-system-aarch64 -m 512M -M virt -cpu cortex-a72 -drive if=pflash,unit=0,format=raw,file=ovmf-aa64/OVMF_CODE.fd,readonly=on -drive if=pflash,unit=1,format=raw,file=ovmf-aa64/OVMF_VARS.fd -net none -smp 4 -device ramfb -device qemu-xhci -device usb-kbd  -hda test.hdd -serial stdio

.PHONY: uefi-rv64-test
uefi-rv64-test:
	$(MAKE) ovmf-rv64
	$(MAKE) test-clean
	$(MAKE) test.hdd
	$(MAKE) limine-uefi-riscv64
	$(MAKE) -C test -f test.mk ARCH=riscv64
	rm -rf test_image/
	mkdir test_image
	sudo losetup -Pf --show test.hdd > loopback_dev
	sudo partprobe `cat loopback_dev`
	sudo mkfs.fat -F 32 `cat loopback_dev`p1
	sudo mount `cat loopback_dev`p1 test_image
	sudo mkdir test_image/boot
	sudo cp -rv $(BINDIR)/* test_image/boot/
	sudo cp -rv test/* test_image/boot/
	sudo $(MKDIR_P) test_image/EFI/BOOT
	sudo cp $(BINDIR)/BOOTRISCV64.EFI test_image/EFI/BOOT/
	sync
	sudo umount test_image/
	sudo losetup -d `cat loopback_dev`
	rm -rf test_image loopback_dev
	qemu-system-riscv64 -m 512M -M virt -cpu rv64 -drive if=pflash,unit=0,format=raw,file=ovmf-rv64/OVMF_CODE.fd,readonly=on -drive if=pflash,unit=1,format=raw,file=ovmf-rv64/OVMF_VARS.fd -net none -smp 4 -device ramfb -device qemu-xhci -device usb-kbd -hda test.hdd -serial stdio

.PHONY: uefi-loongarch64-test
uefi-loongarch64-test:
	$(MAKE) ovmf-loongarch64
	$(MAKE) test-clean
	$(MAKE) test.hdd
	$(MAKE) limine-uefi-loongarch64
	$(MAKE) -C test -f test.mk ARCH=loongarch64
	rm -rf test_image/
	mkdir test_image
	sudo losetup -Pf --show test.hdd > loopback_dev
	sudo partprobe `cat loopback_dev`
	sudo mkfs.fat -F 32 `cat loopback_dev`p1
	sudo mount `cat loopback_dev`p1 test_image
	sudo mkdir test_image/boot
	sudo cp -rv $(BINDIR)/* test_image/boot/
	sudo cp -rv test/* test_image/boot/
	sudo $(MKDIR_P) test_image/EFI/BOOT
	sudo cp $(BINDIR)/BOOTLOONGARCH64.EFI test_image/EFI/BOOT/
	sync
	sudo umount test_image/
	sudo losetup -d `cat loopback_dev`
	rm -rf test_image loopback_dev
	qemu-system-loongarch64 -m 1G -net none -M virt -cpu la464 -device ramfb -device qemu-xhci -device usb-kbd -drive if=pflash,unit=0,format=raw,file=ovmf-loongarch64/OVMF_CODE.fd,readonly=on -drive if=pflash,unit=1,format=raw,file=ovmf-loongarch64/OVMF_VARS.fd -hda test.hdd -serial stdio

.PHONY: uefi-ia32-test
uefi-ia32-test:
	$(MAKE) ovmf-ia32
	$(MAKE) test-clean
	$(MAKE) test.hdd
	$(MAKE) limine-uefi-ia32
	$(MAKE) -C test -f test.mk ARCH=x86
	rm -rf test_image/
	mkdir test_image
	sudo losetup -Pf --show test.hdd > loopback_dev
	sudo partprobe `cat loopback_dev`
	sudo mkfs.fat -F 32 `cat loopback_dev`p1
	sudo mount `cat loopback_dev`p1 test_image
	sudo mkdir test_image/boot
	sudo cp -rv $(BINDIR)/* test_image/boot/
	sudo cp -rv test/* test_image/boot/
	sudo $(MKDIR_P) test_image/EFI/BOOT
	sudo cp $(BINDIR)/BOOTIA32.EFI test_image/EFI/BOOT/
	sync
	sudo umount test_image/
	sudo losetup -d `cat loopback_dev`
	rm -rf test_image loopback_dev
	qemu-system-x86_64 -m 512M -M q35 -drive if=pflash,unit=0,format=raw,file=ovmf-ia32/OVMF_CODE.fd,readonly=on -drive if=pflash,unit=1,format=raw,file=ovmf-ia32/OVMF_VARS.fd -net none -smp 4   -hda test.hdd -debugcon stdio
