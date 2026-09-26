.SUFFIXES:
.DELETE_ON_ERROR:

override SRCDIR := $(shell pwd -P)

override SPACE := $(subst ,, )
override TAB := $(subst ,,	)

override MKESCAPE = $(subst $(TAB),\$(TAB),$(subst $(SPACE),\ ,$(1)))
override SHESCAPE = $(subst ','\'',$(1))
override OBJESCAPE = $(subst .a ,.a' ',$(subst .o ,.o' ',$(call SHESCAPE,$(1))))

COM_OUTPUT := false
E9_OUTPUT := false

override S2CFLAGS := -Os

override CPPFLAGS_FOR_TARGET := \
    -nostdinc \
    -I . \
    -I libc-compat \
    -I ../limine-protocol/include \
    -I ../flanterm/src \
    -I ../libfdt/src \
    -I '$(call SHESCAPE,$(BUILDDIR))/..' \
    -isystem ../freestanding-c-hdrs/include \
    $(CPPFLAGS_FOR_TARGET) \
    -DCOM_OUTPUT=$(COM_OUTPUT) \
    -DE9_OUTPUT=$(E9_OUTPUT) \
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

override TARGET_CFLAGS :=
override TARGET_LDFLAGS :=

ifeq ($(TARGET),bios)
    override TARGET_CFLAGS := $(BIOS_CFLAGS)
    override TARGET_LDFLAGS := $(BIOS_LDFLAGS)
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
    override TARGET_CFLAGS := $(UEFI_X86_64_CFLAGS)
    override TARGET_LDFLAGS := $(UEFI_X86_64_LDFLAGS)
    override CPPFLAGS_FOR_TARGET := \
        -I ../picoefi/inc \
        $(CPPFLAGS_FOR_TARGET) \
        -DUEFI
    override NASMFLAGS_FOR_TARGET := \
        -f elf64 \
        $(NASMFLAGS_FOR_TARGET) \
        -DX86_64_TARGET \
        -DUEFI
endif

ifeq ($(TARGET),uefi-ia32)
    override TARGET_CFLAGS := $(UEFI_IA32_CFLAGS)
    override TARGET_LDFLAGS := $(UEFI_IA32_LDFLAGS)
    override CPPFLAGS_FOR_TARGET := \
        -I ../picoefi/inc \
        $(CPPFLAGS_FOR_TARGET) \
        -DUEFI
    override NASMFLAGS_FOR_TARGET := \
        -f elf32 \
        $(NASMFLAGS_FOR_TARGET) \
        -DIA32_TARGET \
        -DUEFI
endif

ifeq ($(TARGET),uefi-aarch64)
    override TARGET_CFLAGS := $(UEFI_AARCH64_CFLAGS)
    override TARGET_LDFLAGS := $(UEFI_AARCH64_LDFLAGS)
    override CPPFLAGS_FOR_TARGET := \
        -I ../picoefi/inc \
        $(CPPFLAGS_FOR_TARGET) \
        -DUEFI
endif

ifeq ($(TARGET),uefi-riscv64)
    override TARGET_CFLAGS := $(UEFI_RISCV64_CFLAGS)
    override TARGET_LDFLAGS := $(UEFI_RISCV64_LDFLAGS)
    override CPPFLAGS_FOR_TARGET := \
        -I ../picoefi/inc \
        $(CPPFLAGS_FOR_TARGET) \
        -DUEFI
endif

ifeq ($(TARGET),uefi-loongarch64)
    override TARGET_CFLAGS := $(UEFI_LOONGARCH64_CFLAGS)
    override TARGET_LDFLAGS := $(UEFI_LOONGARCH64_LDFLAGS)
    override CPPFLAGS_FOR_TARGET := \
        -I ../picoefi/inc \
        $(CPPFLAGS_FOR_TARGET) \
        -DUEFI
endif

ifeq ($(TARGET_CFLAGS),)
    $(error Configure with --enable-$(TARGET) to build this port)
endif

# The flags from configure already take in CFLAGS_FOR_TARGET and
# LDFLAGS_FOR_TARGET, after the Clang target.
override CFLAGS_FOR_TARGET := $(TARGET_CFLAGS)
override LDFLAGS_FOR_TARGET := $(TARGET_LDFLAGS)

