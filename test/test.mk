override CC := $(CC_FOR_TARGET)
override CFLAGS := -O2 -g -Wall -Wextra
override LDFLAGS :=
override LD := $(LD_FOR_TARGET)

override CC_IS_CLANG := $(shell ! $(CC) --version 2>/dev/null | $(GREP) -q '^Target: '; echo $$?)

ifeq ($(ARCH),x86)
ifeq ($(CC_IS_CLANG),1)
override CC += \
    -target x86_64-unknown-none-elf
endif
override LDFLAGS += \
    -m elf_x86_64
endif
ifeq ($(ARCH),aarch64)
ifeq ($(CC_IS_CLANG),1)
override CC += \
    -target aarch64-unknown-none-elf
endif
override LDFLAGS += \
    -m aarch64elf
endif
ifeq ($(ARCH),riscv64)
ifeq ($(CC_IS_CLANG),1)
override CC += \
    -target riscv64-unknown-none-elf
endif
override LDFLAGS += \
    -m elf64lriscv
endif
ifeq ($(ARCH),loongarch64)
ifeq ($(CC_IS_CLANG),1)
override CC += \
    -target loongarch64-unknown-none-elf
endif
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
    -I../flanterm/src \
    -isystem ../freestnd-c-hdrs/include \
    -D_LIMINE_PROTO \
    -DLIMINE_API_REVISION=3

ifeq ($(ARCH),x86)
override CFLAGS += \
    -m64 \
    -march=x86-64 \
    -mabi=sysv \
    -mgeneral-regs-only \
    -mno-red-zone
endif

ifeq ($(ARCH),aarch64)
override CFLAGS += \
    -mcpu=generic \
    -march=armv8-a+nofp+nosimd \
    -mgeneral-regs-only
endif

ifeq ($(ARCH),riscv64)
override CFLAGS += \
    -march=rv64imac \
    -mabi=lp64 \
    -mno-relax
override LDFLAGS += \
    --no-relax
endif

ifeq ($(ARCH),loongarch64)
override CFLAGS += \
    -march=loongarch64 \
    -mabi=lp64s \
    -mfpu=none \
    -msimd=none
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
    -mabi=sysv \
    -mgeneral-regs-only \
    -I. \
    -I../common/protos \
    -isystem ../freestnd-c-hdrs/include

ifeq ($(ARCH),x86)
all: test.elf multiboot2.elf multiboot.elf
else
all: test.elf
endif

flanterm.o: ../flanterm/src/flanterm.c
	$(CC) $(CFLAGS) -c $< -o $@

flanterm_fb.o: ../flanterm/src/flanterm_backends/fb.c
	$(CC) $(CFLAGS) -c $< -o $@

test.elf: limine.o e9print.o memory.o flanterm.o flanterm_fb.o
	$(LD) $(LDFLAGS) $^ -o $@

multiboot2.elf: multiboot2_trampoline.o
	$(CC) $(CFLAGS_MB) -c memory.c -o memory.o
	$(CC) $(CFLAGS_MB) -c multiboot2.c -o multiboot2.o
	$(CC) $(CFLAGS_MB) -c e9print.c -o e9print.o
	$(LD) $(LDFLAGS_MB2) $^ memory.o multiboot2.o e9print.o -o $@

multiboot.elf: multiboot_trampoline.o
	$(CC) $(CFLAGS_MB) -c memory.c -o memory.o
	$(CC) $(CFLAGS_MB) -c multiboot.c -o multiboot.o
	$(CC) $(CFLAGS_MB) -c e9print.c -o e9print.o
	$(LD) $(LDFLAGS_MB1) $^ memory.o multiboot.o e9print.o -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.asm
	nasm -felf32 -F dwarf -g $< -o $@

clean:
	rm -rf test.elf limine.o e9print.o memory.o
	rm -rf flanterm.o flanterm_fb.o
	rm -rf multiboot2.o multiboot2.elf multiboot2_trampoline.o
	rm -rf multiboot.o multiboot_trampoline.o multiboot.elf
