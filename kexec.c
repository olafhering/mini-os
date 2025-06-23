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

#ifdef CONFIG_PARAVIRT
#error "kexec support not implemented in PV variant"
#endif

#include <errno.h>
#include <mini-os/os.h>
#include <mini-os/lib.h>
#include <mini-os/console.h>
#include <mini-os/elf.h>
#include <mini-os/err.h>
#include <mini-os/kexec.h>

/*
 * General approach for kexec support (PVH only) is as follows:
 *
 * - New kernel needs to be in memory in form of a ELF binary in a virtual
 *   memory region.
 * - A new start_info structure is constructed in memory with the final
 *   memory locations included.
 * - Page tables and memory pages of the new kernel binary conflicting with the
 *   final memory layout are moved to non-conflicting locations.
 * - All memory areas needed for kexec execution are being finalized.
 * - The final kexec execution stage is copied to a memory area below 4G which
 *   doesn't conflict with the target areas of kernel etc.
 * - From here on a graceful failure is no longer possible.
 * - Grants and event channels are torn down.
 * - Execution continues in the final execution stage.
 * - All data is copied to its final addresses.
 * - CPU is switched to 32-bit mode with paging disabled.
 * - The new kernel is activated.
 */

unsigned long kexec_last_addr;

static int analyze_phdrs(elf_ehdr *ehdr)
{
    elf_phdr *phdr;
    unsigned int n_hdr, i;
    unsigned long paddr, offset, filesz, memsz;
    int ret;

    phdr = elf_ptr_add(ehdr, ehdr_val(ehdr, e_phoff));
    n_hdr = ehdr_val(ehdr, e_phnum);
    for ( i = 0; i < n_hdr; i++ )
    {
        ret = kexec_arch_analyze_phdr(ehdr, phdr);
        if ( ret )
            return ret;

        if ( phdr_val(ehdr, phdr, p_type) == PT_LOAD &&
             (phdr_val(ehdr, phdr, p_flags) & (PF_X | PF_W | PF_R)) )
        {
            paddr = phdr_val(ehdr, phdr, p_paddr);
            offset = phdr_val(ehdr, phdr, p_offset);
            filesz = phdr_val(ehdr, phdr, p_filesz);
            memsz = phdr_val(ehdr, phdr, p_memsz);
            if ( filesz > 0 )
            {
                ret = kexec_add_action(KEXEC_COPY, to_virt(paddr),
                                       (char *)ehdr + offset, filesz);
                if ( ret )
                    return ret;
            }
            if ( memsz > filesz )
            {
                ret = kexec_add_action(KEXEC_ZERO, to_virt(paddr + filesz),
                                       NULL, memsz - filesz);
                if ( ret )
                    return ret;
            }
            if ( paddr + memsz > kexec_last_addr )
                kexec_last_addr = paddr + memsz;
        }

        phdr = elf_ptr_add(phdr, ehdr_val(ehdr, e_phentsize));
    }

    return 0;
}

static int analyze_shdrs(elf_ehdr *ehdr)
{
    elf_shdr *shdr;
    unsigned int n_hdr, i;
    int ret;

    if ( !kexec_arch_need_analyze_shdrs() )
        return 0;

    shdr = elf_ptr_add(ehdr, ehdr_val(ehdr, e_shoff));
    n_hdr = ehdr_val(ehdr, e_shnum);
    for ( i = 0; i < n_hdr; i++ )
    {
        ret = kexec_arch_analyze_shdr(ehdr, shdr);
        if ( ret )
            return ret;

        shdr = elf_ptr_add(shdr, ehdr_val(ehdr, e_shentsize));
    }

    return 0;
}

static int analyze_kernel(void *kernel, unsigned long size)
{
    elf_ehdr *ehdr = kernel;
    int ret;

    if ( !IS_ELF(ehdr->e32) )
    {
        printk("kexec: new kernel not an ELF file\n");
        return ENOEXEC;
    }
    if ( ehdr->e32.e_ident[EI_DATA] != ELFDATA2LSB )
    {
        printk("kexec: ELF file of new kernel is big endian\n");
        return ENOEXEC;
    }
    if ( !elf_is_32bit(ehdr) && !elf_is_64bit(ehdr) )
    {
        printk("kexec: ELF file of new kernel is neither 32 nor 64 bit\n");
        return ENOEXEC;
    }
    if ( !kexec_chk_arch(ehdr) )
    {
        printk("kexec: ELF file of new kernel is not compatible with arch\n");
        return ENOEXEC;
    }

    ret = analyze_phdrs(ehdr);
    if ( ret )
        return ret;

    ret = analyze_shdrs(ehdr);
    if ( ret )
        return ret;

    return 0;
}

int kexec(void *kernel, unsigned long kernel_size, const char *cmdline)
{
    int ret;
    unsigned long *func;

    ret = analyze_kernel(kernel, kernel_size);
    if ( ret )
        return ret;

    kexec_set_param_loc(cmdline);

    reserve_memory_below(kexec_last_addr);

    ret = kexec_get_entry(cmdline);
    if ( ret )
    {
        printk("kexec: ELF file of new kernel has no valid entry point\n");
        goto err;
    }

    change_readonly(false);

    ret = kexec_move_used_pages(kexec_last_addr, (unsigned long)kernel,
                                kernel_size);
    if ( ret )
        goto err;

    for ( func = __kexec_array_start; func < __kexec_array_end; func++ )
    {
        ret = ((kexeccall_t)(*func))(false);
        if ( ret )
        {
            for ( func--; func >= __kexec_array_start; func-- )
                ((kexeccall_t)(*func))(true);

            goto err;
        }
    }

    /* Error exit. */
    ret = ENOSYS;

 err:
    change_readonly(true);
    unreserve_memory_below();
    kexec_move_used_pages_undo();
    kexec_get_entry_undo();

    return ret;
}
EXPORT_SYMBOL(kexec);

struct kexec_action __attribute__((__section__(".data.kexec")))
    kexec_actions[KEXEC_MAX_ACTIONS];
static unsigned int act_idx;

int kexec_add_action(int action, void *dest, void *src, unsigned int len)
{
    struct kexec_action *act;

    if ( act_idx == KEXEC_MAX_ACTIONS )
        return -ENOSPC;

    act = kexec_actions + act_idx;
    act_idx++;

    act->action = action;
    act->len = len;
    act->dest = dest;
    act->src = src;

    return 0;
}
