include $(TOOLCHAIN_FILE)
export CC_FOR_TARGET
export LD_FOR_TARGET
export NM_FOR_TARGET
export OBJCOPY_FOR_TARGET
export READELF_FOR_TARGET

override CC := $(CC_FOR_TARGET)
CFLAGS = -O2 -g -Wall -Wextra
LDFLAGS =
override LD := $(LD_FOR_TARGET)
override QEMU := qemu-system-x86_64
QEMUFLAGS = -m 1G -enable-kvm -cpu host

ifneq ($(findstring x86_64,$(shell $(CC_FOR_TARGET) -dumpmachine)),)
override LDFLAGS += \
    -m elf_x86_64
endif
ifneq ($(findstring aarch64,$(shell $(CC_FOR_TARGET) -dumpmachine)),)
override LDFLAGS += \
    -m aarch64elf
endif
ifneq ($(findstring riscv64,$(shell $(CC_FOR_TARGET) -dumpmachine)),)
override LDFLAGS += \
    -m elf64lriscv
endif
ifneq ($(findstring loongarch64,$(shell $(CC_FOR_TARGET) -dumpmachine)),)
override LDFLAGS += \
    -m elf64loongarch
endif

override LDFLAGS += \
    -Tlinker.ld \
    -nostdlib \
    -zmax-page-size=0x1000 \
    -pie \
    -ztext

override LDFLAGS_MB2 := \
    -m elf_i386 \
    -Tmultiboot2.ld \
    -nostdlib \
    -zmax-page-size=0x1000 \
    -static

override LDFLAGS_MB1 := \
    -m elf_i386 \
    -Tmultiboot.ld \
    -nostdlib \
    -zmax-page-size=0x1000 \
    -static

override CFLAGS += \
    -std=c11 \
    -nostdinc \
    -ffreestanding \
    -fno-stack-protector \
    -fno-stack-check \
    -fno-lto \
    -fPIE \
    -I. \
    -I../limine-protocol/include \
    -isystem ../freestnd-c-hdrs/include \
    -D_LIMINE_PROTO \
    -DLIMINE_API_REVISION=3

ifneq ($(findstring x86_64,$(shell $(CC_FOR_TARGET) -dumpmachine)),)
override CFLAGS += \
    -m64 \
    -march=x86-64 \
    -mgeneral-regs-only \
    -mno-red-zone
endif

ifneq ($(findstring aarch64,$(shell $(CC_FOR_TARGET) -dumpmachine)),)
override CFLAGS += \
    -mgeneral-regs-only
endif

ifneq ($(findstring riscv64,$(shell $(CC_FOR_TARGET) -dumpmachine)),)
override CFLAGS += \
    -march=rv64imac \
    -mabi=lp64 \
    -mno-relax
override LDFLAGS += \
    --no-relax
endif

ifneq ($(findstring loongarch64,$(shell $(CC_FOR_TARGET) -dumpmachine)),)
override CFLAGS += \
    -march=loongarch64 \
    -mabi=lp64s
override LDFLAGS += \
    --no-relax
endif

override CFLAGS_MB := \
    -std=c11 \
    -nostdinc \
    -ffreestanding \
    -fno-stack-protector \
    -fno-stack-check \
    -fno-lto \
    -fno-PIC \
    -m32 \
    -march=i686 \
    -mgeneral-regs-only \
    -I. \
    -I../common/protos \
    -isystem ../freestnd-c-hdrs/include

ifneq ($(findstring 86,$(shell $(CC_FOR_TARGET) -dumpmachine)),)
all: test.elf multiboot2.elf multiboot.elf device_tree.dtb
else
all: test.elf device_tree.dtb
endif

flanterm:
	mkdir -p flanterm
	cp -rv ../common/flanterm/* ./flanterm/

test.elf: limine.o e9print.o memory.o flanterm/flanterm.o flanterm/backends/fb.o
	$(LD) $^ $(LDFLAGS) -o $@

multiboot2.elf: multiboot2_trampoline.o
	$(CC) $(CFLAGS_MB) -c memory.c -o memory.o
	$(CC) $(CFLAGS_MB) -c multiboot2.c -o multiboot2.o
	$(CC) $(CFLAGS_MB) -c e9print.c -o e9print.o
	$(LD) $^ memory.o multiboot2.o e9print.o $(LDFLAGS_MB2) -o $@

multiboot.elf: multiboot_trampoline.o
	$(CC) $(CFLAGS_MB) -c memory.c -o memory.o
	$(CC) $(CFLAGS_MB) -c multiboot.c -o multiboot.o
	$(CC) $(CFLAGS_MB) -c e9print.c -o e9print.o
	$(LD) $^ memory.o multiboot.o e9print.o $(LDFLAGS_MB1) -o $@

%.o: %.c flanterm
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.asm
	nasm -felf32 -F dwarf -g $< -o $@

%.dtb: %.dts
	dtc $< -o $@

clean:
	rm -rf test.elf limine.o e9print.o memory.o
	rm -rf multiboot2.o multiboot2.elf multiboot2_trampoline.o
	rm -rf multiboot.o multiboot_trampoline.o multiboot.elf
	rm -rf flanterm device_tree.dtb
