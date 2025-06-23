#ifndef _KEXEC_H
#define _KEXEC_H

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

int kexec_add_action(int action, void *dest, void *src, unsigned int len);

#define KEXEC_SECSIZE ((unsigned long)_kexec_end - (unsigned long)_kexec_start)

int kexec(void *kernel, unsigned long kernel_size,
          const char *cmdline);

/* Initiate final kexec stage. */
void do_kexec(void *kexec_page);

/* Assembler code for switching off paging and passing execution to new OS. */
void kexec_phys(void);

#endif /* _KEXEC_H */
