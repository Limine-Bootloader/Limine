#include <stdint.h>
#include <stddef.h>
#include <lib/misc.h>
#include <sys/cpu.h>
#include <lib/libc.h>
#include <lib/elf.h>
#include <lib/print.h>
#include <lib/rand.h>
#include <lib/elsewhere.h>
#include <mm/pmm.h>
#include <fs/file.h>

#define ET_NONE     0
#define ET_REL      1
#define ET_EXEC     2
#define ET_DYN      3

#define PT_LOAD     0x00000001
#define PT_DYNAMIC  0x00000002
#define PT_INTERP   0x00000003
#define PT_PHDR     0x00000006

#define DT_NULL     0x00000000
#define DT_NEEDED   0x00000001
#define DT_RELA     0x00000007
#define DT_RELASZ   0x00000008
#define DT_RELAENT  0x00000009
#define DT_RELR     0x00000024
#define DT_RELRSZ   0x00000023
#define DT_RELRENT  0x00000025
#define DT_SYMTAB   0x00000006
#define DT_SYMENT   0x0000000b
#define DT_STRTAB   0x00000005
#define DT_PLTREL   0x00000014
#define DT_PLTRELSZ 0x00000002
#define DT_JMPREL   0x00000017
#define DT_FLAGS_1  0x6ffffffb

#define DF_1_PIE    0x08000000

#define ABI_SYSV     0x00
#define ARCH_X86_64  0x3e
#define ARCH_X86_32  0x03
#define ARCH_AARCH64 0xb7
#define ARCH_RISCV   0xf3
#define ARCH_LOONGARCH 0x102
#define BITS_LE      0x01
#define ELFCLASS64   0x02
#define SHT_RELA     0x00000004
#define SHN_UNDEF    0x00000000
#define STB_WEAK     0x00000002
#define R_X86_64_NONE      0x00000000
#define R_AARCH64_NONE     0x00000000
#define R_RISCV_NONE       0x00000000
#define R_LARCH_NONE       0x00000000
#define R_X86_64_RELATIVE  0x00000008
#define R_AARCH64_RELATIVE 0x00000403
#define R_RISCV_RELATIVE   0x00000003
#define R_LARCH_RELATIVE   0x00000003
#define R_X86_64_GLOB_DAT  0x00000006
#define R_AARCH64_GLOB_DAT 0x00000401
#define R_X86_64_JUMP_SLOT 0x00000007
#define R_AARCH64_JUMP_SLOT 0x00000402
#define R_RISCV_JUMP_SLOT  0x00000005
#define R_LARCH_JUMP_SLOT  0x00000005
#define R_X86_64_64        0x00000001
#define R_RISCV_64         0x00000002
#define R_LARCH_64         0x00000002
#define R_AARCH64_ABS64    0x00000101

#define R_INTERNAL_RELR    0xfffffff0

/* Indices into identification array */
#define EI_CLASS    4
#define EI_DATA     5
#define EI_VERSION  6
#define EI_OSABI    7

struct elf32_hdr {
    uint8_t  ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t hdr_size;
    uint16_t phdr_size;
    uint16_t ph_num;
    uint16_t shdr_size;
    uint16_t sh_num;
    uint16_t shstrndx;
};

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

struct elf32_phdr {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
};

struct elf64_rela {
    uint64_t r_addr;
    uint32_t r_info;
    uint32_t r_symbol;
    uint64_t r_addend;
};

struct elf32_dyn {
    uint32_t d_tag;
    uint32_t d_un;
};

struct elf64_dyn {
    uint64_t d_tag;
    uint64_t d_un;
};

static bool elf32_validate(struct elf32_hdr *hdr) {
    if (strncmp((char *)hdr->ident, "\177ELF", 4)) {
        panic(true, "elf: Not a valid ELF file.");
    }

    if (hdr->ident[EI_DATA] != BITS_LE) {
        panic(true, "elf: Not a Little-endian ELF file.");
    }

    if (hdr->machine != ARCH_X86_32) {
        panic(true, "elf: Not an IA-32 ELF file.");
    }

    return true;
}