ifeq ($(TARGET),bios)
    override C_FILES := $(shell cd .. && find common flanterm/src libfdt/src -type f -name '*.c' | LC_ALL=C sort)
    override S_FILES := $(shell cd .. && find common -type f -name '*.S' | LC_ALL=C sort)

    override ASMX86_FILES := $(shell cd .. && find common -type f -name '*.asm_x86' | LC_ALL=C sort)
    override ASM32_FILES := $(shell cd .. && find common -type f -name '*.asm_ia32' | LC_ALL=C sort)
    override ASMB_FILES := $(shell cd .. && find common -type f -name '*.asm_bios_ia32' | LC_ALL=C sort)

    override OBJ_REL := $(C_FILES:.c=.o) $(S_FILES:.S=.o) $(ASM32_FILES:.asm_ia32=.o) $(ASMB_FILES:.asm_bios_ia32=.o) $(ASMX86_FILES:.asm_x86=.o)
endif
ifeq ($(TARGET),uefi-x86-64)
    override C_FILES := $(shell cd .. && find common picoefi/x86_64 flanterm/src libfdt/src -type f -name '*.c' | LC_ALL=C sort)
    override S_FILES := $(shell cd .. && find common picoefi/x86_64 -type f -name '*.S' | LC_ALL=C sort)

    override ASMX86_FILES := $(shell cd .. && find common -type f -name '*.asm_x86' | LC_ALL=C sort)
    override ASM64_FILES := $(shell cd .. && find common -type f -name '*.asm_x86_64' | LC_ALL=C sort)
    override ASM64U_FILES := $(shell cd .. && find common -type f -name '*.asm_uefi_x86_64' | LC_ALL=C sort)

    override OBJ_REL := $(C_FILES:.c=.o) $(S_FILES:.S=.o) $(ASM64_FILES:.asm_x86_64=.o) $(ASM64U_FILES:.asm_uefi_x86_64=.o) $(ASMX86_FILES:.asm_x86=.o)
endif
ifeq ($(TARGET),uefi-ia32)
    override C_FILES := $(shell cd .. && find common picoefi/ia32 flanterm/src libfdt/src -type f -name '*.c' | LC_ALL=C sort)
    override S_FILES := $(shell cd .. && find common picoefi/ia32 -type f -name '*.S' | LC_ALL=C sort)

    override ASMX86_FILES := $(shell cd .. && find common -type f -name '*.asm_x86' | LC_ALL=C sort)
    override ASM32_FILES := $(shell cd .. && find common -type f -name '*.asm_ia32' | LC_ALL=C sort)
    override ASM32U_FILES := $(shell cd .. && find common -type f -name '*.asm_uefi_ia32' | LC_ALL=C sort)

    override OBJ_REL := $(C_FILES:.c=.o) $(S_FILES:.S=.o) $(ASM32_FILES:.asm_ia32=.o) $(ASM32U_FILES:.asm_uefi_ia32=.o) $(ASMX86_FILES:.asm_x86=.o)
endif
ifeq ($(TARGET),uefi-aarch64)
    override C_FILES := $(shell cd .. && find common picoefi/aarch64 flanterm/src libfdt/src -type f -name '*.c' | LC_ALL=C sort)
    override S_FILES := $(shell cd .. && find common picoefi/aarch64 -type f -name '*.S' | LC_ALL=C sort)

    override ASM64_FILES := $(shell cd .. && find common -type f -name '*.asm_aarch64' | LC_ALL=C sort)
    override ASM64U_FILES := $(shell cd .. && find common -type f -name '*.asm_uefi_aarch64' | LC_ALL=C sort)

    override OBJ_REL := $(C_FILES:.c=.o) $(S_FILES:.S=.o) $(ASM64_FILES:.asm_aarch64=.o) $(ASM64U_FILES:.asm_uefi_aarch64=.o)
