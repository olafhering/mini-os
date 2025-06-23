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
#include <mini-os/e820.h>
#include <mini-os/err.h>
#include <mini-os/kexec.h>

#include <xen/elfnote.h>
#include <xen/arch-x86/hvm/start_info.h>

static unsigned long kernel_phys_entry = ~0UL;

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

bool kexec_chk_arch(elf_ehdr *ehdr)
{
    return ehdr->e32.e_machine == EM_386 || ehdr->e32.e_machine == EM_X86_64;
}

static unsigned int note_data_sz(unsigned int sz)
{
    return (sz + 3) & ~3;
}

static void read_note_entry(elf_ehdr *ehdr, void *start, unsigned int len)
{
    elf_note *note = start;
    unsigned int off, note_len, namesz, descsz;
    char *val;

    for ( off = 0; off < len; off += note_len )
    {
        namesz = note_data_sz(note_val(ehdr, note, namesz));
        descsz = note_data_sz(note_val(ehdr, note, descsz));
        val = note_val(ehdr, note, data);
        note_len = val - (char *)note + namesz + descsz;

        if ( !strncmp(val, "Xen", namesz) &&
             note_val(ehdr, note, type) == XEN_ELFNOTE_PHYS32_ENTRY )
        {
            val += namesz;
            switch ( note_val(ehdr, note, descsz) )
            {
            case 1:
                kernel_phys_entry = *(uint8_t *)val;
                return;
            case 2:
                kernel_phys_entry = *(uint16_t *)val;
                return;
            case 4:
                kernel_phys_entry = *(uint32_t *)val;
                return;
            case 8:
                kernel_phys_entry = *(uint64_t *)val;
                return;
            default:
                break;
            }
        }

        note = elf_ptr_add(note, note_len);
    }
}

int kexec_arch_analyze_phdr(elf_ehdr *ehdr, elf_phdr *phdr)
{
    void *notes_start;
    unsigned int notes_len;

    if ( phdr_val(ehdr, phdr, p_type) != PT_NOTE || kernel_phys_entry != ~0UL )
        return 0;

    notes_start = elf_ptr_add(ehdr, phdr_val(ehdr, phdr, p_offset));
    notes_len = phdr_val(ehdr, phdr, p_filesz);
    read_note_entry(ehdr, notes_start, notes_len);

    return 0;
}

int kexec_arch_analyze_shdr(elf_ehdr *ehdr, elf_shdr *shdr)
{
    void *notes_start;
    unsigned int notes_len;

    if ( shdr_val(ehdr, shdr, sh_type) != SHT_NOTE ||
         kernel_phys_entry != ~0UL )
        return 0;

    notes_start = elf_ptr_add(ehdr, shdr_val(ehdr, shdr, sh_offset));
    notes_len = shdr_val(ehdr, shdr, sh_size);
    read_note_entry(ehdr, notes_start, notes_len);

    return 0;
}

bool kexec_arch_need_analyze_shdrs(void)
{
    return kernel_phys_entry == ~0UL;
}

static unsigned long kexec_param_loc;
static unsigned int kexec_param_size;

void kexec_set_param_loc(const char *cmdline)
{
    kexec_param_size = sizeof(struct hvm_start_info);
    kexec_param_size += e820_entries * sizeof(struct hvm_memmap_table_entry);
    kexec_param_size += strlen(cmdline) + 1;

    kexec_last_addr = (kexec_last_addr + 7) & ~7UL;
    kexec_param_loc = kexec_last_addr;
    kexec_last_addr += kexec_param_size;
    kexec_last_addr = round_pgup(kexec_last_addr);
}
#endif /* CONFIG_KEXEC */
