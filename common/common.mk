.SUFFIXES:

include $(TOOLCHAIN_FILE)
export OBJDUMP_FOR_TARGET

override SRCDIR := $(shell pwd -P)

override SPACE := $(subst ,, )

override MKESCAPE = $(subst $(SPACE),\ ,$(1))
override SHESCAPE = $(subst ','\'',$(1))
override OBJESCAPE = $(subst .a ,.a' ',$(subst .o ,.o' ',$(call SHESCAPE,$(1))))

COM_OUTPUT := false
E9_OUTPUT := false

override S2CFLAGS := -Os

override BASE_CFLAGS := $(CFLAGS_FOR_TARGET)

override CFLAGS_FOR_TARGET += \
    -g \
    -Wall \
    -Wextra \
    -Wshadow \
    -Wvla \
    $(WERROR_FLAG) \
    -std=gnu11 \
    -nostdinc \
    -ffreestanding \
    -ffunction-sections \
    -fdata-sections \
    -fno-stack-protector \
    -fno-stack-check \
    -fno-omit-frame-pointer \
    -fno-strict-aliasing \
    -fno-lto

override CPPFLAGS_FOR_TARGET := \
    -I . \
    -I libc-compat \
    -I ../limine-protocol/include \
    -I ../flanterm/src \
    -I ../libfdt/src \
    -I '$(call SHESCAPE,$(BUILDDIR))/..' \
    -isystem ../freestnd-c-hdrs/include \
    $(CPPFLAGS_FOR_TARGET) \
    -DCOM_OUTPUT=$(COM_OUTPUT) \
    -DE9_OUTPUT=$(E9_OUTPUT) \
    -DLIMINE_API_REVISION=3 \
    -DFLANTERM_IN_FLANTERM \
    -MMD \
    -MP

$(call MKESCAPE,$(BUILDDIR))/libfdt/src/fdt_overlay.o: override CFLAGS_FOR_TARGET += \
    -Wno-unused-parameter

$(call MKESCAPE,$(BUILDDIR))/flanterm/src/flanterm_backends/fb.o: override CPPFLAGS_FOR_TARGET += \
    -DFLANTERM_FB_DISABLE_BUMP_ALLOC

override NASMFLAGS_FOR_TARGET += \
    -g \
    -Wall \
    -w-unknown-warning \
    -w-reloc \
    $(WERROR_FLAG)

override NASMFLAGS_FOR_TARGET := \
    $(patsubst -g,-g -F dwarf,$(NASMFLAGS_FOR_TARGET))

ifeq ($(TARGET),bios)
    override CFLAGS_FOR_TARGET += \
        -fno-PIC \
        -m32 \
        -march=i686 \
        -mabi=sysv \
        -mno-80387
    override CPPFLAGS_FOR_TARGET := \
        $(CPPFLAGS_FOR_TARGET) \
        -DBIOS
    override NASMFLAGS_FOR_TARGET := \
        -f elf32 \
        $(NASMFLAGS_FOR_TARGET) \
        -DIA32_TARGET \
        -DBIOS
endif

ifeq ($(TARGET),uefi-x86-64)
    override CFLAGS_FOR_TARGET += \
        -fPIE \
        -fshort-wchar \
        -m64 \
        -march=x86-64 \
        -mabi=sysv \
        -mno-80387 \
        -mno-mmx \
        -mno-sse \
        -mno-sse2 \
        -mno-red-zone
    override CPPFLAGS_FOR_TARGET := \
        -I ../nyu-efi/inc \
        $(CPPFLAGS_FOR_TARGET) \
        -DUEFI
    override NASMFLAGS_FOR_TARGET := \
        -f elf64 \
        $(NASMFLAGS_FOR_TARGET) \
        -DX86_64_TARGET \
        -DUEFI
endif

ifeq ($(TARGET),uefi-ia32)
    override CFLAGS_FOR_TARGET += \
        -fPIE \
        -fshort-wchar \
        -m32 \
        -march=i686 \
        -mabi=sysv \
        -mno-80387
    override CPPFLAGS_FOR_TARGET := \
        -I ../nyu-efi/inc \
        $(CPPFLAGS_FOR_TARGET) \
        -DUEFI
    override NASMFLAGS_FOR_TARGET := \
        -f elf32 \
        $(NASMFLAGS_FOR_TARGET) \
        -DIA32_TARGET \
        -DUEFI
endif

ifeq ($(TARGET),uefi-aarch64)
    override CFLAGS_FOR_TARGET += \
        -fPIE \
        -fshort-wchar \
        -mcpu=generic \
        -march=armv8-a+nofp+nosimd \
        -mgeneral-regs-only
    override CPPFLAGS_FOR_TARGET := \
        -I ../nyu-efi/inc \
        $(CPPFLAGS_FOR_TARGET) \
        -DUEFI
endif

ifeq ($(TARGET),uefi-riscv64)
    override CFLAGS_FOR_TARGET += \
        -fPIE \
        -fshort-wchar

    ifeq ($(CC_FOR_TARGET_IS_CLANG),yes)
        override CFLAGS_FOR_TARGET += -march=rv64imac
    else
        override CFLAGS_FOR_TARGET += -march=rv64imac_zicsr_zifencei
    endif

    override CFLAGS_FOR_TARGET += \
        -mabi=lp64 \
        -mno-relax

    override CPPFLAGS_FOR_TARGET := \
        -I ../nyu-efi/inc \
        $(CPPFLAGS_FOR_TARGET) \
        -DUEFI
endif

ifeq ($(TARGET),uefi-loongarch64)
    override CFLAGS_FOR_TARGET += \
        -fPIE \
        -fshort-wchar \
        -march=loongarch64 \
        -mabi=lp64s \
        -mfpu=none \
        -msimd=none \
        -mno-relax

    override CPPFLAGS_FOR_TARGET := \
        -I ../nyu-efi/inc \
        $(CPPFLAGS_FOR_TARGET) \
        -DUEFI