static bool elf64_validate(struct elf64_hdr *hdr) {
    if (strncmp((char *)hdr->ident, "\177ELF", 4)) {
        panic(true, "elf: Not a valid ELF file.");
    }

    if (hdr->ident[EI_DATA] != BITS_LE) {
        panic(true, "elf: Not a Little-endian ELF file.");
    }

#if defined (__x86_64__) || defined (__i386__)
    if (hdr->machine != ARCH_X86_64) {
        panic(true, "elf: Not an x86-64 ELF file.");
    }
#elif defined (__aarch64__)
    if (hdr->machine != ARCH_AARCH64) {
        panic(true, "elf: Not an aarch64 ELF file.");
    }
#elif defined (__riscv)
    if (hdr->machine != ARCH_RISCV && hdr->ident[EI_CLASS] == ELFCLASS64) {
        panic(true, "elf: Not a riscv64 ELF file.");
    }
#elif defined (__loongarch64)
    if (hdr->machine != ARCH_LOONGARCH && hdr->ident[EI_CLASS] == ELFCLASS64) {
        panic(true, "elf: Not a loongarch64 ELF file.");
    }
#else
#error Unknown architecture
#endif

    return true;
}

int elf_bits(uint8_t *elf) {
    struct elf64_hdr *hdr = (void *)elf;

    if (strncmp((char *)hdr->ident, "\177ELF", 4)) {
        return -1;
    }

    switch (hdr->machine) {
        case ARCH_X86_64:
        case ARCH_AARCH64:
            return 64;
        case ARCH_RISCV:
        case ARCH_LOONGARCH:
            return (hdr->ident[EI_CLASS] == ELFCLASS64) ? 64 : 32;
        case ARCH_X86_32:
            return 32;
        default:
            return -1;
    }
}

struct elf_section_hdr_info elf64_section_hdr_info(uint8_t *elf) {
    struct elf_section_hdr_info info = {0};

    struct elf64_hdr *hdr = (void *)elf;

    elf64_validate(hdr);

    info.num = hdr->sh_num;
    info.section_entry_size = hdr->shdr_size;
    info.str_section_idx = hdr->shstrndx;
    info.section_offset = hdr->shoff;

    return info;
}

struct elf_section_hdr_info elf32_section_hdr_info(uint8_t *elf) {
    struct elf_section_hdr_info info = {0};

    struct elf32_hdr *hdr = (void *)elf;

    elf32_validate(hdr);

    info.num = hdr->sh_num;
    info.section_entry_size = hdr->shdr_size;
    info.str_section_idx = hdr->shstrndx;
    info.section_offset = hdr->shoff;

    return info;
}

static bool elf64_is_relocatable(uint8_t *elf, struct elf64_hdr *hdr) {
    if (hdr->phdr_size < sizeof(struct elf64_phdr)) {
        panic(true, "elf: phdr_size < sizeof(struct elf64_phdr)");
    }

    if (hdr->type != ET_DYN) {
        return false;
    }

    // Find PT_DYNAMIC segment
    for (size_t i = 0; i < hdr->ph_num; i++) {
        struct elf64_phdr *phdr = (void *)elf + (hdr->phoff + i * hdr->phdr_size);

        if (phdr->p_type != PT_DYNAMIC) {
            continue;
        }

        if (phdr->p_filesz == 0) {
            panic(true, "elf: ELF file type is ET_DYN, but PT_DYNAMIC segment has 0 size");
        }

        return true;
    }

    panic(true, "elf: ELF file type is ET_DYN, but PT_DYNAMIC segment missing");
}

