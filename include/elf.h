#ifndef __ELF_H__
#define __ELF_H__
/*
 * Copyright (c) 1995, 1996 Erik Theisen.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdbool.h>
#include <mini-os/types.h>

typedef uint32_t    Elf32_Addr;  /* Unsigned program address */
typedef uint32_t    Elf32_Off;   /* Unsigned file offset */
typedef uint16_t    Elf32_Half;  /* Unsigned medium integer */
typedef uint32_t    Elf32_Word;  /* Unsigned large integer */

typedef uint64_t    Elf64_Addr;
typedef uint64_t    Elf64_Off;
typedef uint16_t    Elf64_Half;
typedef uint32_t    Elf64_Word;
typedef uint64_t    Elf64_Xword;

/* Unique build id string format when using --build-id. */
#define NT_GNU_BUILD_ID 3

/*
 * e_ident[] identification indexes
 * See http://www.caldera.com/developers/gabi/2000-07-17/ch4.eheader.html
 */
#define EI_MAG0        0         /* file ID */
#define EI_MAG1        1         /* file ID */
#define EI_MAG2        2         /* file ID */
#define EI_MAG3        3         /* file ID */
#define EI_CLASS       4         /* file class */
#define EI_DATA        5         /* data encoding */
#define EI_VERSION     6         /* ELF header version */
#define EI_OSABI       7         /* OS/ABI ID */
#define EI_ABIVERSION  8         /* ABI version */
#define EI_PAD         9         /* start of pad bytes */
#define EI_NIDENT     16         /* Size of e_ident[] */

/* e_ident[] magic number */
#define ELFMAG0        0x7f      /* e_ident[EI_MAG0] */
#define ELFMAG1        'E'       /* e_ident[EI_MAG1] */
#define ELFMAG2        'L'       /* e_ident[EI_MAG2] */
#define ELFMAG3        'F'       /* e_ident[EI_MAG3] */
#define ELFMAG         "\177ELF" /* magic */
#define SELFMAG        4         /* size of magic */

/* e_ident[] file class */
#define ELFCLASSNONE   0         /* invalid */
#define ELFCLASS32     1         /* 32-bit objs */
#define ELFCLASS64     2         /* 64-bit objs */
#define ELFCLASSNUM    3         /* number of classes */

/* e_ident[] data encoding */
#define ELFDATANONE    0         /* invalid */
#define ELFDATA2LSB    1         /* Little-Endian */
#define ELFDATA2MSB    2         /* Big-Endian */
#define ELFDATANUM     3         /* number of data encode defines */

/* e_ident */
#define IS_ELF(ehdr) ((ehdr).e_ident[EI_MAG0] == ELFMAG0 && \
                      (ehdr).e_ident[EI_MAG1] == ELFMAG1 && \
                      (ehdr).e_ident[EI_MAG2] == ELFMAG2 && \
                      (ehdr).e_ident[EI_MAG3] == ELFMAG3)

/* e_flags */
#define EF_ARM_EABI_MASK    0xff000000
#define EF_ARM_EABI_UNKNOWN 0x00000000
#define EF_ARM_EABI_VER1    0x01000000
#define EF_ARM_EABI_VER2    0x02000000
#define EF_ARM_EABI_VER3    0x03000000
#define EF_ARM_EABI_VER4    0x04000000
#define EF_ARM_EABI_VER5    0x05000000

/* ELF Header */
typedef struct {
    unsigned char e_ident[EI_NIDENT]; /* ELF Identification */
    Elf32_Half    e_type;        /* object file type */
    Elf32_Half    e_machine;     /* machine */
    Elf32_Word    e_version;     /* object file version */
    Elf32_Addr    e_entry;       /* virtual entry point */
    Elf32_Off     e_phoff;       /* program header table offset */
    Elf32_Off     e_shoff;       /* section header table offset */
    Elf32_Word    e_flags;       /* processor-specific flags */
    Elf32_Half    e_ehsize;      /* ELF header size */
    Elf32_Half    e_phentsize;   /* program header entry size */
    Elf32_Half    e_phnum;       /* number of program header entries */
    Elf32_Half    e_shentsize;   /* section header entry size */
    Elf32_Half    e_shnum;       /* number of section header entries */
    Elf32_Half    e_shstrndx;    /* section header table's "section
                                    header string table" entry offset */
} Elf32_Ehdr;

