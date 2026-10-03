.DELETE_ON_ERROR:

override CC := $(CC_FOR_TARGET)
override CFLAGS := -O2 -g -Wall -Wextra
override LDFLAGS :=
override LD := $(LD_FOR_TARGET)

override CC_IS_CLANG := $(shell ! $(CC) --version 2>/dev/null | $(GREP) -q '^Target: '; echo $$?)

ifeq ($(CC_IS_CLANG),1)
override CC += \
    -target $(patsubst x86,x86_64,$(ARCH))-unknown-none-elf
endif

override LDFLAGS += \
    -nostdlib \
    -zmax-page-size=0x1000 \
    -pie \
    -ztext \
    -Tlinker.ld

ifeq ($(ARCH),x86)
override LDFLAGS += \
    -m elf_x86_64
endif
ifeq ($(ARCH),aarch64)
override LDFLAGS += \
    -m aarch64elf \
    --fix-cortex-a53-843419
endif
ifeq ($(ARCH),riscv64)
override LDFLAGS += \
    -m elf64lriscv
endif
ifeq ($(ARCH),loongarch64)
override LDFLAGS += \
    -m elf64loongarch
endif

override LDFLAGS_MB2 := \
    -m elf_i386 \
    -nostdlib \
    -zmax-page-size=0x1000 \
    -static \
    -Tmultiboot2.ld

override LDFLAGS_MB1 := \
    -m elf_i386 \
    -nostdlib \
    -zmax-page-size=0x1000 \
    -static \
    -Tmultiboot.ld

override CFLAGS += \
    -std=c11 \
    -nostdinc \
    -ffreestanding \
    -fno-common \
    -fno-stack-protector \
    -fno-stack-check \
    -fno-lto \
    -fPIE \
    -I. \
    -I../limine-protocol/include \
    -I../flanterm/src \
    -isystem ../freestanding-c-hdrs/include \
    -D_LIMINE_PROTO \
    $(EXTRA_CFLAGS)

ifeq ($(ARCH),x86)
override CFLAGS += \
    -m64 \
    -march=x86-64 \
    -mgeneral-regs-only \
    -mno-red-zone
endif

ifeq ($(ARCH),aarch64)
override CFLAGS += \
    -mcpu=generic \
    -march=armv8-a+nofp+nosimd \
    -mcmodel=small \
    -mno-outline-atomics
endif

ifeq ($(ARCH),riscv64)
override CFLAGS += \
    -mabi=lp64 \
    -march=rv64imac_zicsr_zifencei
# Keep linker relaxation away from gp, which nothing sets up.
override LDFLAGS += \
    --no-relax-gp
endif

ifeq ($(ARCH),loongarch64)
# Both compilers go through the GOT for external symbols by default, which only
# dynamic linking needs.
ifeq ($(CC_IS_CLANG),1)
override CFLAGS += \
    -fdirect-access-external-data
else
override CFLAGS += \
    -mdirect-extern-access
endif
override CFLAGS += \
    -mabi=lp64s \
    -march=loongarch64 \
    -msoft-float
endif

override CFLAGS_MB := \
    -Wall \
    -Wextra \
    -std=c11 \
    -nostdinc \
    -ffreestanding \
    -fno-common \
    -fno-stack-protector \
    -fno-stack-check \
    -fno-lto \
    -fno-PIC \
    -m32 \
    -march=i686 \
    -mgeneral-regs-only \
    -I. \
    -I../common/protos \
    -isystem ../freestanding-c-hdrs/include

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

# The compiler emits calls to the 64-bit division helpers, which a freestanding
# link has nothing else to satisfy.
cc-runtime.mb.o: ../common/cc-runtime.s2.c
	$(CC) $(CFLAGS_MB) -c $< -o $@

# These are 32-bit where test.elf's objects are not, so they need names of
# their own.
memory.mb.o: memory.c
	$(CC) $(CFLAGS_MB) -c $< -o $@

e9print.mb.o: e9print.c
	$(CC) $(CFLAGS_MB) -c $< -o $@

multiboot.o: multiboot.c
	$(CC) $(CFLAGS_MB) -c $< -o $@

multiboot2.o: multiboot2.c
	$(CC) $(CFLAGS_MB) -c $< -o $@

multiboot2.elf: multiboot2_trampoline.o cc-runtime.mb.o memory.mb.o multiboot2.o e9print.mb.o
	$(LD) $(LDFLAGS_MB2) $^ -o $@

multiboot.elf: multiboot_trampoline.o cc-runtime.mb.o memory.mb.o multiboot.o e9print.mb.o
	$(LD) $(LDFLAGS_MB1) $^ -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.asm
	nasm -felf32 -F dwarf -g $< -o $@

clean:
	rm -rf test.elf limine.o e9print.o memory.o
	rm -rf flanterm.o flanterm_fb.o
	rm -rf e9print.mb.o memory.mb.o cc-runtime.mb.o
	rm -rf multiboot2.o multiboot2.elf multiboot2_trampoline.o
	rm -rf multiboot.o multiboot_trampoline.o multiboot.elf
