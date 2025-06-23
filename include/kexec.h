#ifndef _KEXEC_H
#define _KEXEC_H

int kexec(void *kernel, unsigned long kernel_size,
          const char *cmdline);

#endif /* _KEXEC_H */