typedef struct {
    unsigned char e_ident[EI_NIDENT]; /* Id bytes */
    Elf64_Half    e_type;        /* file type */
    Elf64_Half    e_machine;     /* machine type */
    Elf64_Word    e_version;     /* version number */
    Elf64_Addr    e_entry;       /* entry point */
    Elf64_Off     e_phoff;       /* Program hdr offset */
    Elf64_Off     e_shoff;       /* Section hdr offset */
    Elf64_Word    e_flags;       /* Processor flags */
    Elf64_Half    e_ehsize;      /* sizeof ehdr */
    Elf64_Half    e_phentsize;   /* Program header entry size */
    Elf64_Half    e_phnum;       /* Number of program headers */
    Elf64_Half    e_shentsize;   /* Section header entry size */
    Elf64_Half    e_shnum;       /* Number of section headers */
    Elf64_Half    e_shstrndx;    /* String table index */
} Elf64_Ehdr;

/* e_type */
#define ET_NONE      0           /* No file type */
#define ET_REL       1           /* relocatable file */
#define ET_EXEC      2           /* executable file */
#define ET_DYN       3           /* shared object file */
#define ET_CORE      4           /* core file */
#define ET_NUM       5           /* number of types */
#define ET_LOPROC    0xff00      /* reserved range for processor */
#define ET_HIPROC    0xffff      /*   specific e_type */

/* e_machine */
#define EM_NONE         0        /* No Machine */
#define EM_386          3        /* Intel 80386 */
#define EM_PPC64       21        /* PowerPC 64-bit */
#define EM_ARM         40        /* Advanced RISC Machines ARM */
#define EM_X86_64      62        /* AMD x86-64 architecture */
#define EM_AARCH64    183        /* ARM 64-bit */

/* Version */
#define EV_NONE      0           /* Invalid */
#define EV_CURRENT   1           /* Current */
#define EV_NUM       2           /* number of versions */

/* Program Header */
typedef struct {
    Elf32_Word    p_type;        /* segment type */
    Elf32_Off     p_offset;      /* segment offset */
    Elf32_Addr    p_vaddr;       /* virtual address of segment */
    Elf32_Addr    p_paddr;       /* physical address - ignored? */
    Elf32_Word    p_filesz;      /* number of bytes in file for seg. */
    Elf32_Word    p_memsz;       /* number of bytes in mem. for seg. */
    Elf32_Word    p_flags;       /* flags */
    Elf32_Word    p_align;       /* memory alignment */
} Elf32_Phdr;

typedef struct {
    Elf64_Word    p_type;        /* entry type */
    Elf64_Word    p_flags;       /* flags */
    Elf64_Off     p_offset;      /* offset */
    Elf64_Addr    p_vaddr;       /* virtual address */
    Elf64_Addr    p_paddr;       /* physical address */
    Elf64_Xword   p_filesz;      /* file size */
    Elf64_Xword   p_memsz;       /* memory size */
    Elf64_Xword   p_align;       /* memory & file alignment */
} Elf64_Phdr;

/* Segment types - p_type */
#define PT_NULL      0           /* unused */
#define PT_LOAD      1           /* loadable segment */
#define PT_DYNAMIC   2           /* dynamic linking section */
#define PT_INTERP    3           /* the RTLD */
#define PT_NOTE      4           /* auxiliary information */
#define PT_SHLIB     5           /* reserved - purpose undefined */
#define PT_PHDR      6           /* program header */
#define PT_NUM       7           /* Number of segment types */
#define PT_LOPROC    0x70000000  /* reserved range for processor */
#define PT_HIPROC    0x7fffffff  /*  specific segment types */

/* Segment flags - p_flags */
#define PF_X         0x1        /* Executable */
#define PF_W         0x2        /* Writable */
#define PF_R         0x4        /* Readable */
#define PF_MASKPROC  0xf0000000 /* reserved bits for processor */
                                /*  specific segment flags */