endif

override LDFLAGS_FOR_TARGET += \
    -nostdlib \
    -z max-page-size=0x1000 \
    --gc-sections

ifeq ($(TARGET),bios)
    override LDFLAGS_FOR_TARGET += \
        -m elf_i386 \
        -static \
        --build-id=sha1
endif

ifeq ($(TARGET),uefi-x86-64)
    override LDFLAGS_FOR_TARGET += \
        -m elf_x86_64 \
        -pie \
        -z text
endif

ifeq ($(TARGET),uefi-ia32)
    override LDFLAGS_FOR_TARGET += \
        -m elf_i386 \
        -pie \
        -z text
endif

ifeq ($(TARGET),uefi-aarch64)
    override LDFLAGS_FOR_TARGET += \
        -m aarch64elf \
        -pie \
        -z text
endif

ifeq ($(TARGET),uefi-riscv64)
    override LDFLAGS_FOR_TARGET += \
        -m elf64lriscv \
        --no-relax \
        -pie \
        -z text
endif

ifeq ($(TARGET),uefi-loongarch64)
    override LDFLAGS_FOR_TARGET += \
        -m elf64loongarch \
        --no-relax \
        -pie \
        -z text
endif

ifeq ($(TARGET),bios)
    override C_FILES := $(shell cd .. && find common flanterm/src libfdt/src -type f -name '*.c' | LC_ALL=C sort)
    override S_FILES := $(shell cd .. && find common -type f -name '*.S' | LC_ALL=C sort)

    override ASMX86_FILES := $(shell cd .. && find common -type f -name '*.asm_x86' | LC_ALL=C sort)
    override ASM32_FILES := $(shell cd .. && find common -type f -name '*.asm_ia32' | LC_ALL=C sort)
    override ASMB_FILES := $(shell cd .. && find common -type f -name '*.asm_bios_ia32' | LC_ALL=C sort)

    override OBJ := $(addprefix $(call MKESCAPE,$(BUILDDIR))/, $(C_FILES:.c=.o) $(S_FILES:.S=.o) $(ASM32_FILES:.asm_ia32=.o) $(ASMB_FILES:.asm_bios_ia32=.o) $(ASMX86_FILES:.asm_x86=.o))
    override OBJ_S2 := $(filter %.s2.o,$(OBJ))
endif
ifeq ($(TARGET),uefi-x86-64)
    override C_FILES := $(shell cd .. && find common nyu-efi/x86_64 flanterm/src libfdt/src -type f -name '*.c' | LC_ALL=C sort)
    override S_FILES := $(shell cd .. && find common nyu-efi/x86_64 -type f -name '*.S' | LC_ALL=C sort)

    override ASMX86_FILES := $(shell cd .. && find common -type f -name '*.asm_x86' | LC_ALL=C sort)
    override ASM64_FILES := $(shell cd .. && find common -type f -name '*.asm_x86_64' | LC_ALL=C sort)
    override ASM64U_FILES := $(shell cd .. && find common -type f -name '*.asm_uefi_x86_64' | LC_ALL=C sort)

    override OBJ := $(addprefix $(call MKESCAPE,$(BUILDDIR))/, $(C_FILES:.c=.o) $(S_FILES:.S=.o) $(ASM64_FILES:.asm_x86_64=.o) $(ASM64U_FILES:.asm_uefi_x86_64=.o) $(ASMX86_FILES:.asm_x86=.o))
endif
ifeq ($(TARGET),uefi-ia32)
    override C_FILES := $(shell cd .. && find common nyu-efi/ia32 flanterm/src libfdt/src -type f -name '*.c' | LC_ALL=C sort)
    override S_FILES := $(shell cd .. && find common nyu-efi/ia32 -type f -name '*.S' | LC_ALL=C sort)

    override ASMX86_FILES := $(shell cd .. && find common -type f -name '*.asm_x86' | LC_ALL=C sort)
    override ASM32_FILES := $(shell cd .. && find common -type f -name '*.asm_ia32' | LC_ALL=C sort)
    override ASM32U_FILES := $(shell cd .. && find common -type f -name '*.asm_uefi_ia32' | LC_ALL=C sort)

    override OBJ := $(addprefix $(call MKESCAPE,$(BUILDDIR))/, $(C_FILES:.c=.o) $(S_FILES:.S=.o) $(ASM32_FILES:.asm_ia32=.o) $(ASM32U_FILES:.asm_uefi_ia32=.o) $(ASMX86_FILES:.asm_x86=.o))
endif
ifeq ($(TARGET),uefi-aarch64)
    override C_FILES := $(shell cd .. && find common nyu-efi/aarch64 flanterm/src libfdt/src -type f -name '*.c' | LC_ALL=C sort)
    override S_FILES := $(shell cd .. && find common nyu-efi/aarch64 -type f -name '*.S' | LC_ALL=C sort)

    override ASM64_FILES := $(shell cd .. && find common -type f -name '*.asm_aarch64' | LC_ALL=C sort)
    override ASM64U_FILES := $(shell cd .. && find common -type f -name '*.asm_uefi_aarch64' | LC_ALL=C sort)

    override OBJ := $(addprefix $(call MKESCAPE,$(BUILDDIR))/, $(C_FILES:.c=.o) $(S_FILES:.S=.o) $(ASM64_FILES:.asm_aarch64=.o) $(ASM64U_FILES:.asm_uefi_aarch64=.o))