static bool elf64_apply_relocations(uint8_t *elf, struct elf64_hdr *hdr, void *buffer, uint64_t vaddr, size_t size, uint64_t slide) {
    if (hdr->phdr_size < sizeof(struct elf64_phdr)) {
        panic(true, "elf: phdr_size < sizeof(struct elf64_phdr)");
    }

    uint64_t symtab_offset = 0;
    uint64_t symtab_ent = 0;
    uint64_t strtab_offset = 0;

    uint64_t dt_pltrel = 0;
    uint64_t dt_pltrelsz = 0;
    uint64_t dt_jmprel = 0;

    uint64_t relr_offset = 0;
    uint64_t relr_size = 0;
    uint64_t relr_ent = 0;

    uint64_t rela_offset = 0;
    uint64_t rela_size = 0;
    uint64_t rela_ent = 0;

    // Find DYN segment
    for (uint16_t i = 0; i < hdr->ph_num; i++) {
        struct elf64_phdr *phdr = (void *)elf + (hdr->phoff + i * hdr->phdr_size);

        if (phdr->p_type != PT_DYNAMIC)
            continue;

        for (uint16_t j = 0; j < phdr->p_filesz / sizeof(struct elf64_dyn); j++) {
            struct elf64_dyn *dyn = (void *)elf + (phdr->p_offset + j * sizeof(struct elf64_dyn));

            switch (dyn->d_tag) {
                case DT_RELA:
                    rela_offset = dyn->d_un;
                    break;
                case DT_RELAENT:
                    rela_ent = dyn->d_un;
                    break;
                case DT_RELASZ:
                    rela_size = dyn->d_un;
                    break;
                case DT_RELR:
                    relr_offset = dyn->d_un;
                    break;
                case DT_RELRENT:
                    relr_ent = dyn->d_un;
                    break;
                case DT_RELRSZ:
                    relr_size = dyn->d_un;
                    break;
                case DT_SYMTAB:
                    symtab_offset = dyn->d_un;
                    break;
                case DT_STRTAB:
                    strtab_offset = dyn->d_un;
                    break;
                case DT_SYMENT:
                    symtab_ent = dyn->d_un;
                    if (symtab_ent < sizeof(struct elf64_sym)) {
                        panic(true, "elf: symtab_ent < sizeof(struct elf64_sym)");
                    }
                    break;
                case DT_PLTREL:
                    dt_pltrel = dyn->d_un;
                    break;
                case DT_PLTRELSZ:
                    dt_pltrelsz = dyn->d_un;
                    break;
                case DT_JMPREL:
                    dt_jmprel = dyn->d_un;
                    break;
                case DT_NEEDED:
                    panic(true, "elf: ELF file attempts to load a dynamically linked library");
                case DT_NULL:
                    goto end_of_pt_segment;
            }
        }

        break;
    }
end_of_pt_segment:

    if (rela_offset != 0) {
        for (uint16_t i = 0; i < hdr->ph_num; i++) {
            struct elf64_phdr *_phdr = (void *)elf + (hdr->phoff + i * hdr->phdr_size);

            if (_phdr->p_vaddr <= rela_offset && _phdr->p_vaddr + _phdr->p_filesz > rela_offset) {
                rela_offset -= _phdr->p_vaddr;
                rela_offset += _phdr->p_offset;
                break;
            }
        }
    }

    if (relr_offset != 0) {
        for (uint16_t i = 0; i < hdr->ph_num; i++) {
            struct elf64_phdr *_phdr = (void *)elf + (hdr->phoff + i * hdr->phdr_size);

            if (_phdr->p_vaddr <= relr_offset && _phdr->p_vaddr + _phdr->p_filesz > relr_offset) {
                relr_offset -= _phdr->p_vaddr;
                relr_offset += _phdr->p_offset;
                break;
            }
        }
    }

    if (symtab_offset != 0) {
        for (uint16_t i = 0; i < hdr->ph_num; i++) {
            struct elf64_phdr *_phdr = (void *)elf + (hdr->phoff + i * hdr->phdr_size);

            if (_phdr->p_vaddr <= symtab_offset && _phdr->p_vaddr + _phdr->p_filesz > symtab_offset) {
                symtab_offset -= _phdr->p_vaddr;
                symtab_offset += _phdr->p_offset;
                break;
            }
        }
    }

    if (strtab_offset != 0) {
        for (uint16_t i = 0; i < hdr->ph_num; i++) {
            struct elf64_phdr *_phdr = (void *)elf + (hdr->phoff + i * hdr->phdr_size);

            if (_phdr->p_vaddr <= strtab_offset && _phdr->p_vaddr + _phdr->p_filesz > strtab_offset) {
                strtab_offset -= _phdr->p_vaddr;
                strtab_offset += _phdr->p_offset;
                break;
            }
        }
    }

    if (dt_jmprel != 0) {
        for (uint16_t i = 0; i < hdr->ph_num; i++) {
            struct elf64_phdr *_phdr = (void *)elf + (hdr->phoff + i * hdr->phdr_size);

            if (_phdr->p_vaddr <= dt_jmprel && _phdr->p_vaddr + _phdr->p_filesz > dt_jmprel) {
                dt_jmprel -= _phdr->p_vaddr;
                dt_jmprel += _phdr->p_offset;
                break;
            }
        }
    }

    size_t relocs_i = 0;
    if (relr_size != 0) {
        if (relr_ent != 8) {
            panic(true, "elf: relr_ent != 8");
        }
        for (size_t i = 0; i < relr_size / relr_ent; i++) {
            uint64_t entry = *((uint64_t *)(elf + relr_offset + i * relr_ent));

            if ((entry & 1) == 0) {
                relocs_i++;
            } else {
                relocs_i += __builtin_popcountll(entry) - 1;
            }
        }
    }
    size_t relr_count = relocs_i;
    if (rela_size != 0) {
        if (rela_ent < sizeof(struct elf64_rela)) {
            panic(true, "elf: rela_ent < sizeof(struct elf64_rela)");
        }
        relocs_i += rela_size / rela_ent;
    }
    if (dt_pltrelsz != 0) {
        if (dt_pltrel != DT_RELA) {
            panic(true, "elf: dt_pltrel != DT_RELA");
        }
        relocs_i += dt_pltrelsz / rela_ent;
    }
    struct elf64_rela **relocs = ext_mem_alloc(relocs_i * sizeof(struct elf64_rela *));

    if (relr_size != 0) {
        size_t relr_i;
        for (relr_i = 0; relr_i < relr_count; relr_i++) {
            relocs[relr_i] = ext_mem_alloc(sizeof(struct elf64_rela));
            relocs[relr_i]->r_info = R_INTERNAL_RELR;
        }

        // This logic is partially lifted from https://maskray.me/blog/2021-10-31-relative-relocations-and-relr
        uint64_t where = 0;
        relr_i = 0;
        for (size_t i = 0; i < relr_size / relr_ent; i++) {
            uint64_t entry = *((uint64_t *)(elf + relr_offset + i * relr_ent));

            if ((entry & 1) == 0) {
                where = entry;
                relocs[relr_i++]->r_addr = where;
                where += 8;
            } else {
                for (size_t j = 0; (entry >>= 1) != 0; j++) {
                    if ((entry & 1) != 0) {
                        relocs[relr_i++]->r_addr = where + j * 8;
                    }
                }
                where += 63 * 8;
            }
        }
    }

    if (rela_size != 0) {
        for (uint64_t i = relr_count, offset = 0; offset < rela_size; offset += rela_ent) {
            relocs[i++] = (void *)elf + (rela_offset + offset);
        }
    }

    if (dt_pltrelsz != 0) {
        for (uint64_t i = relr_count + rela_size / rela_ent, offset = 0; offset < dt_pltrelsz; offset += rela_ent) {
            relocs[i++] = (void *)elf + (dt_jmprel + offset);
        }
    }

    for (size_t i = 0; i < relocs_i; i++) {
        struct elf64_rela *relocation = relocs[i];

        // Relocation is before buffer
        if (relocation->r_addr < vaddr)
            continue;

        // Relocation is after buffer
        if (vaddr + size < relocation->r_addr + 8)
            continue;

        // It's inside it, calculate where it is
        uint64_t *ptr = (uint64_t *)((buffer - vaddr) + relocation->r_addr);

        switch (relocation->r_info) {
#if defined (__x86_64__) || defined (__i386__)
            case R_X86_64_NONE:
#elif defined (__aarch64__)
            case R_AARCH64_NONE:
#elif defined (__riscv)
            case R_RISCV_NONE:
#elif defined (__loongarch64)
            case R_LARCH_NONE:
#endif
            {
                break;
            }
#if defined (__x86_64__) || defined (__i386__)
            case R_X86_64_RELATIVE:
#elif defined (__aarch64__)
            case R_AARCH64_RELATIVE:
#elif defined (__riscv)
            case R_RISCV_RELATIVE:
#elif defined (__loongarch64)
            case R_LARCH_RELATIVE:
#endif
            {
                *ptr = slide + relocation->r_addend;
                break;
            }
            case R_INTERNAL_RELR:
            {
                *ptr += slide;
                break;
            }
#if defined (__x86_64__) || defined (__i386__)
            case R_X86_64_GLOB_DAT:
            case R_X86_64_JUMP_SLOT:
#elif defined (__aarch64__)
            case R_AARCH64_GLOB_DAT:
            case R_AARCH64_JUMP_SLOT:
#elif defined (__riscv)
            case R_RISCV_JUMP_SLOT:
#elif defined (__loongarch64)
            case R_LARCH_JUMP_SLOT:
#endif
            {
                struct elf64_sym *s = (void *)elf + symtab_offset + symtab_ent * relocation->r_symbol;
                if (s->st_shndx == SHN_UNDEF) {
                    if ((s->st_info >> 4) == STB_WEAK) {
                        *ptr = 0;
                        break;
                    }
                    panic(true, "elf: Unresolved symbol \"%s\"", elf + strtab_offset + s->st_name);
                }
                *ptr = slide + s->st_value
#if defined (__aarch64__)
                       + relocation->r_addend
#endif
                ;
                break;
            }
#if defined (__x86_64__) || defined (__i386__)
            case R_X86_64_64:
#elif defined (__aarch64__)
            case R_AARCH64_ABS64:
#elif defined (__riscv)
            case R_RISCV_64:
#elif defined (__loongarch64)
            case R_LARCH_64:
#endif
            {
                struct elf64_sym *s = (void *)elf + symtab_offset + symtab_ent * relocation->r_symbol;
                if (s->st_shndx == SHN_UNDEF) {
                    if ((s->st_info >> 4) == STB_WEAK) {
                        *ptr = 0;
                        break;
                    }
                    panic(true, "elf: Unresolved symbol \"%s\"", elf + strtab_offset + s->st_name);
                }
                *ptr = slide + s->st_value + relocation->r_addend;
                break;
            }
            default: {
                panic(true, "elf: Unknown relocation type: %x", relocation->r_info);
            }
        }
    }

    for (size_t i = 0; i < relr_count; i++) {
        pmm_free(relocs[i], sizeof(struct elf64_rela));
    }
    pmm_free(relocs, relocs_i * sizeof(struct elf64_rela *));

    return true;
}

