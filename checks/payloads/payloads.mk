# Builds the executables and images the checks boot, as make
# check-payloads has it: with the toolchain that builds Limine, for the ports
# the build enabled.

.SUFFIXES:
.DELETE_ON_ERROR:

SRCDIR ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
OUT ?= $(SRCDIR)/checks-build

CC := clang
LD := ld.lld
OBJCOPY := llvm-objcopy
READELF := llvm-readelf
# Clang takes its target as a flag, and GCC from its name.
CC_IS_CLANG := yes
ENABLED_PORTS := bios uefi-ia32 uefi-x86-64 uefi-aarch64 uefi-riscv64 uefi-loongarch64

override PAYLOADS := $(SRCDIR)/checks/payloads
override X86_PORTS := $(filter bios uefi-ia32 uefi-x86-64,$(ENABLED_PORTS))
LIMINE_ARCHS ?= $(if $(X86_PORTS),x86_64) \
    $(patsubst uefi-%,%,$(filter uefi-aarch64 uefi-riscv64 uefi-loongarch64,$(ENABLED_PORTS)))

ifeq ($(CC_IS_CLANG),yes)
override target = -target $(1)-unknown-none-elf
override DIRECT_EXTERN := -fdirect-access-external-data
override GOT_EXTERN := -fno-direct-access-external-data
else
override target :=
override DIRECT_EXTERN := -mdirect-extern-access
override GOT_EXTERN := -mno-direct-extern-access
endif

override COMMON_CFLAGS := \
    -std=gnu11 \
    -O2 \
    -g \
    -Wall \
    -Wextra \
    -Werror \
    -ffreestanding \
    -fno-stack-protector \
    -fno-stack-check \
    -fno-lto \
    -fno-exceptions \
    -fno-builtin \
    -nostdinc \
    -isystem $(SRCDIR)/freestanding-c-hdrs/include \
    -I $(SRCDIR)/limine-protocol/include

override CFLAGS_x86_64 := $(call target,x86_64) -m64 -march=x86-64 -mgeneral-regs-only -mno-red-zone
override CFLAGS_aarch64 := $(call target,aarch64) -mcpu=generic -march=armv8-a+nofp+nosimd \
    -mno-outline-atomics
override CFLAGS_riscv64 := $(call target,riscv64) -march=rv64imac_zicsr_zifencei -mabi=lp64 \
    -mcmodel=medany -mno-relax
override CFLAGS_loongarch64 := $(call target,loongarch64) -march=loongarch64 -mabi=lp64s -msoft-float \
    $(DIRECT_EXTERN)

override STATIC_CFLAGS_x86_64 := -fno-PIC -mcmodel=kernel
override STATIC_CFLAGS_aarch64 := -fno-PIC
override STATIC_CFLAGS_riscv64 := -fno-PIC
override STATIC_CFLAGS_loongarch64 := -fno-PIC

# Per layout beyond static: compiler and link flags. A shared object reaches
# its global symbols through the dynamic symbol table.
override LAYOUT_CFLAGS_pie := -fPIE
override LAYOUT_CFLAGS_pierelr := -fPIE
override LAYOUT_CFLAGS_shared := -fPIC
override SHARED_CFLAGS_loongarch64 := $(GOT_EXTERN)
override LAYOUT_LDFLAGS_pie := -pie --no-dynamic-linker -z text
override LAYOUT_LDFLAGS_pierelr := -pie --no-dynamic-linker -z text -z pack-relative-relocs
override LAYOUT_LDFLAGS_shared := -shared -z text -e _start

override LDFLAGS_x86_64 := -m elf_x86_64
override LDFLAGS_aarch64 := -m aarch64elf
override LDFLAGS_riscv64 := -m elf64lriscv --no-relax
override LDFLAGS_loongarch64 := -m elf64loongarch

# The highest paging mode in the protocol's numbering, per architecture.
override PAGING_TOP_x86_64 := 1
override PAGING_TOP_aarch64 := 1
override PAGING_TOP_riscv64 := 2
override PAGING_TOP_loongarch64 := 0

