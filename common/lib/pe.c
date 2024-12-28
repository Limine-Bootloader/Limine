#include <stdint.h>
#include <stddef.h>
#include <lib/misc.h>
#include <lib/libc.h>
#include <lib/pe.h>
#include <lib/print.h>
#include <lib/rand.h>
#include <mm/pmm.h>

#define FIXED_HIGHER_HALF_OFFSET_64 ((uint64_t)0xffffffff80000000)

typedef unsigned char BYTE;
typedef unsigned char UCHAR;
typedef unsigned short USHORT;
typedef unsigned int DWORD;
typedef unsigned int ULONG;
typedef unsigned long long ULONGLONG;

typedef char CHAR;
typedef short SHORT;
typedef int LONG;
typedef long long LONGLONG;

#define IMAGE_DOS_SIGNATURE 0x5a4d

typedef struct _IMAGE_DOS_HEADER {
    USHORT e_magic;
    USHORT e_cblp;
    USHORT e_cp;
    USHORT e_crlc;
    USHORT e_cparhdr;
    USHORT e_minalloc;
    USHORT e_maxalloc;
    USHORT e_ss;
    USHORT e_sp;
    USHORT e_csum;
    USHORT e_ip;
    USHORT e_cs;
    USHORT e_lfarlc;
    USHORT e_ovno;
    USHORT e_res[4];
    USHORT e_oemid;
    USHORT e_oeminfo;
    USHORT e_res2[10];
    LONG e_lfanew;
} IMAGE_DOS_HEADER;

#define IMAGE_FILE_MACHINE_I386 0x14c
#define IMAGE_FILE_MACHINE_AMD64 0x8664
#define IMAGE_FILE_MACHINE_ARM64 0xaa64

#define IMAGE_FILE_RELOCS_STRIPPED 1
#define IMAGE_FILE_EXECUTABLE_IMAGE 2

typedef struct {
    USHORT Machine;
    USHORT NumberOfSections;
    ULONG TimeDateStamp;
    ULONG PointerToSymbolTable;
    ULONG NumberOfSymbols;
    USHORT SizeOfOptionalHeader;
    USHORT Characteristics;
} IMAGE_FILE_HEADER;

typedef struct {
    ULONG VirtualAddress;
    ULONG Size;
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
    USHORT Magic;
    UCHAR MajorLinkerVersion;
    UCHAR MinorLinkerVersion;
    ULONG SizeOfCode;
    ULONG SizeOfInitializedData;
    ULONG SizeOfUninitializedData;
    ULONG AddressOfEntryPoint;
    ULONG BaseOfCode;
    ULONGLONG ImageBase;
    ULONG SectionAlignment;
    ULONG FileAlignment;
    USHORT MajorOperatingSystemVersion;
    USHORT MinorOperatingSystemVersion;
    USHORT MajorImageVersion;
    USHORT MinorImageVersion;
    USHORT MajorSubsystemVersion;
    USHORT MinorSubsystemVersion;
    ULONG Win32VersionValue;
    ULONG SizeOfImage;
    ULONG SizeOfHeaders;
    ULONG CheckSum;
    USHORT Subsystem;
    USHORT DllCharacteristics;
    ULONGLONG SizeOfStackReserve;
    ULONGLONG SizeOfStackCommit;
    ULONGLONG SizeOfHeapReserve;
    ULONGLONG SizeOfHeapCommit;
    ULONG LoaderFlags;
    ULONG NumberOfRvaAndSizes;
    IMAGE_DATA_DIRECTORY DataDirectory[16];
} IMAGE_OPTIONAL_HEADER64;

#define IMAGE_NT_SIGNATURE 0x4550

typedef struct {
    ULONG Signature;
    IMAGE_FILE_HEADER FileHeader;
    IMAGE_OPTIONAL_HEADER64 OptionalHeader;
} IMAGE_NT_HEADERS64;

#define IMAGE_SCN_MEM_DISCARDABLE 0x2000000
#define IMAGE_SCN_MEM_EXECUTE 0x20000000
#define IMAGE_SCN_MEM_READ 0x40000000
#define IMAGE_SCN_MEM_WRITE 0x80000000

typedef struct {
    UCHAR Name[8];
    ULONG VirtualSize;
    ULONG VirtualAddress;
    ULONG SizeOfRawData;
    ULONG PointerToRawData;
    ULONG PointerToRelocations;
    ULONG PointerToLinenumbers;
    USHORT NumberOfRelocations;
    USHORT NumberOfLinenumbers;
    ULONG Characteristics;
} IMAGE_SECTION_HEADER;

typedef struct {
    union {
        DWORD Characteristics;
        DWORD OriginalFirstThunk;
    };
    DWORD TimeDateStamp;
    DWORD ForwarderChain;
    DWORD Name;
    DWORD FirstThunk;
} IMAGE_IMPORT_DESCRIPTOR;

#define IMAGE_REL_BASED_ABSOLUTE 0
#define IMAGE_REL_BASED_HIGHLOW 3
#define IMAGE_REL_BASED_DIR64 10

typedef struct {
    DWORD VirtualAddress;
    DWORD SizeOfBlock;
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
#else
#error Unsupported architecture
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

    memcpy((void *)*physical_base, image, nt_hdrs->OptionalHeader.SizeOfHeaders);

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

        uint64_t section_base = *physical_base + section->VirtualAddress;
        uint64_t section_raw_size = section->VirtualSize < section->SizeOfRawData ? section->VirtualSize : section->SizeOfRawData;

        memcpy((void *)section_base, image + section->PointerToRawData, section_raw_size);
    }

    IMAGE_DATA_DIRECTORY *import_dir = &nt_hdrs->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_DATA_DIRECTORY *reloc_dir = &nt_hdrs->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];

    if (import_dir->Size != 0) {
        IMAGE_IMPORT_DESCRIPTOR *import_desc = (IMAGE_IMPORT_DESCRIPTOR *)(*physical_base + import_dir->VirtualAddress);

        if (import_desc->Name != 0) {
            panic(true, "pe: Kernel must not have any imports");
        }
    }

    if (reloc_dir->VirtualAddress != 0) {
        IMAGE_BASE_RELOCATION_BLOCK *block = (IMAGE_BASE_RELOCATION_BLOCK *)(*physical_base + reloc_dir->VirtualAddress);

        while (block->VirtualAddress != 0) {
            uint64_t block_base = *physical_base + block->VirtualAddress;

            uint64_t entries = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION_BLOCK)) / sizeof(uint16_t);
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
                        panic(true, "pe: Unknown relocation type %u", type);
                }
            }

            block = (IMAGE_BASE_RELOCATION_BLOCK *)((uintptr_t)block + block->SizeOfBlock);
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

            ranges[j].base = *virtual_base + ALIGN_UP(section->VirtualAddress, alignment);
            ranges[j].length = ALIGN_UP(section->VirtualSize, alignment);

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