endif
ifeq ($(TARGET),uefi-riscv64)
    override C_FILES := $(shell cd .. && find common nyu-efi/riscv64 flanterm/src libfdt/src -type f -name '*.c' | LC_ALL=C sort)
    override S_FILES := $(shell cd .. && find common nyu-efi/riscv64 -type f -name '*.S' | LC_ALL=C sort)

    override ASM64_FILES := $(shell cd .. && find common -type f -name '*.asm_riscv64' | LC_ALL=C sort)
    override ASM64U_FILES := $(shell cd .. && find common -type f -name '*.asm_uefi_riscv64' | LC_ALL=C sort)

    override OBJ := $(addprefix $(call MKESCAPE,$(BUILDDIR))/, $(C_FILES:.c=.o) $(S_FILES:.S=.o) $(ASM64_FILES:.asm_riscv64=.o) $(ASM64U_FILES:.asm_uefi_riscv64=.o))
endif
ifeq ($(TARGET),uefi-loongarch64)
    override C_FILES := $(shell cd .. && find common nyu-efi/loongarch64 flanterm/src libfdt/src -type f -name '*.c' | LC_ALL=C sort)
    override S_FILES := $(shell cd .. && find common nyu-efi/loongarch64 -type f -name '*.S' | LC_ALL=C sort)

    override ASM64_FILES := $(shell cd .. && find common -type f -name '*.asm_loongarch64' | LC_ALL=C sort)
    override ASM64U_FILES := $(shell cd .. && find common -type f -name '*.asm_uefi_loongarch64' | LC_ALL=C sort)

    override OBJ := $(addprefix $(call MKESCAPE,$(BUILDDIR))/, $(C_FILES:.c=.o) $(S_FILES:.S=.o) $(ASM64_FILES:.asm_loongarch64=.o) $(ASM64U_FILES:.asm_uefi_loongarch64=.o))
endif

override HEADER_DEPS := $(addprefix $(call MKESCAPE,$(BUILDDIR))/, $(C_FILES:.c=.d) $(C_FILES:.S=.d))

.PHONY: all

ifeq ($(TARGET),bios)
all: $(call MKESCAPE,$(BUILDDIR))/limine-bios.sys $(call MKESCAPE,$(BUILDDIR))/stage2.bin.gz
endif
ifeq ($(TARGET),uefi-x86-64)
all: $(call MKESCAPE,$(BUILDDIR))/BOOTX64.EFI
endif
ifeq ($(TARGET),uefi-ia32)
all: $(call MKESCAPE,$(BUILDDIR))/BOOTIA32.EFI
endif
ifeq ($(TARGET),uefi-aarch64)
all: $(call MKESCAPE,$(BUILDDIR))/BOOTAA64.EFI
endif
ifeq ($(TARGET),uefi-riscv64)
all: $(call MKESCAPE,$(BUILDDIR))/BOOTRISCV64.EFI
endif
ifeq ($(TARGET),uefi-loongarch64)
all: $(call MKESCAPE,$(BUILDDIR))/BOOTLOONGARCH64.EFI
endif

ifeq ($(TARGET),bios)

$(call MKESCAPE,$(BUILDDIR))/stage2.bin.gz: $(call MKESCAPE,$(BUILDDIR))/stage2.bin
	gzip -n -9 < '$(call SHESCAPE,$<)' > '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/stage2.bin: $(call MKESCAPE,$(BUILDDIR))/limine-bios.sys
	dd if='$(call SHESCAPE,$<)' bs=$$(( 0x$$("$(READELF_FOR_TARGET)" -S '$(call SHESCAPE,$(BUILDDIR))/limine.elf' | $(GREP) '\.text\.stage3' | $(SED) 's/^.*] //' | $(AWK) '{print $$3}' | $(SED) 's/^0*//') - 0xf000 )) count=1 of='$(call SHESCAPE,$@)' 2>/dev/null

$(call MKESCAPE,$(BUILDDIR))/stage2.map.o: $(call MKESCAPE,$(BUILDDIR))/limine_nomap.elf
	cd '$(call SHESCAPE,$(BUILDDIR))' && \
		'$(call SHESCAPE,$(SRCDIR))/gensyms.sh' '$(call SHESCAPE,$<)' stage2 32 '\.text\.stage2'
	$(CC_FOR_TARGET) $(CFLAGS_FOR_TARGET) $(CPPFLAGS_FOR_TARGET) -c '$(call SHESCAPE,$(BUILDDIR))/stage2.map.S' -o '$(call SHESCAPE,$@)'
	rm -f '$(call SHESCAPE,$(BUILDDIR))/stage2.map.S' '$(call SHESCAPE,$(BUILDDIR))/stage2.map.d'

$(call MKESCAPE,$(BUILDDIR))/full.map.o: $(call MKESCAPE,$(BUILDDIR))/limine_nos3map.elf
	cd '$(call SHESCAPE,$(BUILDDIR))' && \
		'$(call SHESCAPE,$(SRCDIR))/gensyms.sh' '$(call SHESCAPE,$<)' full 32 '\.text'
	$(CC_FOR_TARGET) $(CFLAGS_FOR_TARGET) $(CPPFLAGS_FOR_TARGET) -c '$(call SHESCAPE,$(BUILDDIR))/full.map.S' -o '$(call SHESCAPE,$@)'
	rm -f '$(call SHESCAPE,$(BUILDDIR))/full.map.S' '$(call SHESCAPE,$(BUILDDIR))/full.map.d'

$(call MKESCAPE,$(BUILDDIR))/limine-bios.sys: $(call MKESCAPE,$(BUILDDIR))/limine_stage2only.elf $(call MKESCAPE,$(BUILDDIR))/limine.elf
	$(OBJCOPY_FOR_TARGET) -O binary '$(call SHESCAPE,$(BUILDDIR))/limine.elf' '$(call SHESCAPE,$@)'
	chmod -x '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/linker_stage2only.ld: linker_bios.ld.in
	$(MKDIR_P) '$(call SHESCAPE,$(BUILDDIR))'
	$(CC_FOR_TARGET) -x c -E -P -undef -DLINKER_STAGE2ONLY linker_bios.ld.in -o '$(call SHESCAPE,$(BUILDDIR))/linker_stage2only.ld'