endif
ifeq ($(TARGET),uefi-riscv64)
    override C_FILES := $(shell cd .. && find common picoefi/riscv64 flanterm/src libfdt/src -type f -name '*.c' | LC_ALL=C sort)
    override S_FILES := $(shell cd .. && find common picoefi/riscv64 -type f -name '*.S' | LC_ALL=C sort)

    override ASM64_FILES := $(shell cd .. && find common -type f -name '*.asm_riscv64' | LC_ALL=C sort)
    override ASM64U_FILES := $(shell cd .. && find common -type f -name '*.asm_uefi_riscv64' | LC_ALL=C sort)

    override OBJ_REL := $(C_FILES:.c=.o) $(S_FILES:.S=.o) $(ASM64_FILES:.asm_riscv64=.o) $(ASM64U_FILES:.asm_uefi_riscv64=.o)
endif
ifeq ($(TARGET),uefi-loongarch64)
    override C_FILES := $(shell cd .. && find common picoefi/loongarch64 flanterm/src libfdt/src -type f -name '*.c' | LC_ALL=C sort)
    override S_FILES := $(shell cd .. && find common picoefi/loongarch64 -type f -name '*.S' | LC_ALL=C sort)

    override ASM64_FILES := $(shell cd .. && find common -type f -name '*.asm_loongarch64' | LC_ALL=C sort)
    override ASM64U_FILES := $(shell cd .. && find common -type f -name '*.asm_uefi_loongarch64' | LC_ALL=C sort)

    override OBJ_REL := $(C_FILES:.c=.o) $(S_FILES:.S=.o) $(ASM64_FILES:.asm_loongarch64=.o) $(ASM64U_FILES:.asm_uefi_loongarch64=.o)
endif

override OBJ := $(addprefix $(call MKESCAPE,$(BUILDDIR))/, $(OBJ_REL))
override OBJ_S2 := $(addprefix $(call MKESCAPE,$(BUILDDIR))/, $(filter %.s2.o,$(OBJ_REL)))
override HEADER_DEPS := $(addprefix $(call MKESCAPE,$(BUILDDIR))/, $(OBJ_REL:.o=.d))

.PHONY: all

ifeq ($(TARGET),bios)
all: $(call MKESCAPE,$(BUILDDIR))/limine-bios.sys $(call MKESCAPE,$(BUILDDIR))/stage2.bin.limlz
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

$(call MKESCAPE,$(BUILDDIR))/stage2.bin.limlz: $(call MKESCAPE,$(BUILDDIR))/stage2.bin $(call MKESCAPE,$(LIMLZPACK))
	'$(call SHESCAPE,$(LIMLZPACK))' '$(call SHESCAPE,$<)' '$(call SHESCAPE,$@)'

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
	chmod a-x '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/linker_stage2only.ld: linker_bios.ld.in
	$(MKDIR_P) '$(call SHESCAPE,$(BUILDDIR))'
	$(CC_FOR_TARGET) -x c -E -P -undef -DLINKER_STAGE2ONLY linker_bios.ld.in -o '$(call SHESCAPE,$(BUILDDIR))/linker_stage2only.ld'

$(call MKESCAPE,$(BUILDDIR))/limine_stage2only.elf: $(OBJ_S2)
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/linker_stage2only.ld'
	$(LD_FOR_TARGET) $(LDFLAGS_FOR_TARGET) '$(call OBJESCAPE,$^)' -T'$(call SHESCAPE,$(BUILDDIR))/linker_stage2only.ld' -o '$(call SHESCAPE,$@)' || \
		( echo "This error may mean that stage 2 was trying to use stage 3 symbols before loading stage 3" && \
		  false )

$(call MKESCAPE,$(BUILDDIR))/linker_nos2map.ld: linker_bios.ld.in
	$(MKDIR_P) '$(call SHESCAPE,$(BUILDDIR))'
	$(CC_FOR_TARGET) -x c -E -P -undef -DLINKER_NOMAP -DLINKER_NOS2MAP linker_bios.ld.in -o '$(call SHESCAPE,$(BUILDDIR))/linker_nos2map.ld'