bool elf64_load_section(uint8_t *elf, void *buffer, const char *name, size_t limit, uint64_t slide) {
    struct elf64_hdr *hdr = (void *)elf;

    elf64_validate(hdr);

    if (hdr->sh_num == 0) {
        return false;
    }

    if (hdr->shdr_size < sizeof(struct elf64_shdr)) {
        panic(true, "elf: shdr_size < sizeof(struct elf64_shdr)");
    }

    struct elf64_shdr *shstrtab = (void *)elf + (hdr->shoff + hdr->shstrndx * hdr->shdr_size);

    char *names = (void *)elf + shstrtab->sh_offset;

    for (uint16_t i = 0; i < hdr->sh_num; i++) {
        struct elf64_shdr *section = (void *)elf + (hdr->shoff + i * hdr->shdr_size);

        if (strcmp(&names[section->sh_name], name) == 0) {
            if (limit == 0) {
                *(void **)buffer = ext_mem_alloc(section->sh_size);
                buffer = *(void **)buffer;
                limit = section->sh_size;
            }
            if (section->sh_size > limit) {
                return false;
            }
            memcpy(buffer, elf + section->sh_offset, section->sh_size);
            return elf64_apply_relocations(elf, hdr, buffer, section->sh_addr, section->sh_size, slide);
        }
    }

    return false;
}