# Variant name, layout (static, pie, pierelr or shared), extra defines. Paging
# values below that use $(ARCH) are expanded per architecture.
override LIMINE_VARIANTS := \
    full:static: \
    pie:pie: \
    rev0:static:-DTEST_BASE_REVISION=0 \
    rev1:static:-DTEST_BASE_REVISION=1 \
    rev2:static:-DTEST_BASE_REVISION=2 \
    rev3:static:-DTEST_BASE_REVISION=3 \
    rev4:static:-DTEST_BASE_REVISION=4 \
    rev5:static:-DTEST_BASE_REVISION=5 \
    rev7:static:-DTEST_BASE_REVISION=7 \
    norev:static:-DTEST_BASE_REVISION=-1 \
    minimal:static:-DTEST_MINIMAL=1 \
    noentry:static:-DTEST_ENTRY_REQUEST=0 \
    nomp:static:-DTEST_MP_FLAGS=-1 \
    noflanterm:static:-DTEST_FLANTERM_REQUEST=0 \
    nokeepiommu:static:-DTEST_KEEP_IOMMU_REQUEST=0 \
    x2apic:static:-DTEST_MP_FLAGS=1 \
    nopaging:static:-DTEST_PAGING_REV=-1 \
    pagingtop:static:-DTEST_PAGING_MODE=TOP,-DTEST_PAGING_MAX=TOP \
    pagingrev0top:static:-DTEST_PAGING_REV=0,-DTEST_PAGING_MODE=TOP \
    pagingonlytop:static:-DTEST_PAGING_MODE=TOP,-DTEST_PAGING_MIN=TOP,-DTEST_PAGING_MAX=TOP \
    pagingbad:static:-DTEST_PAGING_MODE=0,-DTEST_PAGING_MIN=TOP,-DTEST_PAGING_MAX=0 \
    pagingcapped:static:-DTEST_PAGING_MODE=TOP,-DTEST_PAGING_MAX=0 \
    pagingraised:static:-DTEST_PAGING_MODE=0,-DTEST_PAGING_MIN=TOP,-DTEST_PAGING_MAX=TOP \
    im1:static:-DTEST_INTERNAL_MODULES=1 \
    im2:static:-DTEST_INTERNAL_MODULES=2 \
    im3:static:-DTEST_INTERNAL_MODULES=3 \
    im1rev0:static:-DTEST_INTERNAL_MODULES=1,-DTEST_MODULE_REV=0 \
    dupreq:static:-DTEST_DUPLICATE_REQUEST=1 \
    duprev:static:-DTEST_DUPLICATE_BASE_REVISION=1 \
    outside:static:-DTEST_REQUEST_OUTSIDE_MARKERS=1 \
    nomarkers:static:-DTEST_NO_MARKERS=1 \
    stackbig:static:-DTEST_STACK_SIZE=0x100000 \
    stacksmall:static:-DTEST_STACK_SIZE=0x1000 \
    entropy0:static:-DTEST_ENTROPY_COUNT=0 \
    entropybig:static:-DTEST_ENTROPY_COUNT=10000 \
    nodraw:static:-DTEST_DRAW=0 \
    reqs0:static:-DTEST_BASE_REVISION=0,-DTEST_REQS_SECTION=1,-DTEST_DUPLICATE_REQUEST=1 \
    reqs1:static:-DTEST_BASE_REVISION=1,-DTEST_REQS_SECTION=1 \
    reqsmax:static:-DTEST_BASE_REVISION=0,-DTEST_REQS_SECTION=2 \
    piereqs0:pie:-DTEST_BASE_REVISION=0,-DTEST_REQS_SECTION=1,-DTEST_DUPLICATE_REQUEST=1 \
    pierelr:pierelr: \
    shared:shared:-DTEST_SYMBOLS=1

override LIMINE_SRCS := kernel.c lib.c fdt.c tpm.c

.PHONY: all
all: limine-kernels efi-probes linux-stubs $(if $(X86_PORTS),multiboot-kernels) \
    $(if $(filter bios,$(ENABLED_PORTS)),bios-payloads)