$(call MKESCAPE,$(BUILDDIR))/limine_nomap.elf: $(OBJ)
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/linker_nos2map.ld'
	$(LD_FOR_TARGET) $(LDFLAGS_FOR_TARGET) '$(call OBJESCAPE,$^)' -T'$(call SHESCAPE,$(BUILDDIR))/linker_nos2map.ld' -o '$(call SHESCAPE,$@)'
	$(OBJCOPY_FOR_TARGET) -O binary --only-section=.note.gnu.build-id '$(call SHESCAPE,$@)' '$(call SHESCAPE,$(BUILDDIR))/build-id.s2.bin'
	cd '$(call SHESCAPE,$(BUILDDIR))' && \
		$(OBJCOPY_FOR_TARGET) -I binary -B i386 -O elf32-i386 build-id.s2.bin build-id.s2.o
	$(OBJCOPY_FOR_TARGET) -O binary --only-section=.note.gnu.build-id '$(call SHESCAPE,$@)' '$(call SHESCAPE,$(BUILDDIR))/build-id.s3.bin'
	cd '$(call SHESCAPE,$(BUILDDIR))' && \
		$(OBJCOPY_FOR_TARGET) -I binary -B i386 -O elf32-i386 build-id.s3.bin build-id.s3.o
	$(LD_FOR_TARGET) $(LDFLAGS_FOR_TARGET) '$(call OBJESCAPE,$^)' '$(call SHESCAPE,$(BUILDDIR))/build-id.s2.o' '$(call SHESCAPE,$(BUILDDIR))/build-id.s3.o' -T'$(call SHESCAPE,$(BUILDDIR))/linker_nos2map.ld' -o '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/linker_nomap.ld: linker_bios.ld.in
	$(MKDIR_P) '$(call SHESCAPE,$(BUILDDIR))'
	$(CC_FOR_TARGET) -x c -E -P -undef -DLINKER_NOMAP linker_bios.ld.in -o '$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld'

$(call MKESCAPE,$(BUILDDIR))/limine_nos3map.elf: $(OBJ) $(call MKESCAPE,$(BUILDDIR))/stage2.map.o
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld'
	$(LD_FOR_TARGET) $(LDFLAGS_FOR_TARGET) '$(call OBJESCAPE,$^)' -T'$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld' -o '$(call SHESCAPE,$@)'
	$(OBJCOPY_FOR_TARGET) -O binary --only-section=.note.gnu.build-id '$(call SHESCAPE,$@)' '$(call SHESCAPE,$(BUILDDIR))/build-id.s2.bin'
	cd '$(call SHESCAPE,$(BUILDDIR))' && \
		$(OBJCOPY_FOR_TARGET) -I binary -B i386 -O elf32-i386 build-id.s2.bin build-id.s2.o
	$(OBJCOPY_FOR_TARGET) -O binary --only-section=.note.gnu.build-id '$(call SHESCAPE,$@)' '$(call SHESCAPE,$(BUILDDIR))/build-id.s3.bin'
	cd '$(call SHESCAPE,$(BUILDDIR))' && \
		$(OBJCOPY_FOR_TARGET) -I binary -B i386 -O elf32-i386 build-id.s3.bin build-id.s3.o
	$(LD_FOR_TARGET) $(LDFLAGS_FOR_TARGET) '$(call OBJESCAPE,$^)' '$(call SHESCAPE,$(BUILDDIR))/build-id.s2.o' '$(call SHESCAPE,$(BUILDDIR))/build-id.s3.o' -T'$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld' -o '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/linker.ld: linker_bios.ld.in
	$(MKDIR_P) '$(call SHESCAPE,$(BUILDDIR))'
	$(CC_FOR_TARGET) -x c -E -P -undef linker_bios.ld.in -o '$(call SHESCAPE,$(BUILDDIR))/linker.ld'

