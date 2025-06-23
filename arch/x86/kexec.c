/******************************************************************************
 * kexec.c
 *
 * Support of kexec (reboot locally into new mini-os kernel).
 *
 * Copyright (c) 2024, Juergen Gross, SUSE Linux GmbH
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifdef CONFIG_KEXEC

#include <mini-os/os.h>
#include <mini-os/lib.h>
#include <mini-os/kexec.h>

/*
 * Final stage of kexec. Copies all data to the final destinations, zeroes
 * .bss and activates new kernel.
 * Must be called with interrupts off. Stack, code and data must be
 * accessible via identity mapped virtual addresses (virt == phys). Copying
 * and zeroing is done using virtual addresses.
 * No relocations inside the function are allowed, as it is copied to an
 * allocated page before being executed.
 */
static void __attribute__((__section__(".text.kexec")))
    kexec_final(struct kexec_action *actions, unsigned long real)
{
    char *src, *dest;
    unsigned int a, cnt;

    for ( a = 0; ; a++ )
    {
        switch ( actions[a].action )
        {
        case KEXEC_COPY:
            dest = actions[a].dest;
            src = actions[a].src;
            for ( cnt = 0; cnt < actions[a].len; cnt++ )
                *dest++ = *src++;
            break;

        case KEXEC_ZERO:
            dest = actions[a].dest;
            for ( cnt = 0; cnt < actions[a].len; cnt++ )
                *dest++ = 0;
            break;

        case KEXEC_CALL:
            asm("movl %0, %%ebx\n\t"
                "movl %1, %%edi\n\t"
                "jmp *%2"
                : :"m" (actions[a].src), "m" (actions[a].dest), "m" (real));
            break;
        }
    }
}

#define KEXEC_STACK_LONGS  8
static unsigned long __attribute__((__section__(".data.kexec")))
    kexec_stack[KEXEC_STACK_LONGS];

static unsigned long get_kexec_addr(void *kexec_page, void *addr)
{
    unsigned long off = (unsigned long)addr - (unsigned long)_kexec_start;

    return (unsigned long)kexec_page + off;
}

void do_kexec(void *kexec_page)
{
    unsigned long actions;
    unsigned long stack;
    unsigned long final;
    unsigned long phys;

    actions = get_kexec_addr(kexec_page, kexec_actions);
    stack = get_kexec_addr(kexec_page, kexec_stack + KEXEC_STACK_LONGS);
    final = get_kexec_addr(kexec_page, kexec_final);
    phys = get_kexec_addr(kexec_page, kexec_phys);

    memcpy(kexec_page, _kexec_start, KEXEC_SECSIZE);
    asm("cli\n\t"
        "mov %0, %%"ASM_SP"\n\t"
        "mov %1, %%"ASM_ARG1"\n\t"
        "mov %2, %%"ASM_ARG2"\n\t"
        "jmp *%3"
        : :"m" (stack), "m" (actions), "m" (phys), "m" (final));
}

#endif /* CONFIG_KEXEC */