/* Section Header */
typedef struct {
    Elf32_Word    sh_name;      /* name - index into section header
                                   string table section */
    Elf32_Word    sh_type;      /* type */
    Elf32_Word    sh_flags;     /* flags */
    Elf32_Addr    sh_addr;      /* address */
    Elf32_Off     sh_offset;    /* file offset */
    Elf32_Word    sh_size;      /* section size */
    Elf32_Word    sh_link;      /* section header table index link */
    Elf32_Word    sh_info;      /* extra information */
    Elf32_Word    sh_addralign; /* address alignment */
    Elf32_Word    sh_entsize;   /* section entry size */
} Elf32_Shdr;

typedef struct {
    Elf64_Word    sh_name;      /* section name */
    Elf64_Word    sh_type;      /* section type */
    Elf64_Xword   sh_flags;     /* section flags */
    Elf64_Addr    sh_addr;      /* virtual address */
    Elf64_Off     sh_offset;    /* file offset */
    Elf64_Xword   sh_size;      /* section size */
    Elf64_Word    sh_link;      /* link to another */
    Elf64_Word    sh_info;      /* misc info */
    Elf64_Xword   sh_addralign; /* memory alignment */
    Elf64_Xword   sh_entsize;   /* table entry size */
} Elf64_Shdr;

/* sh_type */
#define SHT_NULL        0       /* inactive */
#define SHT_PROGBITS    1       /* program defined information */
#define SHT_SYMTAB      2       /* symbol table section */
#define SHT_STRTAB      3       /* string table section */
#define SHT_RELA        4       /* relocation section with addends*/
#define SHT_HASH        5       /* symbol hash table section */
#define SHT_DYNAMIC     6       /* dynamic section */
#define SHT_NOTE        7       /* note section */
#define SHT_NOBITS      8       /* no space section */
#define SHT_REL         9       /* relation section without addends */
#define SHT_SHLIB      10       /* reserved - purpose unknown */
#define SHT_DYNSYM     11       /* dynamic symbol table section */
#define SHT_NUM        12       /* number of section types */

/* Note definitions */
typedef struct {
    Elf32_Word namesz;
    Elf32_Word descsz;
    Elf32_Word type;
    char data[];
} Elf32_Note;

typedef struct {
    Elf64_Word namesz;
    Elf64_Word descsz;
    Elf64_Word type;
    char data[];
} Elf64_Note;

/* Abstraction layer for handling 32- and 64-bit ELF files. */

typedef union {
    Elf32_Ehdr e32;
    Elf64_Ehdr e64;
} elf_ehdr;

static inline bool elf_is_32bit(elf_ehdr *ehdr)
{
    return ehdr->e32.e_ident[EI_CLASS] == ELFCLASS32;
}

static inline bool elf_is_64bit(elf_ehdr *ehdr)
{
    return ehdr->e32.e_ident[EI_CLASS] == ELFCLASS64;
}

#define ehdr_val(ehdr, elem) (elf_is_32bit(ehdr) ? (ehdr)->e32.elem : (ehdr)->e64.elem)

typedef union {
    Elf32_Phdr e32;
    Elf64_Phdr e64;
} elf_phdr;

#define phdr_val(ehdr, phdr, elem) (elf_is_32bit(ehdr) ? (phdr)->e32.elem : (phdr)->e64.elem)

typedef union {
    Elf32_Shdr e32;
    Elf64_Shdr e64;
} elf_shdr;

#define shdr_val(ehdr, shdr, elem) (elf_is_32bit(ehdr) ? (shdr)->e32.elem : (shdr)->e64.elem)

typedef union {
    Elf32_Note e32;
    Elf64_Note e64;
} elf_note;

#define note_val(ehdr, note, elem) (elf_is_32bit(ehdr) ? (note)->e32.elem : (note)->e64.elem)

static inline void *elf_ptr_add(void *ptr, unsigned long add)
{
    return ptr + add;
}
#endif /* __ELF_H__ */