$(call MKESCAPE,$(BUILDDIR))/limine.elf: $(OBJ) $(call MKESCAPE,$(BUILDDIR))/stage2.map.o $(call MKESCAPE,$(BUILDDIR))/full.map.o
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/linker.ld'
	$(LD_FOR_TARGET) $(LDFLAGS_FOR_TARGET) '$(call OBJESCAPE,$^)' -T'$(call SHESCAPE,$(BUILDDIR))/linker.ld' -o '$(call SHESCAPE,$@)'
	$(OBJCOPY_FOR_TARGET) -O binary --only-section=.note.gnu.build-id '$(call SHESCAPE,$@)' '$(call SHESCAPE,$(BUILDDIR))/build-id.s2.bin'
	cd '$(call SHESCAPE,$(BUILDDIR))' && \
		$(OBJCOPY_FOR_TARGET) -I binary -B i386 -O elf32-i386 build-id.s2.bin build-id.s2.o
	$(OBJCOPY_FOR_TARGET) -O binary --only-section=.note.gnu.build-id '$(call SHESCAPE,$@)' '$(call SHESCAPE,$(BUILDDIR))/build-id.s3.bin'
	cd '$(call SHESCAPE,$(BUILDDIR))' && \
		$(OBJCOPY_FOR_TARGET) -I binary -B i386 -O elf32-i386 build-id.s3.bin build-id.s3.o
	$(LD_FOR_TARGET) $(LDFLAGS_FOR_TARGET) '$(call OBJESCAPE,$^)' '$(call SHESCAPE,$(BUILDDIR))/build-id.s2.o' '$(call SHESCAPE,$(BUILDDIR))/build-id.s3.o' -T'$(call SHESCAPE,$(BUILDDIR))/linker.ld' -o '$(call SHESCAPE,$@)'

endif

ifeq ($(TARGET),uefi-x86-64)

$(call MKESCAPE,$(BUILDDIR))/full.map.o: $(call MKESCAPE,$(BUILDDIR))/limine_nomap.elf
	cd '$(call SHESCAPE,$(BUILDDIR))' && \
		'$(call SHESCAPE,$(SRCDIR))/gensyms.sh' '$(call SHESCAPE,$<)' full 64 '\.text'
	$(CC_FOR_TARGET) $(CFLAGS_FOR_TARGET) $(CPPFLAGS_FOR_TARGET) -c '$(call SHESCAPE,$(BUILDDIR))/full.map.S' -o '$(call SHESCAPE,$@)'
	rm -f '$(call SHESCAPE,$(BUILDDIR))/full.map.S' '$(call SHESCAPE,$(BUILDDIR))/full.map.d'

$(call MKESCAPE,$(BUILDDIR))/BOOTX64.EFI: $(call MKESCAPE,$(BUILDDIR))/limine.elf
	$(OBJCOPY_FOR_TARGET) -O binary '$(call SHESCAPE,$<)' '$(call SHESCAPE,$@)'
	chmod a-x '$(call SHESCAPE,$@)'
	dd if=/dev/null of='$(call SHESCAPE,$@)' bs=4096 seek=$$(( ($$(wc -c < '$(call SHESCAPE,$@)') + 4095) / 4096 )) 2>/dev/null

$(call MKESCAPE,$(BUILDDIR))/linker_nomap.ld: linker_uefi_x86_64.ld.in
	$(MKDIR_P) '$(call SHESCAPE,$(BUILDDIR))'
	$(CC_FOR_TARGET) -x c -E -P -undef -DLINKER_NOMAP linker_uefi_x86_64.ld.in -o '$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld'

$(call MKESCAPE,$(BUILDDIR))/limine_nomap.elf: $(OBJ)
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld'
	$(LD_FOR_TARGET) \
		-T'$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld' \
		$(LDFLAGS_FOR_TARGET) '$(call OBJESCAPE,$^)' -o '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/linker.ld: linker_uefi_x86_64.ld.in
	$(MKDIR_P) '$(call SHESCAPE,$(BUILDDIR))'
	$(CC_FOR_TARGET) -x c -E -P -undef linker_uefi_x86_64.ld.in -o '$(call SHESCAPE,$(BUILDDIR))/linker.ld'

$(call MKESCAPE,$(BUILDDIR))/limine.elf: $(OBJ) $(call MKESCAPE,$(BUILDDIR))/full.map.o
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/linker.ld'
	$(LD_FOR_TARGET) \
		-T'$(call SHESCAPE,$(BUILDDIR))/linker.ld' \
		$(LDFLAGS_FOR_TARGET) '$(call OBJESCAPE,$^)' -o '$(call SHESCAPE,$@)'

endif

ifeq ($(TARGET),uefi-aarch64)

