#ifndef LIB__ELF_H__
#define LIB__ELF_H__

#include <stdint.h>
#include <stdbool.h>
#include <lib/elsewhere.h>
#include <lib/misc.h>

#define FIXED_HIGHER_HALF_OFFSET_64 ((uint64_t)0xffffffff80000000)

#define ELF_PF_X 1
#define ELF_PF_W 2
#define ELF_PF_R 4

struct elf_section_hdr_info {
    uint32_t section_entry_size;
    uint32_t str_section_idx;
    uint32_t num;
    uint32_t section_offset;
};

int elf_bits(uint8_t *elf);

struct elf_section_hdr_info elf64_section_hdr_info(uint8_t *elf);
struct elf_section_hdr_info elf32_section_hdr_info(uint8_t *elf);

bool elf64_load_section(uint8_t *elf, void *buffer, const char *name, size_t limit, uint64_t slide);
bool elf64_load(uint8_t *elf, uint64_t *entry_point, uint64_t *_slide, uint32_t alloc_type, bool kaslr, struct mem_range **ranges, uint64_t *ranges_count, uint64_t *physical_base, uint64_t *virtual_base, uint64_t *image_size, uint64_t *image_size_before_bss, bool *is_reloc);

bool elf32_load_elsewhere(uint8_t *elf, uint64_t *entry_point,
                          struct elsewhere_range **ranges);
bool elf64_load_elsewhere(uint8_t *elf, uint64_t *entry_point,
                          struct elsewhere_range **ranges);

struct elf64_hdr {
    uint8_t  ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t hdr_size;
    uint16_t phdr_size;
    uint16_t ph_num;
    uint16_t shdr_size;
    uint16_t sh_num;
    uint16_t shstrndx;
};

struct elf64_shdr {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
};

struct elf32_shdr {
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
};

struct elf64_sym {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
};

#endif