override define limine_variant
# $(1) arch, $(2) name, $(3) layout, $(4) defines
override OBJS_$(1)_$(2) := $$(addprefix $(OUT)/limine/$(1)/$(2)/,$$(LIMINE_SRCS:.c=.o) arch_$(1).o entry_$(1).o)

$(OUT)/limine/$(1)/$(2)/%.o: $(PAYLOADS)/limine/%.c $(wildcard $(PAYLOADS)/limine/*.h) $(PAYLOADS)/payloads.mk
	mkdir -p $$(@D)
	$$(CC) $$(COMMON_CFLAGS) $$(CFLAGS_$(1)) $$(if $$(filter static,$(3)),$$(STATIC_CFLAGS_$(1)),$$(LAYOUT_CFLAGS_$(3))) \
		$$(if $$(filter shared,$(3)),$$(SHARED_CFLAGS_$(1))) -DTEST_VARIANT='"$(2)"' $$(subst TOP,$$(PAGING_TOP_$(1)),$$(subst $$(COMMA), ,$(4))) -c $$< -o $$@

$(OUT)/limine/$(1)/$(2)/%.o: $(PAYLOADS)/limine/%.S $(PAYLOADS)/limine/state.h $(PAYLOADS)/payloads.mk
	mkdir -p $$(@D)
	$$(CC) $$(COMMON_CFLAGS) $$(CFLAGS_$(1)) $$(if $$(filter static,$(3)),$$(STATIC_CFLAGS_$(1)),$$(LAYOUT_CFLAGS_$(3))) \
		$$(if $$(filter shared,$(3)),$$(SHARED_CFLAGS_$(1))) -c $$< -o $$@

$(OUT)/limine/$(1)/$(2)/linker.ld: $(PAYLOADS)/limine/linker.ld.in
	mkdir -p $$(@D)
	$$(CC) -E -P -x c -undef -DBASE=$$(if $$(filter static,$(3)),0xffffffff80000000,0) -DPIE=$$(if $$(filter static,$(3)),0,1) $$< -o $$@

$(OUT)/limine/$(1)/$(2).elf: $$(OBJS_$(1)_$(2)) $(OUT)/limine/$(1)/$(2)/linker.ld
	$$(LD) $$(LDFLAGS_$(1)) -nostdlib -z max-page-size=0x1000 -z noexecstack \
		$$(if $$(filter static,$(3)),-static,$$(LAYOUT_LDFLAGS_$(3))) \
		-T $(OUT)/limine/$(1)/$(2)/linker.ld $$(OBJS_$(1)_$(2)) -o $$@

override LIMINE_ELFS += $(OUT)/limine/$(1)/$(2).elf
endef

override COMMA := ,

$(foreach a,$(LIMINE_ARCHS),$(foreach v,$(LIMINE_VARIANTS),$(eval $(call limine_variant,$(a),$(word 1,$(subst :, ,$(v))),$(word 2,$(subst :, ,$(v))),$(word 3,$(subst :, ,$(v)))))))

.PHONY: limine-kernels
limine-kernels: $(LIMINE_ELFS)

# The EFI probe, for every UEFI target, built the way Limine builds itself:
# PicoEFI's entry stub, self-relocator and linker script.
override EFI_TARGETS := $(subst x86-64,x86_64,$(patsubst uefi-%,%,$(filter uefi-%,$(ENABLED_PORTS))))

override EFI_CFLAGS_x86_64 := $(call target,x86_64) -m64 -march=x86-64 -mgeneral-regs-only -mno-red-zone
override EFI_CFLAGS_ia32 := $(call target,i686) -m32 -march=i686 -mgeneral-regs-only
override EFI_CFLAGS_aarch64 := $(call target,aarch64) -mgeneral-regs-only
override EFI_CFLAGS_riscv64 := $(call target,riscv64) -march=rv64imac_zicsr_zifencei -mabi=lp64 -mno-relax
override EFI_CFLAGS_loongarch64 := $(call target,loongarch64) -march=loongarch64 -mabi=lp64s -msoft-float \
    $(DIRECT_EXTERN)

override EFI_LDFLAGS_x86_64 := -m elf_x86_64
override EFI_LDFLAGS_ia32 := -m elf_i386
override EFI_LDFLAGS_aarch64 := -m aarch64elf
override EFI_LDFLAGS_riscv64 := -m elf64lriscv --no-relax
override EFI_LDFLAGS_loongarch64 := -m elf64loongarch

override EFI_COMMON_CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -Werror -ffreestanding -fno-stack-protector \
    -fno-stack-check -fno-lto -fno-exceptions -fPIE -fshort-wchar -nostdinc \
    -isystem $(SRCDIR)/freestanding-c-hdrs/include -I $(SRCDIR)/picoefi/inc

override define efi_target
$(OUT)/efi/$(1)/%.o: $(PAYLOADS)/efi/%.c $(PAYLOADS)/payloads.mk
	mkdir -p $$(@D)
	$$(CC) $$(EFI_COMMON_CFLAGS) $$(EFI_CFLAGS_$(1)) -c $$< -o $$@

$(OUT)/efi/$(1)/entry.o: $(SRCDIR)/picoefi/$(1)/entry.S
	mkdir -p $$(@D)
	$$(CC) $$(EFI_COMMON_CFLAGS) $$(EFI_CFLAGS_$(1)) -c $$< -o $$@

$(OUT)/efi/$(1)/reloc.o: $(SRCDIR)/picoefi/$(1)/reloc.c
	mkdir -p $$(@D)
	$$(CC) $$(EFI_COMMON_CFLAGS) $$(EFI_CFLAGS_$(1)) -c $$< -o $$@

# The copy bootstrap leaves in common/, which release tarballs keep.
$(OUT)/efi/$(1)/lgrt.o: $(SRCDIR)/common/freestanding-lgrt.s2.c
	mkdir -p $$(@D)
	$$(CC) $$(EFI_COMMON_CFLAGS) $$(EFI_CFLAGS_$(1)) -Wno-error -c $$< -o $$@

$(OUT)/efi/$(1)/efiprobe.elf: $(addprefix $(OUT)/efi/$(1)/,entry.o reloc.o efiprobe.o mem.o lgrt.o)
	$$(LD) $$(EFI_LDFLAGS_$(1)) -nostdlib -pie -z text -z max-page-size=0x1000 --hash-style=gnu \
		-z pack-relative-relocs -T $(SRCDIR)/picoefi/$(1)/link_script.lds $$^ -o $$@

$(OUT)/efi/$(1)/efiprobe.efi: $(OUT)/efi/$(1)/efiprobe.elf
	$$(OBJCOPY) -O binary $$< $$@

override EFI_PROBES += $(OUT)/efi/$(1)/efiprobe.efi
endef

$(foreach t,$(EFI_TARGETS),$(eval $(call efi_target,$(t))))

.PHONY: efi-probes
efi-probes: $(EFI_PROBES)

# Linux stand-ins: an Image for each non-x86 architecture. The arm64 one comes
# in two flavours of the header's physical placement flag, the riscv64 one
# also with a header version Limine refuses.
override LINUX_CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-stack-protector \
    -fno-stack-check -fno-lto -fno-exceptions -fPIE -fvisibility=hidden -nostdinc \
    -isystem $(SRCDIR)/freestanding-c-hdrs/include
override LINUX_CFLAGS_aarch64 := $(CFLAGS_aarch64) -mstrict-align -mgeneral-regs-only
override LINUX_CFLAGS_riscv64 := $(CFLAGS_riscv64)
override LINUX_CFLAGS_loongarch64 := $(CFLAGS_loongarch64)

override define linux_stub
$(OUT)/linux/$(1)/$(2)/stub.o: $(PAYLOADS)/linux/stub.c $(PAYLOADS)/payloads.mk
	mkdir -p $$(@D)
	$$(CC) $$(LINUX_CFLAGS) $$(LINUX_CFLAGS_$(1)) -c $$< -o $$@

$(OUT)/linux/$(1)/$(2)/head.o: $(PAYLOADS)/linux/head_$(1).S $(PAYLOADS)/payloads.mk
	mkdir -p $$(@D)
	$$(CC) $$(LINUX_CFLAGS) $$(LINUX_CFLAGS_$(1)) $(3) -c $$< -o $$@

$(OUT)/linux/$(1)/$(2)/Image.elf: $(OUT)/linux/$(1)/$(2)/head.o $(OUT)/linux/$(1)/$(2)/stub.o $(PAYLOADS)/linux/stub.ld
	$$(LD) $$(LDFLAGS_$(1)) -nostdlib -static -z max-page-size=0x1000 --defsym=LOAD_OFFSET=0x200000 \
		-T $(PAYLOADS)/linux/stub.ld $(OUT)/linux/$(1)/$(2)/head.o $(OUT)/linux/$(1)/$(2)/stub.o -o $$@

$(OUT)/linux/$(1)/$(2)/Image: $(OUT)/linux/$(1)/$(2)/Image.elf
	$$(OBJCOPY) -O binary $$< $$@

override LINUX_STUBS += $(OUT)/linux/$(1)/$(2)/Image
endef

ifneq ($(filter aarch64,$(LIMINE_ARCHS)),)
$(eval $(call linux_stub,aarch64,default,-DIMAGE_FLAGS=0x2))
$(eval $(call linux_stub,aarch64,anywhere,-DIMAGE_FLAGS=0xa))
endif
ifneq ($(filter riscv64,$(LIMINE_ARCHS)),)
$(eval $(call linux_stub,riscv64,default,))
$(eval $(call linux_stub,riscv64,proto01,-DIMAGE_VERSION=0x1))
endif
ifneq ($(filter loongarch64,$(LIMINE_ARCHS)),)
$(eval $(call linux_stub,loongarch64,default,))
endif


# The x86 bzImage stand-in: position independent 32-bit and 64-bit builds
# behind the real-mode header.
override X86_STUB_CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-stack-protector \
    -fno-stack-check -fno-lto -fno-exceptions -mgeneral-regs-only -nostdinc \
    -isystem $(SRCDIR)/freestanding-c-hdrs/include

$(OUT)/linux/x86/part_a.bin: $(PAYLOADS)/linux/x86_stub.c $(PAYLOADS)/linux/x86_entry32.asm $(PAYLOADS)/linux/x86_part.ld
	mkdir -p $(@D)
	$(CC) $(X86_STUB_CFLAGS) $(call target,i686) -m32 -march=i686 -fPIE -c $< -o $(@D)/stub32.o
	nasm -f elf32 -Wall -w-reloc -Werror $(PAYLOADS)/linux/x86_entry32.asm -o $(@D)/entry32.o
	$(LD) -m elf_i386 -nostdlib -pie -z text --no-dynamic-linker --defsym=PART_BASE=0 \
		-T $(PAYLOADS)/linux/x86_part.ld $(@D)/entry32.o $(@D)/stub32.o -o $(@D)/part_a.elf
	@if $(READELF) -r $(@D)/part_a.elf | grep -q R_386; then \
		echo 'the 32-bit stub part needs relocating' >&2; exit 1; fi
	$(OBJCOPY) -O binary $(@D)/part_a.elf $@

$(OUT)/linux/x86/part_b.bin: $(PAYLOADS)/linux/x86_stub.c $(PAYLOADS)/linux/x86_entry64.asm $(PAYLOADS)/linux/x86_part.ld
	mkdir -p $(@D)
	$(CC) $(X86_STUB_CFLAGS) $(call target,x86_64) -m64 -march=x86-64 -mno-red-zone -fPIE -c $< -o $(@D)/stub64.o
	nasm -f elf64 -Wall -w-reloc -Werror $(PAYLOADS)/linux/x86_entry64.asm -o $(@D)/entry64.o
	$(LD) -m elf_x86_64 -nostdlib -pie -z text --no-dynamic-linker --defsym=PART_BASE=0 \
		-T $(PAYLOADS)/linux/x86_part.ld $(@D)/entry64.o $(@D)/stub64.o -o $(@D)/part_b.elf
	@if $(READELF) -r $(@D)/part_b.elf | grep -q R_X86_64; then \
		echo 'the 64-bit stub part needs relocating' >&2; exit 1; fi
	$(OBJCOPY) -O binary $(@D)/part_b.elf $@

$(OUT)/linux/x86/pm.bin: $(PAYLOADS)/linux/x86_pm.asm $(OUT)/linux/x86/part_a.bin $(OUT)/linux/x86/part_b.bin
	nasm -f bin -Wall -w-reloc -Werror -i $(OUT)/linux/x86/ $< -o $@

# Variant: version, loadflags, xloadflags, relocatable, preferred address,
# initrd_addr_max.
override X86_BZIMAGE_VARIANTS := \
    default:0x020f:0x21:0:1:0x1000000:0x37ffffff \
    nonreloc:0x020f:0x21:0:0:0x1000000:0x37ffffff \
    proto209:0x0209:0x21:0:0:0:0x37ffffff \
    above4g:0x020f:0x21:3:1:0x100000000:0x37ffffff \
    lowinitrd:0x020f:0x21:0:1:0x1000000:0x07ffffff \
    proto201:0x0201:0x21:0:1:0x1000000:0x37ffffff \
    nohigh:0x020f:0x20:0:1:0x1000000:0x37ffffff

override define x86_bzimage
$(OUT)/linux/x86/$(1)/bzImage: $(PAYLOADS)/linux/x86_head.asm $(OUT)/linux/x86/pm.bin $(PAYLOADS)/payloads.mk
	mkdir -p $$(@D)
	nasm -f bin -Wall -w-reloc -Werror -i $(OUT)/linux/x86/ -DVERSION=$(2) -DLOADFLAGS=$(3) -DXLOADFLAGS=$(4) \
		-DRELOCATABLE=$(5) -DPREF_ADDRESS=$(6) -DINITRD_ADDR_MAX=$(7) $$< -o $$@

override X86_BZIMAGES += $(OUT)/linux/x86/$(1)/bzImage
endef

$(foreach v,$(X86_BZIMAGE_VARIANTS),$(eval $(call x86_bzimage,$(word 1,$(subst :, ,$(v))),$(word 2,$(subst :, ,$(v))),$(word 3,$(subst :, ,$(v))),$(word 4,$(subst :, ,$(v))),$(word 5,$(subst :, ,$(v))),$(word 6,$(subst :, ,$(v))),$(word 7,$(subst :, ,$(v))))))

$(OUT)/bios/bootrec.bin: $(PAYLOADS)/bios/bootrec.asm
	mkdir -p $(@D)
	nasm -f bin -Wall -w-reloc -Werror $< -o $@

$(OUT)/bios/a20off.bin: $(PAYLOADS)/bios/a20off.asm
	mkdir -p $(@D)
	nasm -f bin -Wall -w-reloc -Werror $< -o $@

$(OUT)/bios/a20off-noint15.bin: $(PAYLOADS)/bios/a20off.asm
	mkdir -p $(@D)
	nasm -f bin -Wall -w-reloc -Werror -DBREAK_INT15 $< -o $@

.PHONY: linux-stubs
linux-stubs: $(LINUX_STUBS) $(if $(X86_PORTS),$(X86_BZIMAGES))

.PHONY: bios-payloads
bios-payloads: $(OUT)/bios/bootrec.bin $(OUT)/bios/a20off.bin $(OUT)/bios/a20off-noint15.bin

# The Multiboot 1 check kernel, in the header flavours the checks need. Variant:
# name, header flags, extra defines. The a.out kludge ones are flat binaries,
# and one ELF64 container holds the plain 32-bit kernel too.
override MB_CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-stack-protector \
    -fno-stack-check -fno-lto -fno-exceptions -fno-builtin -fno-PIC -mgeneral-regs-only -nostdinc \
    -isystem $(SRCDIR)/freestanding-c-hdrs/include $(call target,i686) -m32 -march=i686

override MB1_VARIANTS := \
    elf:0x2: \
    video:0x6:-DMODE_TYPE=0,-DWIDTH=800,-DHEIGHT=600,-DDEPTH=32 \
    text:0x6:-DMODE_TYPE=1,-DWIDTH=80,-DHEIGHT=25,-DDEPTH=0 \
    align:0x3: \
    pic:0x2:-DPIC_TEST \
    aout:0x10002:-DAOUT \
    aoutwhole:0x10002:-DAOUT,-DAOUT_WHOLE_FILE

override define mb1_kernel
$(OUT)/multiboot/mb1/$(1)/kernel.elf: $(PAYLOADS)/multiboot/mb1_head.asm $(PAYLOADS)/multiboot/mb1.c \
		$(PAYLOADS)/multiboot/mbio.c $(PAYLOADS)/multiboot/mbio.h $(PAYLOADS)/multiboot/mb_idt.asm \
		$(PAYLOADS)/multiboot/mb.ld $(PAYLOADS)/payloads.mk
	mkdir -p $$(@D)
	nasm -f elf32 -Wall -w-reloc -Werror -DFLAGS=$(2) $$(subst $$(COMMA), ,$(3)) \
		$(PAYLOADS)/multiboot/mb1_head.asm -o $$(@D)/head.o
	$$(CC) $$(MB_CFLAGS) $$(subst $$(COMMA), ,$(3)) -c $(PAYLOADS)/multiboot/mb1.c -o $$(@D)/mb1.o
	$$(CC) $$(MB_CFLAGS) -c $(PAYLOADS)/multiboot/mbio.c -o $$(@D)/mbio.o
	nasm -f elf32 -Wall -w-reloc -Werror $(PAYLOADS)/multiboot/mb_idt.asm -o $$(@D)/idt.o
	$$(LD) -m elf_i386 -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T $(PAYLOADS)/multiboot/mb.ld \
		$$(@D)/head.o $$(@D)/mb1.o $$(@D)/mbio.o $$(@D)/idt.o -o $$@

$(OUT)/multiboot/mb1/$(1)/kernel.bin: $(OUT)/multiboot/mb1/$(1)/kernel.elf
	$$(OBJCOPY) -O binary $$< $$@

override MB_KERNELS += $(OUT)/multiboot/mb1/$(1)/kernel.elf $(OUT)/multiboot/mb1/$(1)/kernel.bin
endef

$(foreach v,$(MB1_VARIANTS),$(eval $(call mb1_kernel,$(word 1,$(subst :, ,$(v))),$(word 2,$(subst :, ,$(v))),$(word 3,$(subst :, ,$(v))))))

$(OUT)/multiboot/mb1/elf/kernel64.elf: $(OUT)/multiboot/mb1/elf/kernel.elf
	$(OBJCOPY) -O elf64-x86-64 $< $@

override MB1_HEADER_OFFSETS := 8160 8164 8180 8184 8192

$(OUT)/multiboot/mb1/hdr%.bin: $(PAYLOADS)/multiboot/mb1_flat.asm
	mkdir -p $(@D)
	nasm -f bin -Wall -w-reloc -Werror -DHEADER_OFFSET=$* $< -o $@

# The Multiboot 2 check kernel: position independent code, linked at 3MiB like
# the Multiboot 1 one, which the relocatable header tag can move. Only the
# header may hold absolute addresses. Variant: name, defines.
override MB2_VARIANTS := \
    elf: \
    infoopt:-DINFO_FLAGS=1 \
    fb:-DFB,-DWIDTH=800,-DHEIGHT=600,-DDEPTH=32 \
    text:-DCONSOLE=3,-DNO_FB_REQUEST \
    modalign:-DMODULE_ALIGN \
    aout:-DADDRESS,-DLOAD_ADDR=__load_start,-DLOAD_END=__load_end \
    aoutm1:-DADDRESS,-DLOAD_ADDR=0xffffffff,-DLOAD_END=0 \
    reloclow:-DRELOC,-DRELOC_MIN=0x400000,-DRELOC_MAX=0x1000000,-DRELOC_ALIGN=0x200000,-DRELOC_PREF=1 \
    relochigh:-DRELOC,-DRELOC_MIN=0x400000,-DRELOC_MAX=0x1000000,-DRELOC_ALIGN=0x200000,-DRELOC_PREF=2 \
    reloczero:-DINFO_FLAGS=1,-DRELOC,-DRELOC_MIN=0,-DRELOC_MAX=0x1000000,-DRELOC_ALIGN=0x1000,-DRELOC_PREF=1 \
    reloczeronone:-DINFO_FLAGS=1,-DRELOC,-DRELOC_MIN=0,-DRELOC_MAX=0x1000000,-DRELOC_ALIGN=0x1000,-DRELOC_PREF=0 \
    efibs:-DEFI_BS=1 \
    efibsreq:-DEFI_BS=0

override define mb2_kernel
$(OUT)/multiboot/mb2/$(1)/kernel.elf: $(PAYLOADS)/multiboot/mb2_head.asm $(PAYLOADS)/multiboot/mb2.c \
		$(PAYLOADS)/multiboot/mbio.c $(PAYLOADS)/multiboot/mbio.h $(PAYLOADS)/multiboot/mb.ld $(PAYLOADS)/payloads.mk
	mkdir -p $$(@D)
	nasm -f elf32 -Wall -w-reloc -Werror $$(subst $$(COMMA), ,$(2)) $(PAYLOADS)/multiboot/mb2_head.asm -o $$(@D)/head.o
	$$(CC) $$(MB_CFLAGS) -fPIE -c $(PAYLOADS)/multiboot/mb2.c -o $$(@D)/mb2.o
	$$(CC) $$(MB_CFLAGS) -fPIE -fvisibility=hidden -c $(PAYLOADS)/multiboot/mbio.c -o $$(@D)/mbio.o
	$$(LD) -m elf_i386 -nostdlib -static --emit-relocs -z max-page-size=0x1000 -z noexecstack \
		-T $(PAYLOADS)/multiboot/mb.ld $$(@D)/head.o $$(@D)/mb2.o $$(@D)/mbio.o -o $$(@D)/relocs.elf
	@$$(READELF) -r $$(@D)/relocs.elf | awk '/^Relocation section/ { sec = $$$$3 } \
		/R_386_32 |R_386_GOT32/ && sec !~ /multiboot/ { bad = 1 } END { exit bad }' \
		|| { echo 'the Multiboot 2 kernel holds an absolute address' >&2; exit 1; }
	$$(OBJCOPY) --wildcard --remove-section='.rel*' $$(@D)/relocs.elf $$@

$(OUT)/multiboot/mb2/$(1)/kernel.bin: $(OUT)/multiboot/mb2/$(1)/kernel.elf
	$$(OBJCOPY) -O binary $$< $$@

$(OUT)/multiboot/mb2/$(1)/kernel64.elf: $(OUT)/multiboot/mb2/$(1)/kernel.elf
	$$(OBJCOPY) -O elf64-x86-64 $$< $$@

override MB_KERNELS += $(OUT)/multiboot/mb2/$(1)/kernel.elf $(OUT)/multiboot/mb2/$(1)/kernel.bin \
    $(OUT)/multiboot/mb2/$(1)/kernel64.elf
endef

$(foreach v,$(MB2_VARIANTS),$(eval $(call mb2_kernel,$(word 1,$(subst :, ,$(v))),$(word 2,$(subst :, ,$(v))))))

override MB2_HEADER_OFFSETS := 32704 32712 32752 32760 32768

$(OUT)/multiboot/mb2/hdr%.bin: $(PAYLOADS)/multiboot/mb2_flat.asm
	mkdir -p $(@D)
	nasm -f bin -Wall -w-reloc -Werror -DHEADER_OFFSET=$* $< -o $@

.PHONY: multiboot-kernels
multiboot-kernels: $(MB_KERNELS) $(OUT)/multiboot/mb1/elf/kernel64.elf \
	$(foreach o,$(MB1_HEADER_OFFSETS),$(OUT)/multiboot/mb1/hdr$(o).bin) \
	$(foreach o,$(MB2_HEADER_OFFSETS),$(OUT)/multiboot/mb2/hdr$(o).bin)

.PHONY: clean
clean:
	rm -rf $(OUT)
