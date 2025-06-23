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
extern unsigned long __kexec_array_start[], __kexec_array_end[];

typedef int(*kexeccall_t)(bool undo);
#define kexec_call(func)                                                   \
    static kexeccall_t __kexeccall_##func __attribute__((__used__))        \
                       __attribute__((__section__(".kexec_array"))) = func

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

/* Finalize parameter location and size. */
void kexec_set_param_loc(const char *cmdline);

/* Get entry point and parameter of new kernel. */
int kexec_get_entry(const char *cmdline);
void kexec_get_entry_undo(void);

/* Move used pages away from new kernel area. */
int kexec_move_used_pages(unsigned long boundary, unsigned long kernel,
                          unsigned long kernel_size);
void kexec_move_used_pages_undo(void);

#endif /* _KEXEC_H */
