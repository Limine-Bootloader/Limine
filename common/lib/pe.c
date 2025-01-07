#include <stdint.h>
#include <stddef.h>
#include <lib/misc.h>
#include <lib/libc.h>
#include <lib/pe.h>
#include <lib/print.h>
#include <lib/rand.h>
#include <mm/pmm.h>

#define FIXED_HIGHER_HALF_OFFSET_64 ((uint64_t)0xffffffff80000000)

#define IMAGE_DOS_SIGNATURE 0x5a4d

typedef struct _IMAGE_DOS_HEADER {
    uint16_t e_magic;
    uint16_t e_cblp;
    uint16_t e_cp;
    uint16_t e_crlc;
    uint16_t e_cparhdr;
    uint16_t e_minalloc;
    uint16_t e_maxalloc;
    uint16_t e_ss;
    uint16_t e_sp;
    uint16_t e_csum;
    uint16_t e_ip;
    uint16_t e_cs;
    uint16_t e_lfarlc;
    uint16_t e_ovno;
    uint16_t e_res[4];
    uint16_t e_oemid;
    uint16_t e_oeminfo;
    uint16_t e_res2[10];
    uint32_t e_lfanew;
} IMAGE_DOS_HEADER;

#define IMAGE_FILE_MACHINE_I386 0x14c
#define IMAGE_FILE_MACHINE_AMD64 0x8664
#define IMAGE_FILE_MACHINE_ARM64 0xaa64
#define IMAGE_FILE_MACHINE_RISCV64 0x5064
#define IMAGE_FILE_MACHINE_LOONGARCH64 0x6264

#define IMAGE_FILE_RELOCS_STRIPPED 1
#define IMAGE_FILE_EXECUTABLE_IMAGE 2

typedef struct {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
} IMAGE_FILE_HEADER;

typedef struct {
    uint32_t VirtualAddress;
    uint32_t Size;
} IMAGE_DATA_DIRECTORY;

#define IMAGE_NT_OPTIONAL_HDR32_MAGIC 0x10b
#define IMAGE_NT_OPTIONAL_HDR64_MAGIC 0x20b

#define IMAGE_DIRECTORY_ENTRY_EXPORT 0
#define IMAGE_DIRECTORY_ENTRY_IMPORT 1
#define IMAGE_DIRECTORY_ENTRY_RESOURCE 2
#define IMAGE_DIRECTORY_ENTRY_EXCEPTION 3
#define IMAGE_DIRECTORY_ENTRY_SECURITY 4
#define IMAGE_DIRECTORY_ENTRY_BASERELOC 5
#define IMAGE_DIRECTORY_ENTRY_DEBUG 6
#define IMAGE_DIRECTORY_ENTRY_ARCHITECTURE 7
#define IMAGE_DIRECTORY_ENTRY_GLOBALPTR 8
#define IMAGE_DIRECTORY_ENTRY_TLS 9
#define IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG 10
#define IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT 11
#define IMAGE_DIRECTORY_ENTRY_IAT 12
#define IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT 13
#define IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR 14

typedef struct {
    uint16_t Magic;
    uint8_t MajorLinkerVersion;
    uint8_t MinorLinkerVersion;
    uint32_t SizeOfCode;
    uint32_t SizeOfInitializedData;
    uint32_t SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint;
    uint32_t BaseOfCode;
    uint64_t ImageBase;
    uint32_t SectionAlignment;
    uint32_t FileAlignment;
    uint16_t MajorOperatingSystemVersion;
    uint16_t MinorOperatingSystemVersion;
    uint16_t MajorImageVersion;
    uint16_t MinorImageVersion;
    uint16_t MajorSubsystemVersion;
    uint16_t MinorSubsystemVersion;
    uint32_t Win32VersionValue;
    uint32_t SizeOfImage;
    uint32_t SizeOfHeaders;
    uint32_t CheckSum;
    uint16_t Subsystem;
    uint16_t DllCharacteristics;
    uint64_t SizeOfStackReserve;
    uint64_t SizeOfStackCommit;
    uint64_t SizeOfHeapReserve;
    uint64_t SizeOfHeapCommit;
    uint32_t LoaderFlags;
    uint32_t NumberOfRvaAndSizes;
    IMAGE_DATA_DIRECTORY DataDirectory[16];
} IMAGE_OPTIONAL_HEADER64;

#define IMAGE_NT_SIGNATURE 0x4550

typedef struct {
    uint32_t Signature;
    IMAGE_FILE_HEADER FileHeader;
    IMAGE_OPTIONAL_HEADER64 OptionalHeader;
} IMAGE_NT_HEADERS64;

#define IMAGE_SCN_MEM_DISCARDABLE 0x2000000
#define IMAGE_SCN_MEM_EXECUTE 0x20000000
#define IMAGE_SCN_MEM_READ 0x40000000
#define IMAGE_SCN_MEM_WRITE 0x80000000

typedef struct {
    char Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations;
    uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t Characteristics;
} IMAGE_SECTION_HEADER;