$(call MKESCAPE,$(BUILDDIR))/limine_stage2only.elf: $(OBJ_S2)
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/linker_stage2only.ld'
	$(LD_FOR_TARGET) '$(call OBJESCAPE,$^)' $(LDFLAGS_FOR_TARGET) -T'$(call SHESCAPE,$(BUILDDIR))/linker_stage2only.ld' -o '$(call SHESCAPE,$@)' || \
		( echo "This error may mean that stage 2 was trying to use stage 3 symbols before loading stage 3" && \
		  false )

$(call MKESCAPE,$(BUILDDIR))/linker_nos2map.ld: linker_bios.ld.in
	$(MKDIR_P) '$(call SHESCAPE,$(BUILDDIR))'
	$(CC_FOR_TARGET) -x c -E -P -undef -DLINKER_NOMAP -DLINKER_NOS2MAP linker_bios.ld.in -o '$(call SHESCAPE,$(BUILDDIR))/linker_nos2map.ld'

$(call MKESCAPE,$(BUILDDIR))/empty:
	touch '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/limine_nomap.elf: $(OBJ)
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/empty'
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/linker_nos2map.ld'
	$(LD_FOR_TARGET) '$(call OBJESCAPE,$^)' $(LDFLAGS_FOR_TARGET) -T'$(call SHESCAPE,$(BUILDDIR))/linker_nos2map.ld' -o '$(call SHESCAPE,$@)'
	$(OBJCOPY_FOR_TARGET) -O binary --only-section=.note.gnu.build-id '$(call SHESCAPE,$@)' '$(call SHESCAPE,$(BUILDDIR))/build-id.s2.bin'
	cd '$(call SHESCAPE,$(BUILDDIR))' && \
		$(OBJCOPY_FOR_TARGET) -I binary -B i386 -O elf32-i386 build-id.s2.bin build-id.s2.o && \
		$(OBJCOPY_FOR_TARGET) --add-section .note.GNU-stack='$(call SHESCAPE,$(BUILDDIR))/empty' --set-section-flags .note.GNU-stack=noload,readonly build-id.s2.o
	$(OBJCOPY_FOR_TARGET) -O binary --only-section=.note.gnu.build-id '$(call SHESCAPE,$@)' '$(call SHESCAPE,$(BUILDDIR))/build-id.s3.bin'
	cd '$(call SHESCAPE,$(BUILDDIR))' && \
		$(OBJCOPY_FOR_TARGET) -I binary -B i386 -O elf32-i386 build-id.s3.bin build-id.s3.o && \
		$(OBJCOPY_FOR_TARGET) --add-section .note.GNU-stack='$(call SHESCAPE,$(BUILDDIR))/empty' --set-section-flags .note.GNU-stack=noload,readonly build-id.s3.o
	$(LD_FOR_TARGET) '$(call OBJESCAPE,$^)' '$(call SHESCAPE,$(BUILDDIR))/build-id.s2.o' '$(call SHESCAPE,$(BUILDDIR))/build-id.s3.o' $(LDFLAGS_FOR_TARGET) -T'$(call SHESCAPE,$(BUILDDIR))/linker_nos2map.ld' -o '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/linker_nomap.ld: linker_bios.ld.in
	$(MKDIR_P) '$(call SHESCAPE,$(BUILDDIR))'
	$(CC_FOR_TARGET) -x c -E -P -undef -DLINKER_NOMAP linker_bios.ld.in -o '$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld'

$(call MKESCAPE,$(BUILDDIR))/limine_nos3map.elf: $(OBJ) $(call MKESCAPE,$(BUILDDIR))/stage2.map.o
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/empty'
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld'
	$(LD_FOR_TARGET) '$(call OBJESCAPE,$^)' $(LDFLAGS_FOR_TARGET) -T'$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld' -o '$(call SHESCAPE,$@)'
	$(OBJCOPY_FOR_TARGET) -O binary --only-section=.note.gnu.build-id '$(call SHESCAPE,$@)' '$(call SHESCAPE,$(BUILDDIR))/build-id.s2.bin'
	cd '$(call SHESCAPE,$(BUILDDIR))' && \
		$(OBJCOPY_FOR_TARGET) -I binary -B i386 -O elf32-i386 build-id.s2.bin build-id.s2.o && \
		$(OBJCOPY_FOR_TARGET) --add-section .note.GNU-stack='$(call SHESCAPE,$(BUILDDIR))/empty' --set-section-flags .note.GNU-stack=noload,readonly build-id.s2.o
	$(OBJCOPY_FOR_TARGET) -O binary --only-section=.note.gnu.build-id '$(call SHESCAPE,$@)' '$(call SHESCAPE,$(BUILDDIR))/build-id.s3.bin'
	cd '$(call SHESCAPE,$(BUILDDIR))' && \
		$(OBJCOPY_FOR_TARGET) -I binary -B i386 -O elf32-i386 build-id.s3.bin build-id.s3.o && \
		$(OBJCOPY_FOR_TARGET) --add-section .note.GNU-stack='$(call SHESCAPE,$(BUILDDIR))/empty' --set-section-flags .note.GNU-stack=noload,readonly build-id.s3.o
	$(LD_FOR_TARGET) '$(call OBJESCAPE,$^)' '$(call SHESCAPE,$(BUILDDIR))/build-id.s2.o' '$(call SHESCAPE,$(BUILDDIR))/build-id.s3.o' $(LDFLAGS_FOR_TARGET) -T'$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld' -o '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/linker.ld: linker_bios.ld.in
	$(MKDIR_P) '$(call SHESCAPE,$(BUILDDIR))'
	$(CC_FOR_TARGET) -x c -E -P -undef linker_bios.ld.in -o '$(call SHESCAPE,$(BUILDDIR))/linker.ld'