static uint64_t elf64_max_align(uint8_t *elf) {
    uint64_t ret = 0;

    struct elf64_hdr *hdr = (void *)elf;

    if (hdr->phdr_size < sizeof(struct elf64_phdr)) {
        panic(true, "elf: phdr_size < sizeof(struct elf64_phdr)");
    }

    for (uint16_t i = 0; i < hdr->ph_num; i++) {
        struct elf64_phdr *phdr = (void *)elf + (hdr->phoff + i * hdr->phdr_size);

        if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0) {
            continue;
        }

        if (phdr->p_align > ret) {
            ret = phdr->p_align;
        }
    }

    if (ret == 0) {
        panic(true, "elf: Executable has no loadable segments");
    }

    return ret;
}

static void elf64_get_ranges(uint8_t *elf, uint64_t slide, struct mem_range **_ranges, uint64_t *_ranges_count) {
    struct elf64_hdr *hdr = (void *)elf;

    uint64_t ranges_count = 0;

    if (hdr->phdr_size < sizeof(struct elf64_phdr)) {
        panic(true, "elf: phdr_size < sizeof(struct elf64_phdr)");
    }

    bool is_reloc = elf64_is_relocatable(elf, hdr);

    for (uint16_t i = 0; i < hdr->ph_num; i++) {
        struct elf64_phdr *phdr = (void *)elf + (hdr->phoff + i * hdr->phdr_size);

        if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0) {
            continue;
        }

        if (phdr->p_vaddr < FIXED_HIGHER_HALF_OFFSET_64) {
            if (!is_reloc) {
                continue;
            }
        }

        ranges_count++;
    }

    if (ranges_count == 0) {
        panic(true, "elf: No higher half PHDRs exist");
    }

    struct mem_range *ranges = ext_mem_alloc(ranges_count * sizeof(struct mem_range));

    size_t r = 0;
    for (uint16_t i = 0; i < hdr->ph_num; i++) {
        struct elf64_phdr *phdr = (void *)elf + (hdr->phoff + i * hdr->phdr_size);

        if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0) {
            continue;
        }

        if (phdr->p_vaddr < FIXED_HIGHER_HALF_OFFSET_64) {
            if (!is_reloc) {
                continue;
            }
        }

        uint64_t load_addr = phdr->p_vaddr + slide;
        uint64_t this_top = load_addr + phdr->p_memsz;

        ranges[r].base = load_addr & ~(phdr->p_align - 1);
        ranges[r].length = ALIGN_UP(this_top - ranges[r].base, phdr->p_align);

        if (phdr->p_flags & ELF_PF_X) {
            ranges[r].permissions |= MEM_RANGE_X;
        }

        if (phdr->p_flags & ELF_PF_W) {
            ranges[r].permissions |= MEM_RANGE_W;
        }

        if (phdr->p_flags & ELF_PF_R) {
            ranges[r].permissions |= MEM_RANGE_R;
        }

        r++;
    }

    *_ranges_count = ranges_count;
    *_ranges = ranges;
}

