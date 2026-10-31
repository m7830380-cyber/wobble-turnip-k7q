// Minimal ELF64 definitions for hosts without <elf.h> (Darwin/iOS)
#pragma once
#ifdef __APPLE__
#include <stdint.h>
typedef uint64_t Elf64_Addr, Elf64_Off, Elf64_Xword;
typedef int64_t Elf64_Sxword;
typedef uint32_t Elf64_Word;
typedef uint16_t Elf64_Half, Elf64_Section;
typedef struct { unsigned char e_ident[16]; Elf64_Half e_type, e_machine; Elf64_Word e_version; Elf64_Addr e_entry; Elf64_Off e_phoff, e_shoff; Elf64_Word e_flags; Elf64_Half e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } Elf64_Ehdr;
typedef struct { Elf64_Word p_type, p_flags; Elf64_Off p_offset; Elf64_Addr p_vaddr, p_paddr; Elf64_Xword p_filesz, p_memsz, p_align; } Elf64_Phdr;
typedef struct { Elf64_Sxword d_tag; union { Elf64_Xword d_val; Elf64_Addr d_ptr; } d_un; } Elf64_Dyn;
typedef struct { Elf64_Word st_name; unsigned char st_info, st_other; Elf64_Section st_shndx; Elf64_Addr st_value; Elf64_Xword st_size; } Elf64_Sym;
typedef struct { Elf64_Addr r_offset; Elf64_Xword r_info; Elf64_Sxword r_addend; } Elf64_Rela;
#define ELF64_R_SYM(i) ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffff)
#define ELF64_ST_BIND(i) ((i) >> 4)
#define ELF64_ST_TYPE(i) ((i) & 0xf)
#define ELFMAG "\177ELF"
#define SELFMAG 4
#define EM_AARCH64 183
#define SHN_UNDEF 0
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2
#define PT_NULL 0
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_TLS 7
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_STRSZ 10
#define DT_INIT 12
#define DT_JMPREL 23
#define DT_INIT_ARRAY 25
#define DT_INIT_ARRAYSZ 27
#define DT_GNU_HASH 0x6ffffef5
struct dl_phdr_info { Elf64_Addr dlpi_addr; const char *dlpi_name; const Elf64_Phdr *dlpi_phdr; Elf64_Half dlpi_phnum; unsigned long long dlpi_adds, dlpi_subs; size_t dlpi_tls_modid; void *dlpi_tls_data; };
#else
#include <elf.h>
#endif