$(call MKESCAPE,$(BUILDDIR))/limine.elf: $(OBJ) $(call MKESCAPE,$(BUILDDIR))/stage2.map.o $(call MKESCAPE,$(BUILDDIR))/full.map.o
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/empty'
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/linker.ld'
	$(LD_FOR_TARGET) '$(call OBJESCAPE,$^)' $(LDFLAGS_FOR_TARGET) -T'$(call SHESCAPE,$(BUILDDIR))/linker.ld' -o '$(call SHESCAPE,$@)'
	$(OBJCOPY_FOR_TARGET) -O binary --only-section=.note.gnu.build-id '$(call SHESCAPE,$@)' '$(call SHESCAPE,$(BUILDDIR))/build-id.s2.bin'
	cd '$(call SHESCAPE,$(BUILDDIR))' && \
		$(OBJCOPY_FOR_TARGET) -I binary -B i386 -O elf32-i386 build-id.s2.bin build-id.s2.o && \
		$(OBJCOPY_FOR_TARGET) --add-section .note.GNU-stack='$(call SHESCAPE,$(BUILDDIR))/empty' --set-section-flags .note.GNU-stack=noload,readonly build-id.s2.o
	$(OBJCOPY_FOR_TARGET) -O binary --only-section=.note.gnu.build-id '$(call SHESCAPE,$@)' '$(call SHESCAPE,$(BUILDDIR))/build-id.s3.bin'
	cd '$(call SHESCAPE,$(BUILDDIR))' && \
		$(OBJCOPY_FOR_TARGET) -I binary -B i386 -O elf32-i386 build-id.s3.bin build-id.s3.o && \
		$(OBJCOPY_FOR_TARGET) --add-section .note.GNU-stack='$(call SHESCAPE,$(BUILDDIR))/empty' --set-section-flags .note.GNU-stack=noload,readonly build-id.s3.o
	$(LD_FOR_TARGET) '$(call OBJESCAPE,$^)' '$(call SHESCAPE,$(BUILDDIR))/build-id.s2.o' '$(call SHESCAPE,$(BUILDDIR))/build-id.s3.o' $(LDFLAGS_FOR_TARGET) -T'$(call SHESCAPE,$(BUILDDIR))/linker.ld' -o '$(call SHESCAPE,$@)'

endif

ifeq ($(TARGET),uefi-x86-64)

$(call MKESCAPE,$(BUILDDIR))/full.map.o: $(call MKESCAPE,$(BUILDDIR))/limine_nomap.elf
	cd '$(call SHESCAPE,$(BUILDDIR))' && \
		'$(call SHESCAPE,$(SRCDIR))/gensyms.sh' '$(call SHESCAPE,$<)' full 64 '\.text'
	$(CC_FOR_TARGET) $(CFLAGS_FOR_TARGET) $(CPPFLAGS_FOR_TARGET) -c '$(call SHESCAPE,$(BUILDDIR))/full.map.S' -o '$(call SHESCAPE,$@)'
	rm -f '$(call SHESCAPE,$(BUILDDIR))/full.map.S' '$(call SHESCAPE,$(BUILDDIR))/full.map.d'

$(call MKESCAPE,$(BUILDDIR))/BOOTX64.EFI: $(call MKESCAPE,$(BUILDDIR))/limine.elf
	$(OBJCOPY_FOR_TARGET) -O binary '$(call SHESCAPE,$<)' '$(call SHESCAPE,$@)'
	chmod -x '$(call SHESCAPE,$@)'
	dd if=/dev/zero of='$(call SHESCAPE,$@)' bs=4096 count=0 seek=$$(( ($$(wc -c < '$(call SHESCAPE,$@)') + 4095) / 4096 ))

$(call MKESCAPE,$(BUILDDIR))/linker_nomap.ld: linker_uefi_x86_64.ld.in
	$(MKDIR_P) '$(call SHESCAPE,$(BUILDDIR))'
	$(CC_FOR_TARGET) -x c -E -P -undef -DLINKER_NOMAP linker_uefi_x86_64.ld.in -o '$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld'

$(call MKESCAPE,$(BUILDDIR))/limine_nomap.elf: $(OBJ)
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld'
	$(LD_FOR_TARGET) \
		-T'$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld' \
		'$(call OBJESCAPE,$^)' $(LDFLAGS_FOR_TARGET) -o '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/linker.ld: linker_uefi_x86_64.ld.in
	$(MKDIR_P) '$(call SHESCAPE,$(BUILDDIR))'
	$(CC_FOR_TARGET) -x c -E -P -undef linker_uefi_x86_64.ld.in -o '$(call SHESCAPE,$(BUILDDIR))/linker.ld'

$(call MKESCAPE,$(BUILDDIR))/limine.elf: $(OBJ) $(call MKESCAPE,$(BUILDDIR))/full.map.o
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/linker.ld'
	$(LD_FOR_TARGET) \
		-T'$(call SHESCAPE,$(BUILDDIR))/linker.ld' \
		'$(call OBJESCAPE,$^)' $(LDFLAGS_FOR_TARGET) -o '$(call SHESCAPE,$@)'

endif

ifeq ($(TARGET),uefi-aarch64)

$(call MKESCAPE,$(BUILDDIR))/full.map.o: $(call MKESCAPE,$(BUILDDIR))/limine_nomap.elf
	cd '$(call SHESCAPE,$(BUILDDIR))' && \
		'$(call SHESCAPE,$(SRCDIR))/gensyms.sh' '$(call SHESCAPE,$<)' full 64 '\.text'
	$(CC_FOR_TARGET) $(CFLAGS_FOR_TARGET) $(CPPFLAGS_FOR_TARGET) -c '$(call SHESCAPE,$(BUILDDIR))/full.map.S' -o '$(call SHESCAPE,$@)'
	rm -f '$(call SHESCAPE,$(BUILDDIR))/full.map.S' '$(call SHESCAPE,$(BUILDDIR))/full.map.d'