bool elf64_load(uint8_t *elf, uint64_t *entry_point, uint64_t *_slide, uint32_t alloc_type, bool kaslr, struct mem_range **ranges, uint64_t *ranges_count, uint64_t *physical_base, uint64_t *virtual_base, uint64_t *_image_size, uint64_t *_image_size_before_bss, bool *is_reloc) {
    struct elf64_hdr *hdr = (void *)elf;

    elf64_validate(hdr);

    if (hdr->type != ET_EXEC && hdr->type != ET_DYN) {
        panic(true, "elf: ELF file not of type ET_EXEC nor ET_DYN");
    }

    if (is_reloc) {
        *is_reloc = false;
    }
    if (elf64_is_relocatable(elf, hdr)) {
        if (is_reloc) {
            *is_reloc = true;
        }
    }

    uint64_t slide = 0;
    size_t try_count = 0;
    size_t max_simulated_tries = 0x10000;

    uint64_t entry = hdr->entry;

    uint64_t max_align = elf64_max_align(elf);

    uint64_t image_size = 0;

    if (hdr->phdr_size < sizeof(struct elf64_phdr)) {
        panic(true, "elf: phdr_size < sizeof(struct elf64_phdr)");
    }

    bool lower_to_higher = false;

    uint64_t min_vaddr = (uint64_t)-1;
    uint64_t max_vaddr = 0;
    for (uint16_t i = 0; i < hdr->ph_num; i++) {
        struct elf64_phdr *phdr = (void *)elf + (hdr->phoff + i * hdr->phdr_size);

        if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0) {
            continue;
        }

        if (phdr->p_vaddr < FIXED_HIGHER_HALF_OFFSET_64) {
            if (!*is_reloc) {
                panic(true, "elf: Lower half PHDRs are not allowed");
            }
            lower_to_higher = true;
        } else {
            if (lower_to_higher) {
                panic(true, "elf: Mix of lower and higher half PHDRs in relocatable kernel");
            }
        }

        // check for overlapping phdrs
        for (uint16_t j = 0; j < hdr->ph_num; j++) {
            struct elf64_phdr *phdr_in = (void *)elf + (hdr->phoff + j * hdr->phdr_size);

            if (phdr_in->p_type != PT_LOAD || phdr_in->p_memsz == 0) {
                continue;
            }

            if (phdr_in->p_vaddr < FIXED_HIGHER_HALF_OFFSET_64) {
                if (!*is_reloc) {
                    continue;
                }
            }

            if (phdr_in == phdr) {
                continue;
            }

            if ((phdr_in->p_vaddr >= phdr->p_vaddr
              && phdr_in->p_vaddr < phdr->p_vaddr + phdr->p_memsz)
                ||
                (phdr_in->p_vaddr + phdr_in->p_memsz > phdr->p_vaddr
              && phdr_in->p_vaddr + phdr_in->p_memsz <= phdr->p_vaddr + phdr->p_memsz)) {
                panic(true, "elf: Attempted to load ELF file with overlapping PHDRs (%u and %u overlap)", i, j);
            }

            if (ranges != NULL) {
                uint64_t page_rounded_base = ALIGN_DOWN(phdr->p_vaddr, 4096);
                uint64_t page_rounded_top = ALIGN_UP(phdr->p_vaddr + phdr->p_memsz, 4096);
                uint64_t page_rounded_base_in = ALIGN_DOWN(phdr_in->p_vaddr, 4096);
                uint64_t page_rounded_top_in = ALIGN_UP(phdr_in->p_vaddr + phdr_in->p_memsz, 4096);

                if ((page_rounded_base >= page_rounded_base_in
                  && page_rounded_base < page_rounded_top_in)
                   ||
                    (page_rounded_top > page_rounded_base_in
                  && page_rounded_top <= page_rounded_top_in)) {
                    if ((phdr->p_flags & 0b111) != (phdr_in->p_flags & 0b111)) {
                        panic(true, "elf: Attempted to load ELF file with PHDRs with different permissions sharing the same memory page.");
                    }
                }
            }
        }

        if (phdr->p_vaddr < min_vaddr) {
            min_vaddr = phdr->p_vaddr;
        }

        if (phdr->p_vaddr + phdr->p_memsz > max_vaddr) {
            max_vaddr = phdr->p_vaddr + phdr->p_memsz;
        }
    }

    if (min_vaddr == (uint64_t)-1) {
        panic(true, "elf: No usable PHDRs exist");
    }

    if (lower_to_higher) {
        slide = FIXED_HIGHER_HALF_OFFSET_64 - min_vaddr;
    }

    image_size = max_vaddr - min_vaddr;

    *physical_base = (uintptr_t)ext_mem_alloc_type_aligned(image_size, alloc_type, max_align);
    *virtual_base = min_vaddr;

    if (_image_size) {
        *_image_size = image_size;
    }

