#include <stdint.h>
#include <stddef.h>
#include <lib/misc.h>
#include <sys/cpu.h>
#include <lib/libc.h>
#include <lib/pe.h>
#include <lib/print.h>
#include <lib/rand.h>
#include <mm/pmm.h>

#define FIXED_HIGHER_HALF_OFFSET_64 ((uint64_t)0xffffffff80000000)

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

bool pe_is_valid(struct file_handle *file) {
    if (file == NULL || file->size < sizeof(IMAGE_DOS_HEADER) + sizeof(uint32_t)) {
        return false;
    }

    uint16_t dos_magic = 0;
    if (fread(file, &dos_magic, offsetof(IMAGE_DOS_HEADER, e_magic), sizeof(uint16_t)) != sizeof(uint16_t)) {
        return false;
    }

    if (dos_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }

    uint32_t e_lfanew = 0;
    if (fread(file, &e_lfanew, offsetof(IMAGE_DOS_HEADER, e_lfanew), sizeof(uint32_t)) != sizeof(uint32_t)) {
        return false;
    }

    if (e_lfanew < sizeof(IMAGE_DOS_HEADER) || e_lfanew > file->size - sizeof(uint32_t)) {
        return false;
    }

    uint32_t pe_sig = 0;
    if (fread(file, &pe_sig, e_lfanew, sizeof(uint32_t)) != sizeof(uint32_t)) {
        return false;
    }

    return pe_sig == IMAGE_NT_SIGNATURE;
}