$(call MKESCAPE,$(BUILDDIR))/BOOTAA64.EFI: $(call MKESCAPE,$(BUILDDIR))/limine.elf
	$(OBJCOPY_FOR_TARGET) -O binary '$(call SHESCAPE,$<)' '$(call SHESCAPE,$@)'
	chmod -x '$(call SHESCAPE,$@)'
	dd if=/dev/zero of='$(call SHESCAPE,$@)' bs=4096 count=0 seek=$$(( ($$(wc -c < '$(call SHESCAPE,$@)') + 4095) / 4096 ))

$(call MKESCAPE,$(BUILDDIR))/linker_nomap.ld: linker_uefi_aarch64.ld.in
	$(MKDIR_P) '$(call SHESCAPE,$(BUILDDIR))'
	$(CC_FOR_TARGET) -x c -E -P -undef -DLINKER_NOMAP linker_uefi_aarch64.ld.in -o '$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld'

$(call MKESCAPE,$(BUILDDIR))/limine_nomap.elf: $(OBJ)
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld'
	$(LD_FOR_TARGET) \
		-T'$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld' \
		'$(call OBJESCAPE,$^)' $(LDFLAGS_FOR_TARGET) -o '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/linker.ld: linker_uefi_aarch64.ld.in
	$(MKDIR_P) '$(call SHESCAPE,$(BUILDDIR))'
	$(CC_FOR_TARGET) -x c -E -P -undef linker_uefi_aarch64.ld.in -o '$(call SHESCAPE,$(BUILDDIR))/linker.ld'

$(call MKESCAPE,$(BUILDDIR))/limine.elf: $(OBJ) $(call MKESCAPE,$(BUILDDIR))/full.map.o
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/linker.ld'
	$(LD_FOR_TARGET) \
		-T'$(call SHESCAPE,$(BUILDDIR))/linker.ld' \
		'$(call OBJESCAPE,$^)' $(LDFLAGS_FOR_TARGET) -o '$(call SHESCAPE,$@)'
endif

ifeq ($(TARGET),uefi-riscv64)

$(call MKESCAPE,$(BUILDDIR))/full.map.o: $(call MKESCAPE,$(BUILDDIR))/limine_nomap.elf
	cd '$(call SHESCAPE,$(BUILDDIR))' && \
		'$(call SHESCAPE,$(SRCDIR))/gensyms.sh' '$(call SHESCAPE,$<)' full 64 '\.text'
	$(CC_FOR_TARGET) $(CFLAGS_FOR_TARGET) $(CPPFLAGS_FOR_TARGET) -c '$(call SHESCAPE,$(BUILDDIR))/full.map.S' -o '$(call SHESCAPE,$@)'
	rm -f '$(call SHESCAPE,$(BUILDDIR))/full.map.S' '$(call SHESCAPE,$(BUILDDIR))/full.map.d'

$(call MKESCAPE,$(BUILDDIR))/BOOTRISCV64.EFI: $(call MKESCAPE,$(BUILDDIR))/limine.elf
	$(OBJCOPY_FOR_TARGET) -O binary '$(call SHESCAPE,$<)' '$(call SHESCAPE,$@)'
	chmod -x '$(call SHESCAPE,$@)'
	dd if=/dev/zero of='$(call SHESCAPE,$@)' bs=4096 count=0 seek=$$(( ($$(wc -c < '$(call SHESCAPE,$@)') + 4095) / 4096 ))

$(call MKESCAPE,$(BUILDDIR))/linker_nomap.ld: linker_uefi_riscv64.ld.in
	$(MKDIR_P) '$(call SHESCAPE,$(BUILDDIR))'
	$(CC_FOR_TARGET) -x c -E -P -undef -DLINKER_NOMAP linker_uefi_riscv64.ld.in -o '$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld'

$(call MKESCAPE,$(BUILDDIR))/limine_nomap.elf: $(OBJ)
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld'
	$(LD_FOR_TARGET) \
		-T'$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld' \
		'$(call OBJESCAPE,$^)' $(LDFLAGS_FOR_TARGET) -o '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/linker.ld: linker_uefi_riscv64.ld.in
	$(MKDIR_P) '$(call SHESCAPE,$(BUILDDIR))'
	$(CC_FOR_TARGET) -x c -E -P -undef linker_uefi_riscv64.ld.in -o '$(call SHESCAPE,$(BUILDDIR))/linker.ld'

$(call MKESCAPE,$(BUILDDIR))/limine.elf: $(OBJ) $(call MKESCAPE,$(BUILDDIR))/full.map.o
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/linker.ld'
	$(LD_FOR_TARGET) \
		-T'$(call SHESCAPE,$(BUILDDIR))/linker.ld' \
		'$(call OBJESCAPE,$^)' $(LDFLAGS_FOR_TARGET) -o '$(call SHESCAPE,$@)'
endif

ifeq ($(TARGET),uefi-loongarch64)

$(call MKESCAPE,$(BUILDDIR))/full.map.o: $(call MKESCAPE,$(BUILDDIR))/limine_nomap.elf
	cd '$(call SHESCAPE,$(BUILDDIR))' && \
		'$(call SHESCAPE,$(SRCDIR))/gensyms.sh' '$(call SHESCAPE,$<)' full 64 '\.text'
	$(CC_FOR_TARGET) $(CFLAGS_FOR_TARGET) $(CPPFLAGS_FOR_TARGET) -c '$(call SHESCAPE,$(BUILDDIR))/full.map.S' -o '$(call SHESCAPE,$@)'
	rm -f '$(call SHESCAPE,$(BUILDDIR))/full.map.S' '$(call SHESCAPE,$(BUILDDIR))/full.map.d'