again:
    if (*is_reloc && kaslr) {
        slide = (rand32() & ~(max_align - 1)) + (lower_to_higher ? FIXED_HIGHER_HALF_OFFSET_64 - min_vaddr : 0);

        if (*virtual_base + slide + image_size < 0xffffffff80000000 /* this comparison relies on overflow */) {
            if (++try_count == max_simulated_tries) {
                panic(true, "elf: Image wants to load too high");
            }
            goto again;
        }
    }

    uint64_t bss_size = 0;

    for (uint16_t i = 0; i < hdr->ph_num; i++) {
        struct elf64_phdr *phdr = (void *)elf + (hdr->phoff + i * hdr->phdr_size);

        if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0) {
            continue;
        }

        // Sanity checks
        if (phdr->p_filesz > phdr->p_memsz) {
            panic(true, "elf: p_filesz > p_memsz");
        }

        uint64_t load_addr = *physical_base + (phdr->p_vaddr - *virtual_base);

#if defined (__aarch64__)
        uint64_t this_top = load_addr + phdr->p_memsz;

        uint64_t mem_base, mem_size;

        mem_base = load_addr & ~(phdr->p_align - 1);
        mem_size = this_top - mem_base;
#endif

        memcpy((void *)(uintptr_t)load_addr, elf + (phdr->p_offset), phdr->p_filesz);

        if (phdr->p_vaddr + phdr->p_memsz == max_vaddr) {
            bss_size = phdr->p_memsz - phdr->p_filesz;
        }

        if (!elf64_apply_relocations(elf, hdr, (void *)(uintptr_t)load_addr, phdr->p_vaddr, phdr->p_memsz, slide)) {
            panic(true, "elf: Failed to apply relocations");
        }

#if defined (__aarch64__)
        clean_dcache_poc(mem_base, mem_base + mem_size);
        inval_icache_pou(mem_base, mem_base + mem_size);
#endif
    }

    if (_image_size_before_bss != NULL) {
        *_image_size_before_bss = image_size - bss_size;
    }

    *virtual_base += slide;
    *entry_point = entry + slide;
    if (_slide) {
        *_slide = slide;
    }

    if (ranges_count != NULL && ranges != NULL) {
        elf64_get_ranges(elf, slide, ranges, ranges_count);
    }

    return true;
}