$(call MKESCAPE,$(BUILDDIR))/full.map.o: $(call MKESCAPE,$(BUILDDIR))/limine_nomap.elf
	cd '$(call SHESCAPE,$(BUILDDIR))' && \
		'$(call SHESCAPE,$(SRCDIR))/gensyms.sh' '$(call SHESCAPE,$<)' full 64 '\.text'
	$(CC_FOR_TARGET) $(CFLAGS_FOR_TARGET) $(CPPFLAGS_FOR_TARGET) -c '$(call SHESCAPE,$(BUILDDIR))/full.map.S' -o '$(call SHESCAPE,$@)'
	rm -f '$(call SHESCAPE,$(BUILDDIR))/full.map.S' '$(call SHESCAPE,$(BUILDDIR))/full.map.d'

$(call MKESCAPE,$(BUILDDIR))/BOOTAA64.EFI: $(call MKESCAPE,$(BUILDDIR))/limine.elf
	$(OBJCOPY_FOR_TARGET) -O binary '$(call SHESCAPE,$<)' '$(call SHESCAPE,$@)'
	chmod a-x '$(call SHESCAPE,$@)'
	dd if=/dev/null of='$(call SHESCAPE,$@)' bs=4096 seek=$$(( ($$(wc -c < '$(call SHESCAPE,$@)') + 4095) / 4096 )) 2>/dev/null

$(call MKESCAPE,$(BUILDDIR))/linker_nomap.ld: linker_uefi_aarch64.ld.in
	$(MKDIR_P) '$(call SHESCAPE,$(BUILDDIR))'
	$(CC_FOR_TARGET) -x c -E -P -undef -DLINKER_NOMAP linker_uefi_aarch64.ld.in -o '$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld'

$(call MKESCAPE,$(BUILDDIR))/limine_nomap.elf: $(OBJ)
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld'
	$(LD_FOR_TARGET) \
		-T'$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld' \
		$(LDFLAGS_FOR_TARGET) '$(call OBJESCAPE,$^)' -o '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/linker.ld: linker_uefi_aarch64.ld.in
	$(MKDIR_P) '$(call SHESCAPE,$(BUILDDIR))'
	$(CC_FOR_TARGET) -x c -E -P -undef linker_uefi_aarch64.ld.in -o '$(call SHESCAPE,$(BUILDDIR))/linker.ld'

$(call MKESCAPE,$(BUILDDIR))/limine.elf: $(OBJ) $(call MKESCAPE,$(BUILDDIR))/full.map.o
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/linker.ld'
	$(LD_FOR_TARGET) \
		-T'$(call SHESCAPE,$(BUILDDIR))/linker.ld' \
		$(LDFLAGS_FOR_TARGET) '$(call OBJESCAPE,$^)' -o '$(call SHESCAPE,$@)'
endif

ifeq ($(TARGET),uefi-riscv64)

$(call MKESCAPE,$(BUILDDIR))/full.map.o: $(call MKESCAPE,$(BUILDDIR))/limine_nomap.elf
	cd '$(call SHESCAPE,$(BUILDDIR))' && \
		'$(call SHESCAPE,$(SRCDIR))/gensyms.sh' '$(call SHESCAPE,$<)' full 64 '\.text'
	$(CC_FOR_TARGET) $(CFLAGS_FOR_TARGET) $(CPPFLAGS_FOR_TARGET) -c '$(call SHESCAPE,$(BUILDDIR))/full.map.S' -o '$(call SHESCAPE,$@)'
	rm -f '$(call SHESCAPE,$(BUILDDIR))/full.map.S' '$(call SHESCAPE,$(BUILDDIR))/full.map.d'

$(call MKESCAPE,$(BUILDDIR))/BOOTRISCV64.EFI: $(call MKESCAPE,$(BUILDDIR))/limine.elf
	$(OBJCOPY_FOR_TARGET) -O binary '$(call SHESCAPE,$<)' '$(call SHESCAPE,$@)'
	chmod a-x '$(call SHESCAPE,$@)'
	dd if=/dev/null of='$(call SHESCAPE,$@)' bs=4096 seek=$$(( ($$(wc -c < '$(call SHESCAPE,$@)') + 4095) / 4096 )) 2>/dev/null

$(call MKESCAPE,$(BUILDDIR))/linker_nomap.ld: linker_uefi_riscv64.ld.in
	$(MKDIR_P) '$(call SHESCAPE,$(BUILDDIR))'
	$(CC_FOR_TARGET) -x c -E -P -undef -DLINKER_NOMAP linker_uefi_riscv64.ld.in -o '$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld'