$(call MKESCAPE,$(BUILDDIR))/BOOTLOONGARCH64.EFI: $(call MKESCAPE,$(BUILDDIR))/limine.elf
	$(OBJCOPY_FOR_TARGET) -O binary '$(call SHESCAPE,$<)' '$(call SHESCAPE,$@)'
	chmod -x '$(call SHESCAPE,$@)'
	dd if=/dev/zero of='$(call SHESCAPE,$@)' bs=4096 count=0 seek=$$(( ($$(wc -c < '$(call SHESCAPE,$@)') + 4095) / 4096 ))

$(call MKESCAPE,$(BUILDDIR))/linker_nomap.ld: linker_uefi_loongarch64.ld.in
	$(MKDIR_P) '$(call SHESCAPE,$(BUILDDIR))'
	$(CC_FOR_TARGET) -x c -E -P -undef -DLINKER_NOMAP linker_uefi_loongarch64.ld.in -o '$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld'

$(call MKESCAPE,$(BUILDDIR))/limine_nomap.elf: $(OBJ)
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld'
	$(LD_FOR_TARGET) \
		-T'$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld' \
		'$(call OBJESCAPE,$^)' $(LDFLAGS_FOR_TARGET) -o '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/linker.ld: linker_uefi_loongarch64.ld.in
	$(MKDIR_P) '$(call SHESCAPE,$(BUILDDIR))'
	$(CC_FOR_TARGET) -x c -E -P -undef linker_uefi_loongarch64.ld.in -o '$(call SHESCAPE,$(BUILDDIR))/linker.ld'

$(call MKESCAPE,$(BUILDDIR))/limine.elf: $(OBJ) $(call MKESCAPE,$(BUILDDIR))/full.map.o
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/linker.ld'
	$(LD_FOR_TARGET) \
		-T'$(call SHESCAPE,$(BUILDDIR))/linker.ld' \
		'$(call OBJESCAPE,$^)' $(LDFLAGS_FOR_TARGET) -o '$(call SHESCAPE,$@)'
endif

ifeq ($(TARGET),uefi-ia32)

$(call MKESCAPE,$(BUILDDIR))/full.map.o: $(call MKESCAPE,$(BUILDDIR))/limine_nomap.elf
	cd '$(call SHESCAPE,$(BUILDDIR))' && \
		'$(call SHESCAPE,$(SRCDIR))/gensyms.sh' '$(call SHESCAPE,$<)' full 32 '\.text'
	$(CC_FOR_TARGET) $(CFLAGS_FOR_TARGET) $(CPPFLAGS_FOR_TARGET) -c '$(call SHESCAPE,$(BUILDDIR))/full.map.S' -o '$(call SHESCAPE,$@)'
	rm -f '$(call SHESCAPE,$(BUILDDIR))/full.map.S' '$(call SHESCAPE,$(BUILDDIR))/full.map.d'

$(call MKESCAPE,$(BUILDDIR))/BOOTIA32.EFI: $(call MKESCAPE,$(BUILDDIR))/limine.elf
	$(OBJCOPY_FOR_TARGET) -O binary '$(call SHESCAPE,$<)' '$(call SHESCAPE,$@)'
	chmod -x '$(call SHESCAPE,$@)'
	dd if=/dev/zero of='$(call SHESCAPE,$@)' bs=4096 count=0 seek=$$(( ($$(wc -c < '$(call SHESCAPE,$@)') + 4095) / 4096 ))

$(call MKESCAPE,$(BUILDDIR))/linker_nomap.ld: linker_uefi_ia32.ld.in
	$(MKDIR_P) '$(call SHESCAPE,$(BUILDDIR))'
	$(CC_FOR_TARGET) -x c -E -P -undef -DLINKER_NOMAP linker_uefi_ia32.ld.in -o '$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld'

$(call MKESCAPE,$(BUILDDIR))/limine_nomap.elf: $(OBJ)
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld'
	$(LD_FOR_TARGET) \
		-T'$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld' \
		'$(call OBJESCAPE,$^)' $(LDFLAGS_FOR_TARGET) -o '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/linker.ld: linker_uefi_ia32.ld.in
	$(MKDIR_P) '$(call SHESCAPE,$(BUILDDIR))'
	$(CC_FOR_TARGET) -x c -E -P -undef linker_uefi_ia32.ld.in -o '$(call SHESCAPE,$(BUILDDIR))/linker.ld'

$(call MKESCAPE,$(BUILDDIR))/limine.elf: $(OBJ) $(call MKESCAPE,$(BUILDDIR))/full.map.o
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/linker.ld'
	$(LD_FOR_TARGET) \
		-T'$(call SHESCAPE,$(BUILDDIR))/linker.ld' \
		'$(call OBJESCAPE,$^)' $(LDFLAGS_FOR_TARGET) -o '$(call SHESCAPE,$@)'

endif

-include $(HEADER_DEPS)

$(call MKESCAPE,$(BUILDDIR))/%.o: ../%.c
	$(MKDIR_P) "$$(dirname '$(call SHESCAPE,$@)')"
	$(CC_FOR_TARGET) $(CFLAGS_FOR_TARGET) $(CPPFLAGS_FOR_TARGET) -c '$(call SHESCAPE,$<)' -o '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/%.o: ../%.S
	$(MKDIR_P) "$$(dirname '$(call SHESCAPE,$@)')"
	$(CC_FOR_TARGET) $(CFLAGS_FOR_TARGET) $(CPPFLAGS_FOR_TARGET) -c '$(call SHESCAPE,$<)' -o '$(call SHESCAPE,$@)'

