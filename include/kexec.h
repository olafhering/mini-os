#ifndef _KEXEC_H
#define _KEXEC_H
#include <mini-os/elf.h>

/* One element of kexec actions (last element must have action KEXEC_CALL): */
struct kexec_action {
    enum {
        KEXEC_COPY,   /* Copy len bytes from src to dest. */
        KEXEC_ZERO,   /* Zero len bytes at dest. */
        KEXEC_CALL    /* Call dest with paging turned off, param is src. */
    } action;
    unsigned int len;
    void *dest;
    void *src;
};

#define KEXEC_MAX_ACTIONS  16

extern char _kexec_start[], _kexec_end[];
extern struct kexec_action kexec_actions[KEXEC_MAX_ACTIONS];

extern unsigned long kexec_last_addr;

int kexec_add_action(int action, void *dest, void *src, unsigned int len);

#define KEXEC_SECSIZE ((unsigned long)_kexec_end - (unsigned long)_kexec_start)

int kexec(void *kernel, unsigned long kernel_size,
          const char *cmdline);

/* Initiate final kexec stage. */
void do_kexec(void *kexec_page);

/* Assembler code for switching off paging and passing execution to new OS. */
void kexec_phys(void);

/* Check kernel to match current architecture. */
bool kexec_chk_arch(elf_ehdr *ehdr);

/* Architecture specific ELF handling functions. */
int kexec_arch_analyze_phdr(elf_ehdr *ehdr, elf_phdr *phdr);
int kexec_arch_analyze_shdr(elf_ehdr *ehdr, elf_shdr *shdr);
bool kexec_arch_need_analyze_shdrs(void);

#endif /* _KEXEC_H */