typedef struct {
    union {
        uint32_t Characteristics;
        uint32_t OriginalFirstThunk;
    };
    uint32_t TimeDateStamp;
    uint32_t ForwarderChain;
    uint32_t Name;
    uint32_t FirstThunk;
} IMAGE_IMPORT_DESCRIPTOR;

#define IMAGE_REL_BASED_ABSOLUTE 0
#define IMAGE_REL_BASED_HIGHLOW 3
#define IMAGE_REL_BASED_DIR64 10

typedef struct {
    uint32_t VirtualAddress;
    uint32_t SizeOfBlock;
} IMAGE_BASE_RELOCATION_BLOCK;

static void pe64_validate(uint8_t *image) {
    IMAGE_DOS_HEADER *dos_hdr = (IMAGE_DOS_HEADER *)image;

    if (dos_hdr->e_magic != IMAGE_DOS_SIGNATURE) {
        panic(true, "pe: Not a valid PE file");
    }

    IMAGE_NT_HEADERS64 *nt_hdrs = (IMAGE_NT_HEADERS64 *)(image + dos_hdr->e_lfanew);

    if (nt_hdrs->Signature != IMAGE_NT_SIGNATURE) {
        panic(true, "pe: Not a valid PE file");
    }

#if defined(__x86_64__) || defined(__i386__)
    if (nt_hdrs->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        panic(true, "pe: Not an x86-64 PE file");
    }
#elif defined(__aarch64__)
    if (nt_hdrs->FileHeader.Machine != IMAGE_FILE_MACHINE_ARM64) {
        panic(true, "pe: Not an ARM64 PE file");
    }
#elif defined (__riscv) && (__riscv_xlen == 64)
    if (nt_hdrs->FileHeader.Machine != IMAGE_FILE_MACHINE_RISCV64) {
        panic(true, "pe: Not a RISC-V PE file");
    }
#elif defined (__loongarch__) && (__loongarch_grlen == 64)
    if (nt_hdrs->FileHeader.Machine != IMAGE_FILE_MACHINE_LOONGARCH64) {
        panic(true, "pe: Not a loongarch64 PE file");
    }
#else
#error Unknown architecture
#endif
}

int pe_bits(uint8_t *image) {
    IMAGE_DOS_HEADER *dos_hdr = (IMAGE_DOS_HEADER *)image;

    if (dos_hdr->e_magic != IMAGE_DOS_SIGNATURE) {
        return -1;
    }

    IMAGE_NT_HEADERS64 *nt_hdrs = (IMAGE_NT_HEADERS64 *)(image + dos_hdr->e_lfanew);

    if (nt_hdrs->Signature != IMAGE_NT_SIGNATURE) {
        return -1;
    }

    switch (nt_hdrs->FileHeader.Machine) {
        case IMAGE_FILE_MACHINE_I386:
            return 32;
        case IMAGE_FILE_MACHINE_AMD64:
        case IMAGE_FILE_MACHINE_ARM64:
        case IMAGE_FILE_MACHINE_RISCV64:
        case IMAGE_FILE_MACHINE_LOONGARCH64:
            return 64;
    }

    return -1;
}