ifeq ($(TARGET),bios)
$(call MKESCAPE,$(BUILDDIR))/%.s2.o: ../%.s2.c
	$(MKDIR_P) "$$(dirname '$(call SHESCAPE,$@)')"
	$(CC_FOR_TARGET) $(CFLAGS_FOR_TARGET) $(S2CFLAGS) $(CPPFLAGS_FOR_TARGET) -c '$(call SHESCAPE,$<)' -o '$(call SHESCAPE,$@)'
endif

ifeq ($(TARGET),bios)
$(call MKESCAPE,$(BUILDDIR))/%.o: ../%.asm_ia32
	$(MKDIR_P) "$$(dirname '$(call SHESCAPE,$@)')"
	nasm '$(call SHESCAPE,$<)' $(NASMFLAGS_FOR_TARGET) -o '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/%.o: ../%.asm_bios_ia32
	$(MKDIR_P) "$$(dirname '$(call SHESCAPE,$@)')"
	nasm '$(call SHESCAPE,$<)' $(NASMFLAGS_FOR_TARGET) -o '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/%.o: ../%.asm_x86
	$(MKDIR_P) "$$(dirname '$(call SHESCAPE,$@)')"
	nasm '$(call SHESCAPE,$<)' $(NASMFLAGS_FOR_TARGET) -o '$(call SHESCAPE,$@)'
endif

ifeq ($(TARGET),uefi-x86-64)
$(call MKESCAPE,$(BUILDDIR))/%.o: ../%.asm_x86_64
	$(MKDIR_P) "$$(dirname '$(call SHESCAPE,$@)')"
	nasm '$(call SHESCAPE,$<)' $(NASMFLAGS_FOR_TARGET) -o '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/%.o: ../%.asm_uefi_x86_64
	$(MKDIR_P) "$$(dirname '$(call SHESCAPE,$@)')"
	nasm '$(call SHESCAPE,$<)' $(NASMFLAGS_FOR_TARGET) -o '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/%.o: ../%.asm_x86
	$(MKDIR_P) "$$(dirname '$(call SHESCAPE,$@)')"
	nasm '$(call SHESCAPE,$<)' $(NASMFLAGS_FOR_TARGET) -o '$(call SHESCAPE,$@)'
endif

ifeq ($(TARGET),uefi-aarch64)
$(call MKESCAPE,$(BUILDDIR))/%.o: ../%.asm_aarch64
	$(MKDIR_P) "$$(dirname '$(call SHESCAPE,$@)')"
	$(CC_FOR_TARGET) $(CFLAGS_FOR_TARGET) $(CPPFLAGS_FOR_TARGET) -x assembler-with-cpp -c '$(call SHESCAPE,$<)' -o '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/%.o: ../%.asm_uefi_aarch64
	$(MKDIR_P) "$$(dirname '$(call SHESCAPE,$@)')"
	$(CC_FOR_TARGET) $(CFLAGS_FOR_TARGET) $(CPPFLAGS_FOR_TARGET) -x assembler-with-cpp -c '$(call SHESCAPE,$<)' -o '$(call SHESCAPE,$@)'
endif

ifeq ($(TARGET),uefi-riscv64)
$(call MKESCAPE,$(BUILDDIR))/%.o: ../%.asm_riscv64
	$(MKDIR_P) "$$(dirname '$(call SHESCAPE,$@)')"
	$(CC_FOR_TARGET) $(CFLAGS_FOR_TARGET) $(CPPFLAGS_FOR_TARGET) -x assembler-with-cpp -c '$(call SHESCAPE,$<)' -o '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/%.o: ../%.asm_uefi_riscv64
	$(MKDIR_P) "$$(dirname '$(call SHESCAPE,$@)')"
	$(CC_FOR_TARGET) $(CFLAGS_FOR_TARGET) $(CPPFLAGS_FOR_TARGET) -x assembler-with-cpp -c '$(call SHESCAPE,$<)' -o '$(call SHESCAPE,$@)'
endif

ifeq ($(TARGET),uefi-loongarch64)
$(call MKESCAPE,$(BUILDDIR))/%.o: ../%.asm_loongarch64
	$(MKDIR_P) "$$(dirname '$(call SHESCAPE,$@)')"
	$(CC_FOR_TARGET) $(CFLAGS_FOR_TARGET) $(CPPFLAGS_FOR_TARGET) -x assembler-with-cpp -c '$(call SHESCAPE,$<)' -o '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/%.o: ../%.asm_uefi_loongarch64
	$(MKDIR_P) "$$(dirname '$(call SHESCAPE,$@)')"
	$(CC_FOR_TARGET) $(CFLAGS_FOR_TARGET) $(CPPFLAGS_FOR_TARGET) -x assembler-with-cpp -c '$(call SHESCAPE,$<)' -o '$(call SHESCAPE,$@)'
endif

ifeq ($(TARGET),uefi-ia32)
$(call MKESCAPE,$(BUILDDIR))/%.o: ../%.asm_ia32
	$(MKDIR_P) "$$(dirname '$(call SHESCAPE,$@)')"
	nasm '$(call SHESCAPE,$<)' $(NASMFLAGS_FOR_TARGET) -o '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/%.o: ../%.asm_uefi_ia32
	$(MKDIR_P) "$$(dirname '$(call SHESCAPE,$@)')"
	nasm '$(call SHESCAPE,$<)' $(NASMFLAGS_FOR_TARGET) -o '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/%.o: ../%.asm_x86
	$(MKDIR_P) "$$(dirname '$(call SHESCAPE,$@)')"
	nasm '$(call SHESCAPE,$<)' $(NASMFLAGS_FOR_TARGET) -o '$(call SHESCAPE,$@)'
endif