bool elf32_load_elsewhere(uint8_t *elf, uint64_t *entry_point,
                          struct elsewhere_range **ranges) {
    struct elf32_hdr *hdr = (void *)elf;

    elf32_validate(hdr);

    if (hdr->type != ET_EXEC && hdr->type != ET_DYN) {
        panic(true, "elf: ELF file not of type ET_EXEC nor ET_DYN");
    }

    *entry_point = hdr->entry;
    bool entry_adjusted = false;

    if (hdr->phdr_size < sizeof(struct elf32_phdr)) {
        panic(true, "elf: phdr_size < sizeof(struct elf32_phdr)");
    }

    size_t image_size = 0;
    uint64_t min_paddr = (uint64_t)-1;
    uint64_t max_paddr = 0;
    for (uint16_t i = 0; i < hdr->ph_num; i++) {
        struct elf32_phdr *phdr = (void *)elf + (hdr->phoff + i * hdr->phdr_size);

        if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0)
            continue;

        if (phdr->p_paddr < min_paddr) {
            min_paddr = phdr->p_paddr;
        }

        if (phdr->p_paddr + phdr->p_memsz > max_paddr) {
            max_paddr = phdr->p_paddr + phdr->p_memsz;
        }
    }
    image_size = max_paddr - min_paddr;

    void *elsewhere = ext_mem_alloc(image_size);

    *ranges = ext_mem_alloc(sizeof(struct elsewhere_range));

    (*ranges)->elsewhere = (uintptr_t)elsewhere;
    (*ranges)->target = min_paddr;
    (*ranges)->length = image_size;

    for (uint16_t i = 0; i < hdr->ph_num; i++) {
        struct elf32_phdr *phdr = (void *)elf + (hdr->phoff + i * hdr->phdr_size);

        if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0)
            continue;

        // Sanity checks
        if (phdr->p_filesz > phdr->p_memsz) {
            panic(true, "elf: p_filesz > p_memsz");
        }

        memcpy(elsewhere + (phdr->p_paddr - min_paddr), elf + phdr->p_offset, phdr->p_filesz);

        if (!entry_adjusted
         && *entry_point >= phdr->p_vaddr
         && *entry_point < (phdr->p_vaddr + phdr->p_memsz)) {
            *entry_point -= phdr->p_vaddr;
            *entry_point += phdr->p_paddr;
            entry_adjusted = true;
        }
    }

    return true;
}

bool elf64_load_elsewhere(uint8_t *elf, uint64_t *entry_point,
                          struct elsewhere_range **ranges) {
    struct elf64_hdr *hdr = (void *)elf;

    elf64_validate(hdr);

    if (hdr->type != ET_EXEC && hdr->type != ET_DYN) {
        panic(true, "elf: ELF file not of type ET_EXEC nor ET_DYN");
    }

    *entry_point = hdr->entry;
    bool entry_adjusted = false;

    if (hdr->phdr_size < sizeof(struct elf64_phdr)) {
        panic(true, "elf: phdr_size < sizeof(struct elf64_phdr)");
    }

    size_t image_size = 0;
    uint64_t min_paddr = (uint64_t)-1;
    uint64_t max_paddr = 0;
    for (uint16_t i = 0; i < hdr->ph_num; i++) {
        struct elf64_phdr *phdr = (void *)elf + (hdr->phoff + i * hdr->phdr_size);

        if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0)
            continue;

        if (phdr->p_paddr < min_paddr) {
            min_paddr = phdr->p_paddr;
        }

        if (phdr->p_paddr + phdr->p_memsz > max_paddr) {
            max_paddr = phdr->p_paddr + phdr->p_memsz;
        }
    }
    image_size = max_paddr - min_paddr;

    void *elsewhere = ext_mem_alloc(image_size);

    *ranges = ext_mem_alloc(sizeof(struct elsewhere_range));

    (*ranges)->elsewhere = (uintptr_t)elsewhere;
    (*ranges)->target = min_paddr;
    (*ranges)->length = image_size;

    for (uint16_t i = 0; i < hdr->ph_num; i++) {
        struct elf64_phdr *phdr = (void *)elf + (hdr->phoff + i * hdr->phdr_size);

        if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0)
            continue;

        // Sanity checks
        if (phdr->p_filesz > phdr->p_memsz) {
            panic(true, "elf: p_filesz > p_memsz");
        }

        memcpy(elsewhere + (phdr->p_paddr - min_paddr), elf + phdr->p_offset, phdr->p_filesz);

        if (!entry_adjusted
         && *entry_point >= phdr->p_vaddr
         && *entry_point < (phdr->p_vaddr + phdr->p_memsz)) {
            *entry_point -= phdr->p_vaddr;
            *entry_point += phdr->p_paddr;
            entry_adjusted = true;
        }
    }

    return true;
}