$(call MKESCAPE,$(BUILDDIR))/limine_nomap.elf: $(OBJ)
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld'
	$(LD_FOR_TARGET) \
		-T'$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld' \
		$(LDFLAGS_FOR_TARGET) '$(call OBJESCAPE,$^)' -o '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/linker.ld: linker_uefi_riscv64.ld.in
	$(MKDIR_P) '$(call SHESCAPE,$(BUILDDIR))'
	$(CC_FOR_TARGET) -x c -E -P -undef linker_uefi_riscv64.ld.in -o '$(call SHESCAPE,$(BUILDDIR))/linker.ld'

$(call MKESCAPE,$(BUILDDIR))/limine.elf: $(OBJ) $(call MKESCAPE,$(BUILDDIR))/full.map.o
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/linker.ld'
	$(LD_FOR_TARGET) \
		-T'$(call SHESCAPE,$(BUILDDIR))/linker.ld' \
		$(LDFLAGS_FOR_TARGET) '$(call OBJESCAPE,$^)' -o '$(call SHESCAPE,$@)'
endif

ifeq ($(TARGET),uefi-loongarch64)

$(call MKESCAPE,$(BUILDDIR))/full.map.o: $(call MKESCAPE,$(BUILDDIR))/limine_nomap.elf
	cd '$(call SHESCAPE,$(BUILDDIR))' && \
		'$(call SHESCAPE,$(SRCDIR))/gensyms.sh' '$(call SHESCAPE,$<)' full 64 '\.text'
	$(CC_FOR_TARGET) $(CFLAGS_FOR_TARGET) $(CPPFLAGS_FOR_TARGET) -c '$(call SHESCAPE,$(BUILDDIR))/full.map.S' -o '$(call SHESCAPE,$@)'
	rm -f '$(call SHESCAPE,$(BUILDDIR))/full.map.S' '$(call SHESCAPE,$(BUILDDIR))/full.map.d'

$(call MKESCAPE,$(BUILDDIR))/BOOTLOONGARCH64.EFI: $(call MKESCAPE,$(BUILDDIR))/limine.elf
	$(OBJCOPY_FOR_TARGET) -O binary '$(call SHESCAPE,$<)' '$(call SHESCAPE,$@)'
	chmod a-x '$(call SHESCAPE,$@)'
	dd if=/dev/null of='$(call SHESCAPE,$@)' bs=4096 seek=$$(( ($$(wc -c < '$(call SHESCAPE,$@)') + 4095) / 4096 )) 2>/dev/null

$(call MKESCAPE,$(BUILDDIR))/linker_nomap.ld: linker_uefi_loongarch64.ld.in
	$(MKDIR_P) '$(call SHESCAPE,$(BUILDDIR))'
	$(CC_FOR_TARGET) -x c -E -P -undef -DLINKER_NOMAP linker_uefi_loongarch64.ld.in -o '$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld'

$(call MKESCAPE,$(BUILDDIR))/limine_nomap.elf: $(OBJ)
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld'
	$(LD_FOR_TARGET) \
		-T'$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld' \
		$(LDFLAGS_FOR_TARGET) '$(call OBJESCAPE,$^)' -o '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/linker.ld: linker_uefi_loongarch64.ld.in
	$(MKDIR_P) '$(call SHESCAPE,$(BUILDDIR))'
	$(CC_FOR_TARGET) -x c -E -P -undef linker_uefi_loongarch64.ld.in -o '$(call SHESCAPE,$(BUILDDIR))/linker.ld'

$(call MKESCAPE,$(BUILDDIR))/limine.elf: $(OBJ) $(call MKESCAPE,$(BUILDDIR))/full.map.o
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/linker.ld'
	$(LD_FOR_TARGET) \
		-T'$(call SHESCAPE,$(BUILDDIR))/linker.ld' \
		$(LDFLAGS_FOR_TARGET) '$(call OBJESCAPE,$^)' -o '$(call SHESCAPE,$@)'
endif

ifeq ($(TARGET),uefi-ia32)