bool pe64_load(uint8_t *image, uint64_t *entry_point, uint64_t *_slide, uint32_t alloc_type, bool kaslr, struct mem_range **_ranges, uint64_t *_ranges_count, uint64_t *physical_base, uint64_t *virtual_base, uint64_t *_image_size, uint64_t *image_size_before_bss, bool *_is_reloc) {
    pe64_validate(image);

    IMAGE_DOS_HEADER *dos_hdr = (IMAGE_DOS_HEADER *)image;
    IMAGE_NT_HEADERS64 *nt_hdrs = (IMAGE_NT_HEADERS64 *)(image + dos_hdr->e_lfanew);
    IMAGE_SECTION_HEADER *sections = (IMAGE_SECTION_HEADER *)((uintptr_t)&nt_hdrs->OptionalHeader + nt_hdrs->FileHeader.SizeOfOptionalHeader);

    bool is_reloc = true;

    if (nt_hdrs->FileHeader.Characteristics & IMAGE_FILE_RELOCS_STRIPPED) {
        is_reloc = false;
    }

    if (_is_reloc) {
        *_is_reloc = is_reloc;
    }

    uint64_t image_base = nt_hdrs->OptionalHeader.ImageBase;
    uint64_t image_size = nt_hdrs->OptionalHeader.SizeOfImage;
    uint64_t alignment = nt_hdrs->OptionalHeader.SectionAlignment;

    bool lower_to_higher = false;

    if (image_base < FIXED_HIGHER_HALF_OFFSET_64) {
        if (!is_reloc) {
            panic(true, "pe: Lower half images are not allowed");
        }

        lower_to_higher = true;
    }

    uint64_t slide = 0;
    size_t try_count = 0;
    size_t max_simulated_tries = 0x10000;

    if (lower_to_higher) {
        slide = FIXED_HIGHER_HALF_OFFSET_64 - image_base;
    }

    *physical_base = (uintptr_t)ext_mem_alloc_type_aligned(image_size, alloc_type, alignment);
    *virtual_base = image_base;

    memcpy((void *)(uintptr_t)*physical_base, image, nt_hdrs->OptionalHeader.SizeOfHeaders);

    if (_image_size) {
        *_image_size = image_size;
    }

    if (is_reloc && kaslr) {
again:
        slide = (rand32() & ~(alignment - 1)) + (lower_to_higher ? FIXED_HIGHER_HALF_OFFSET_64 - image_base : 0);

        if (*virtual_base + slide + image_size < 0xffffffff80000000 /* this comparison relies on overflow */) {
            if (++try_count == max_simulated_tries) {
                panic(true, "pe: Image wants to load too high");
            }
            goto again;
        }
    }

    for (size_t i = 0; i < nt_hdrs->FileHeader.NumberOfSections; i++) {
        IMAGE_SECTION_HEADER *section = &sections[i];

        uintptr_t section_base = *physical_base + section->VirtualAddress;
        uint32_t section_raw_size = section->VirtualSize < section->SizeOfRawData ? section->VirtualSize : section->SizeOfRawData;

        memcpy((void *)section_base, image + section->PointerToRawData, section_raw_size);
    }

    IMAGE_DATA_DIRECTORY *import_dir = &nt_hdrs->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_DATA_DIRECTORY *reloc_dir = &nt_hdrs->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];

    if (import_dir->Size != 0) {
        IMAGE_IMPORT_DESCRIPTOR *import_desc = (IMAGE_IMPORT_DESCRIPTOR *)((uintptr_t)*physical_base + import_dir->VirtualAddress);

        if (import_desc->Name != 0) {
            panic(true, "pe: Kernel must not have any imports");
        }
    }

    if (reloc_dir->VirtualAddress != 0) {
        size_t reloc_block_offset = 0;

        while (reloc_dir->Size - reloc_block_offset >= sizeof(IMAGE_BASE_RELOCATION_BLOCK)) {
            IMAGE_BASE_RELOCATION_BLOCK *block = (IMAGE_BASE_RELOCATION_BLOCK *)((uintptr_t)*physical_base + reloc_dir->VirtualAddress + reloc_block_offset);

            uintptr_t block_base = *physical_base + block->VirtualAddress;
            size_t entries = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION_BLOCK)) / sizeof(uint16_t);
            uint16_t *relocs = (uint16_t *)(block + 1);

            for (size_t i = 0; i < entries; i++) {
                uint16_t type = relocs[i] >> 12;
                uint16_t offset = relocs[i] & 0xfff;

                if (type == IMAGE_REL_BASED_ABSOLUTE) {
                    continue;
                }

                switch (type) {
                    case IMAGE_REL_BASED_HIGHLOW:
                        *(uint32_t *)(block_base + offset) += slide;
                        break;
                    case IMAGE_REL_BASED_DIR64:
                        *(uint64_t *)(block_base + offset) += slide;
                        break;
                    default:
                        panic(true, "pe: Unsupported relocation type %u", type);
                }
            }

            reloc_block_offset += block->SizeOfBlock;
        }
    }

    if (image_size_before_bss) {
        *image_size_before_bss = image_size;
    }

    *virtual_base += slide;
    *entry_point = *virtual_base + nt_hdrs->OptionalHeader.AddressOfEntryPoint;

    if (_slide) {
        *_slide = slide;
    }

    if (_ranges && _ranges_count) {
        size_t range_count = 0;

        for (size_t i = 0; i < nt_hdrs->FileHeader.NumberOfSections; i++) {
            IMAGE_SECTION_HEADER *section = &sections[i];

            if (section->Characteristics & IMAGE_SCN_MEM_DISCARDABLE) {
                continue;
            }

            range_count++;
        }

        struct mem_range *ranges = ext_mem_alloc(range_count * sizeof(struct mem_range));

        *_ranges = ranges;
        *_ranges_count = range_count;

        for (size_t i = 0, j = 0; i < nt_hdrs->FileHeader.NumberOfSections; i++) {
            IMAGE_SECTION_HEADER *section = &sections[i];

            if (section->Characteristics & IMAGE_SCN_MEM_DISCARDABLE) {
                continue;
            }

            uintptr_t misalign = section->VirtualAddress % alignment;

            ranges[j].base = *virtual_base + ALIGN_DOWN(section->VirtualAddress, alignment);
            ranges[j].length = ALIGN_UP(section->VirtualSize + misalign, alignment);

            if (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
                ranges[j].permissions |= MEM_RANGE_X;
            }

            if (section->Characteristics & IMAGE_SCN_MEM_WRITE) {
                ranges[j].permissions |= MEM_RANGE_W;
            }

            if (section->Characteristics & IMAGE_SCN_MEM_READ) {
                ranges[j].permissions |= MEM_RANGE_R;
            }

            j++;
        }
    }

    return true;
}