static void pe64_validate(uint8_t *image, size_t file_size) {
    IMAGE_DOS_HEADER *dos_hdr = (IMAGE_DOS_HEADER *)image;

    if (file_size < sizeof(IMAGE_DOS_HEADER)) {
        panic(true, "pe: File too small for DOS header");
    }

    if (dos_hdr->e_magic != IMAGE_DOS_SIGNATURE) {
        panic(true, "pe: Not a valid PE file");
    }

    if (file_size < sizeof(IMAGE_NT_HEADERS64)) {
        panic(true, "pe: File too small for NT headers");
    }

    if (dos_hdr->e_lfanew > file_size - sizeof(IMAGE_NT_HEADERS64)) {
        panic(true, "pe: e_lfanew offset out of bounds");
    }

    // The PE layout puts the header on an 8-byte boundary.
    if (dos_hdr->e_lfanew % 8 != 0) {
        panic(true, "pe: NT headers are not aligned in the file");
    }

    IMAGE_NT_HEADERS64 *nt_hdrs = (IMAGE_NT_HEADERS64 *)(image + dos_hdr->e_lfanew);

    if (nt_hdrs->Signature != IMAGE_NT_SIGNATURE) {
        panic(true, "pe: Not a valid PE file");
    }

    if (nt_hdrs->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        panic(true, "pe: Not a valid PE32+ file");
    }

    // Directories past the ones the header structure holds go unused.
    size_t directory_count = MIN(nt_hdrs->OptionalHeader.NumberOfRvaAndSizes, SIZEOF_ARRAY(nt_hdrs->OptionalHeader.DataDirectory));
    size_t directories_size = directory_count * sizeof(IMAGE_DATA_DIRECTORY);

    if (nt_hdrs->FileHeader.SizeOfOptionalHeader < offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory) + directories_size) {
        panic(true, "pe: SizeOfOptionalHeader too small");
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

int pe_bits(uint8_t *image, size_t image_size) {
    if (image_size < sizeof(IMAGE_DOS_HEADER)) {
        return -1;
    }

    IMAGE_DOS_HEADER *dos_hdr = (IMAGE_DOS_HEADER *)image;

    if (dos_hdr->e_magic != IMAGE_DOS_SIGNATURE) {
        return -1;
    }

    if (image_size < sizeof(IMAGE_NT_HEADERS64)) {
        return -1;
    }

    if ((size_t)dos_hdr->e_lfanew > image_size - sizeof(IMAGE_NT_HEADERS64)) {
        return -1;
    }

    if (dos_hdr->e_lfanew % 8 != 0) {
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

// A relocation's target alignment is the image's to choose, and a wide
// unaligned access may trap on riscv64 and loongarch64.
static uint64_t reloc_load(const void *p, size_t size) {
    const volatile uint8_t *s = p;
    uint64_t v = 0;

    for (size_t i = 0; i < size; i++) {
        v |= (uint64_t)s[i] << (i * 8);
    }

    return v;
}

static void reloc_store(void *p, uint64_t v, size_t size) {
    volatile uint8_t *d = p;

    for (size_t i = 0; i < size; i++) {
        d[i] = (uint8_t)(v >> (i * 8));
    }
}

// Only object files are meant to have a zero VirtualSize, so an image section
// with one is taken to span its raw data.
static uint32_t pe_section_virtual_size(IMAGE_SECTION_HEADER *section) {
    if (section->VirtualSize == 0) {
        return section->SizeOfRawData;
    }

    return section->VirtualSize;
}

static IMAGE_DATA_DIRECTORY *pe64_data_directory(IMAGE_NT_HEADERS64 *nt_hdrs, size_t index) {
    if (index >= nt_hdrs->OptionalHeader.NumberOfRvaAndSizes) {
        return NULL;
    }

    return &nt_hdrs->OptionalHeader.DataDirectory[index];
}

bool pe64_load(uint8_t *image, size_t file_size, uint64_t *entry_point, uint64_t *_slide, uint32_t alloc_type, bool kaslr, struct mem_range **_ranges, uint64_t *_ranges_count, uint64_t *physical_base, uint64_t *virtual_base, uint64_t *_image_size, uint64_t *image_size_before_bss, bool *_is_reloc) {
    pe64_validate(image, file_size);

    IMAGE_DOS_HEADER *dos_hdr = (IMAGE_DOS_HEADER *)image;
    IMAGE_NT_HEADERS64 *nt_hdrs = (IMAGE_NT_HEADERS64 *)(image + dos_hdr->e_lfanew);

    // Validate SizeOfOptionalHeader doesn't cause sections pointer to go out of bounds
    uint64_t sections_offset = (uint64_t)dos_hdr->e_lfanew + sizeof(uint32_t) + sizeof(IMAGE_FILE_HEADER) + nt_hdrs->FileHeader.SizeOfOptionalHeader;
    uint64_t sections_end = sections_offset + (uint64_t)nt_hdrs->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
    if (sections_end > file_size) {
        panic(true, "pe: Section headers extend beyond file bounds");
    }

    if (sections_offset % 4 != 0) {
        panic(true, "pe: Section headers are not aligned in the file");
    }

    if (nt_hdrs->FileHeader.NumberOfSections == 0) {
        panic(true, "pe: Executable has no sections");
    }

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

    if (alignment == 0) {
        panic(true, "pe: SectionAlignment is zero");
    }

    if (alignment > 1 && (alignment & (alignment - 1)) != 0) {
        panic(true, "pe: SectionAlignment is not a power of 2");
    }

    // Permissions are applied per section but mapped a page at a time, so
    // sections sharing a page could demand conflicting ones.
    if (alignment < 0x1000) {
        panic(true, "pe: SectionAlignment is below the page size");
    }

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

    // Validate SizeOfHeaders doesn't exceed file size or image size
    if (nt_hdrs->OptionalHeader.SizeOfHeaders > file_size
     || nt_hdrs->OptionalHeader.SizeOfHeaders > image_size) {
        panic(true, "pe: SizeOfHeaders exceeds file or image size");
    }

    memcpy((void *)(uintptr_t)*physical_base, image, nt_hdrs->OptionalHeader.SizeOfHeaders);

    if (_image_size) {
        *_image_size = image_size;
    }

    if (is_reloc && kaslr) {
again:
        slide = (safe_rand32() & ~(alignment - 1)) + (lower_to_higher ? FIXED_HIGHER_HALF_OFFSET_64 - image_base : 0);

        if (*virtual_base + slide + image_size < 0xffffffff80000000 /* this comparison relies on overflow */) {
            if (++try_count == max_simulated_tries) {
                panic(true, "pe: Image wants to load too high");
            }
            goto again;
        }
    }

    uint64_t prev_section_top = 0;

    for (size_t i = 0; i < nt_hdrs->FileHeader.NumberOfSections; i++) {
        IMAGE_SECTION_HEADER *section = &sections[i];

        uintptr_t section_base = *physical_base + section->VirtualAddress;
        uint32_t section_size = pe_section_virtual_size(section);
        uint32_t section_raw_size = section_size < section->SizeOfRawData ? section_size : section->SizeOfRawData;

        // Validate section doesn't write past the image buffer
        if ((uint64_t)section->VirtualAddress + section_raw_size > image_size) {
            panic(true, "pe: Section %U exceeds image bounds", (uint64_t)i);
        }

        // The full virtual size (not just the raw size copied above) is mapped
        // by the mem_range pass, so the whole virtual extent must fit too.
        if ((uint64_t)section->VirtualAddress + section_size > image_size) {
            panic(true, "pe: Section %U virtual size exceeds image bounds", (uint64_t)i);
        }

        // The PE specification has section VAs ascending and adjacent, so one
        // starting below the previous top would overwrite it once copied.
        if ((uint64_t)section->VirtualAddress < prev_section_top) {
            panic(true, "pe: Section %U overlaps or is out of order", (uint64_t)i);
        }

        prev_section_top = (uint64_t)section->VirtualAddress + section_size;

        if (section->VirtualAddress % alignment != 0) {
            panic(true, "pe: Section %U is not aligned to SectionAlignment", (uint64_t)i);
        }

        // Validate section data doesn't exceed file bounds
        if ((uint64_t)section->PointerToRawData + section_raw_size > file_size) {
            panic(true, "pe: Section %U data extends beyond file bounds", (uint64_t)i);
        }

        memcpy((void *)section_base, image + section->PointerToRawData, section_raw_size);
    }

    // Unless a section covers them, the headers get a read-only range of their
    // own, so their pages have to end before the first section begins.
    if (sections[0].VirtualAddress != 0) {
        uint64_t headers_top = ALIGN_UP((uint64_t)nt_hdrs->OptionalHeader.SizeOfHeaders,
                                        0x1000, panic(true, "pe: Alignment overflow"));

        if (headers_top > sections[0].VirtualAddress) {
            panic(true, "pe: Headers overlap the first section");
        }
    }

    IMAGE_DATA_DIRECTORY *import_dir = pe64_data_directory(nt_hdrs, IMAGE_DIRECTORY_ENTRY_IMPORT);
    IMAGE_DATA_DIRECTORY *reloc_dir = pe64_data_directory(nt_hdrs, IMAGE_DIRECTORY_ENTRY_BASERELOC);

    if (import_dir != NULL && import_dir->Size != 0) {
        if (import_dir->VirtualAddress >= image_size ||
            sizeof(IMAGE_IMPORT_DESCRIPTOR) > image_size - import_dir->VirtualAddress) {
            panic(true, "pe: Import directory VirtualAddress out of bounds");
        }

        if (import_dir->VirtualAddress % 4 != 0) {
            panic(true, "pe: Import directory is not aligned in the image");
        }

        IMAGE_IMPORT_DESCRIPTOR *import_desc = (IMAGE_IMPORT_DESCRIPTOR *)((uintptr_t)*physical_base + import_dir->VirtualAddress);

        if (import_desc->Name != 0) {
            panic(true, "pe: Kernel must not have any imports");
        }
    }

    if (reloc_dir != NULL && reloc_dir->VirtualAddress != 0) {
        if (reloc_dir->VirtualAddress >= image_size ||
            reloc_dir->Size > image_size - reloc_dir->VirtualAddress) {
            panic(true, "pe: Relocation directory VirtualAddress out of bounds");
        }
        size_t reloc_block_offset = 0;

        while (reloc_dir->Size - reloc_block_offset >= sizeof(IMAGE_BASE_RELOCATION_BLOCK)) {
            // Each base relocation block must start on a 32-bit boundary.
            if (((uint64_t)reloc_dir->VirtualAddress + reloc_block_offset) % 4 != 0) {
                panic(true, "pe: Relocation block is not aligned in the image");
            }

            IMAGE_BASE_RELOCATION_BLOCK *block = (IMAGE_BASE_RELOCATION_BLOCK *)((uintptr_t)*physical_base + reloc_dir->VirtualAddress + reloc_block_offset);

            // The block header lives in the image these relocations write to,
            // where the directory above does not, so its fields are taken once.
            uint32_t block_va = block->VirtualAddress;
            uint32_t block_size = block->SizeOfBlock;

            // Validate SizeOfBlock to prevent infinite loop (if 0) and underflow (if too small)
            if (block_size < sizeof(IMAGE_BASE_RELOCATION_BLOCK)) {
                panic(true, "pe: Invalid relocation block size");
            }

            if (block_size > reloc_dir->Size - reloc_block_offset) {
                panic(true, "pe: Relocation block size exceeds directory");
            }

            if (block_va >= image_size) {
                panic(true, "pe: Relocation block VirtualAddress out of bounds");
            }

            uintptr_t block_base = *physical_base + block_va;
            size_t entries = (block_size - sizeof(IMAGE_BASE_RELOCATION_BLOCK)) / sizeof(uint16_t);
            uint16_t *relocs = (uint16_t *)(block + 1);

            for (size_t i = 0; i < entries; i++) {
                uint16_t type = relocs[i] >> 12;
                uint16_t offset = relocs[i] & 0xfff;

                if (type == IMAGE_REL_BASED_ABSOLUTE) {
                    continue;
                }

                size_t write_size;
                switch (type) {
                    case IMAGE_REL_BASED_HIGHLOW:
                        if (lower_to_higher) {
                            panic(true, "pe: 32-bit relocations are incompatible with higher-half loading");
                        }
                        write_size = 4;
                        break;
                    case IMAGE_REL_BASED_DIR64: write_size = 8; break;
                    default:
                        panic(true, "pe: Unsupported relocation type %u", type);
                }

                if ((uint64_t)block_va + offset + write_size > image_size) {
                    panic(true, "pe: Relocation offset out of bounds");
                }

                void *ptr = (void *)(block_base + offset);

                reloc_store(ptr, reloc_load(ptr, write_size) + slide, write_size);
            }

            reloc_block_offset += block_size;
        }
    }

    sync_icache_range((uintptr_t)*physical_base, (uintptr_t)*physical_base + image_size);

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

        bool headers_within_section = false;

        for (size_t i = 0; i < nt_hdrs->FileHeader.NumberOfSections; i++) {
            IMAGE_SECTION_HEADER *section = &sections[i];

            if (section->VirtualAddress == 0) {
                headers_within_section = true;
            }

            range_count++;
        }

        if (!headers_within_section) {
            range_count++;
        }

        struct mem_range *ranges = ext_mem_alloc_counted(range_count, sizeof(struct mem_range));

        *_ranges = ranges;
        *_ranges_count = range_count;

        size_t range_index = 0;

        // A range rounded past SizeOfImage maps memory the image does not own,
        // and ImageBase can wrap those addresses, so the clamp compares lengths.
        uint64_t image_extent = ALIGN_UP(image_size, 0x1000, panic(true, "pe: Alignment overflow"));
        uint64_t image_top = CHECKED_ADD(*virtual_base, image_extent,
            panic(true, "pe: Image extends past the address space"));

        if (!headers_within_section) {
            struct mem_range *range = &ranges[range_index++];
            range->base = *virtual_base;
            range->length = ALIGN_UP(nt_hdrs->OptionalHeader.SizeOfHeaders, 0x1000, panic(true, "pe: Alignment overflow"));

            if (range->length > image_top - range->base) {
                range->length = image_top - range->base;
            }

            range->permissions = MEM_RANGE_R;
        }

        for (size_t i = 0; i < nt_hdrs->FileHeader.NumberOfSections; i++) {
            IMAGE_SECTION_HEADER *section = &sections[i];

            struct mem_range *range = &ranges[range_index++];
            range->base = *virtual_base + section->VirtualAddress;
            // ALIGN_UP accumulates in its first argument's type, so the cast
            // is what keeps a section rounding up past 4 GiB from truncating.
            range->length = ALIGN_UP((uint64_t)pe_section_virtual_size(section), alignment, panic(true, "pe: Alignment overflow"));

            if (range->length > image_top - range->base) {
                range->length = image_top - range->base;
            }

            if (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
                range->permissions |= MEM_RANGE_X;
            }

            if (section->Characteristics & IMAGE_SCN_MEM_WRITE) {
                range->permissions |= MEM_RANGE_W;
            }

            if (section->Characteristics & IMAGE_SCN_MEM_READ) {
                range->permissions |= MEM_RANGE_R;
            }
        }
    }

    return true;
}