$(call MKESCAPE,$(BUILDDIR))/full.map.o: $(call MKESCAPE,$(BUILDDIR))/limine_nomap.elf
	cd '$(call SHESCAPE,$(BUILDDIR))' && \
		'$(call SHESCAPE,$(SRCDIR))/gensyms.sh' '$(call SHESCAPE,$<)' full 32 '\.text'
	$(CC_FOR_TARGET) $(CFLAGS_FOR_TARGET) $(CPPFLAGS_FOR_TARGET) -c '$(call SHESCAPE,$(BUILDDIR))/full.map.S' -o '$(call SHESCAPE,$@)'
	rm -f '$(call SHESCAPE,$(BUILDDIR))/full.map.S' '$(call SHESCAPE,$(BUILDDIR))/full.map.d'

$(call MKESCAPE,$(BUILDDIR))/BOOTIA32.EFI: $(call MKESCAPE,$(BUILDDIR))/limine.elf
	$(OBJCOPY_FOR_TARGET) -O binary '$(call SHESCAPE,$<)' '$(call SHESCAPE,$@)'
	chmod a-x '$(call SHESCAPE,$@)'
	dd if=/dev/null of='$(call SHESCAPE,$@)' bs=4096 seek=$$(( ($$(wc -c < '$(call SHESCAPE,$@)') + 4095) / 4096 )) 2>/dev/null

$(call MKESCAPE,$(BUILDDIR))/linker_nomap.ld: linker_uefi_ia32.ld.in
	$(MKDIR_P) '$(call SHESCAPE,$(BUILDDIR))'
	$(CC_FOR_TARGET) -x c -E -P -undef -DLINKER_NOMAP linker_uefi_ia32.ld.in -o '$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld'

$(call MKESCAPE,$(BUILDDIR))/limine_nomap.elf: $(OBJ)
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld'
	$(LD_FOR_TARGET) \
		-T'$(call SHESCAPE,$(BUILDDIR))/linker_nomap.ld' \
		$(LDFLAGS_FOR_TARGET) '$(call OBJESCAPE,$^)' -o '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/linker.ld: linker_uefi_ia32.ld.in
	$(MKDIR_P) '$(call SHESCAPE,$(BUILDDIR))'
	$(CC_FOR_TARGET) -x c -E -P -undef linker_uefi_ia32.ld.in -o '$(call SHESCAPE,$(BUILDDIR))/linker.ld'

$(call MKESCAPE,$(BUILDDIR))/limine.elf: $(OBJ) $(call MKESCAPE,$(BUILDDIR))/full.map.o
	$(MAKE) -f common.mk '$(call SHESCAPE,$(BUILDDIR))/linker.ld'
	$(LD_FOR_TARGET) \
		-T'$(call SHESCAPE,$(BUILDDIR))/linker.ld' \
		$(LDFLAGS_FOR_TARGET) '$(call OBJESCAPE,$^)' -o '$(call SHESCAPE,$@)'

endif

-include $(HEADER_DEPS)

# Must precede the generic .c rule: make before 3.82 picks the first matching
# pattern rule rather than the shortest stem, and both match a .s2.o target.
ifeq ($(TARGET),bios)
$(call MKESCAPE,$(BUILDDIR))/%.s2.o: ../%.s2.c
	$(MKDIR_P) "$$(dirname '$(call SHESCAPE,$@)')"
	$(CC_FOR_TARGET) $(CFLAGS_FOR_TARGET) $(S2CFLAGS) $(CPPFLAGS_FOR_TARGET) -c '$(call SHESCAPE,$<)' -o '$(call SHESCAPE,$@)'
endif

$(call MKESCAPE,$(BUILDDIR))/%.o: ../%.c
	$(MKDIR_P) "$$(dirname '$(call SHESCAPE,$@)')"
	$(CC_FOR_TARGET) $(CFLAGS_FOR_TARGET) $(CPPFLAGS_FOR_TARGET) -c '$(call SHESCAPE,$<)' -o '$(call SHESCAPE,$@)'

$(call MKESCAPE,$(BUILDDIR))/%.o: ../%.S
	$(MKDIR_P) "$$(dirname '$(call SHESCAPE,$@)')"
	$(CC_FOR_TARGET) $(CFLAGS_FOR_TARGET) $(CPPFLAGS_FOR_TARGET) -c '$(call SHESCAPE,$<)' -o '$(call SHESCAPE,$@)'

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
