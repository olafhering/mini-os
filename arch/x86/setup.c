/******************************************************************************
 * common.c
 * 
 * Common stuff special to x86 goes here.
 * 
 * Copyright (c) 2002-2003, K A Fraser & R Neugebauer
 * Copyright (c) 2005, Grzegorz Milos, Intel Research Cambridge
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
 *
 */

#include <mini-os/os.h>
#include <mini-os/lib.h> /* for printk, memcpy */
#include <mini-os/kernel.h>
#include <mini-os/hypervisor.h>
#include <xen/xen.h>
#include <xen/arch-x86/cpuid.h>
#include <xen/arch-x86/hvm/start_info.h>
#include <xen/hvm/params.h>

/*
 * Shared page for communicating with the hypervisor.
 * Events flags go here, for example.
 */
shared_info_t *HYPERVISOR_shared_info;

/*
 * Just allocate the kernel stack here. SS:ESP is set up to point here
 * in head.S.
 */
char stack[2*STACK_SIZE];

extern char shared_info[PAGE_SIZE];

static inline void fpu_init(void) {
	asm volatile("fninit");
}

#ifdef __SSE__
static inline void sse_init(void) {
	unsigned long status = 0x1f80;
	asm volatile("ldmxcsr %0" : : "m" (status));
}
#else
#define sse_init()
#endif

#ifdef CONFIG_PARAVIRT
/*
 * This pointer holds a reference to the start_info struct.
 */
start_info_t *start_info_ptr;
EXPORT_SYMBOL(start_info_ptr);

#define hpc_init()

shared_info_t *map_shared_info(void)
{
    int rc;
    unsigned long pa = start_info_ptr->shared_info;

    if ( (rc = HYPERVISOR_update_va_mapping((unsigned long)shared_info,
                                            __pte(pa | 7), UVMF_INVLPG)) )
    {
        printk("Failed to map shared_info!! rc=%d\n", rc);
        do_exit();
    }
    return (shared_info_t *)shared_info;
}

void unmap_shared_info(void)
{
    int rc;
    pte_t nullpte = { };

    if ( (rc = HYPERVISOR_update_va_mapping((unsigned long)shared_info,
                                            nullpte, UVMF_INVLPG)) )
    {
        printk("Failed to unmap shared_info page!! rc=%d\n", rc);
        do_exit();
    }
}

static void get_cmdline(void *p)
{
    start_info_t *si = p;

    strncpy(cmdline, (char *)si->cmd_line, MAX_CMDLINE_SIZE - 1);
}

static void print_start_of_day(void *p)
{
    start_info_t *si = p;

    printk("Xen Minimal OS (pv)!\n");
    printk("  start_info: %p(VA)\n", si);
    printk("    nr_pages: 0x%lx\n", si->nr_pages);
    printk("  shared_inf: 0x%08lx(MA)\n", si->shared_info);
    printk("     pt_base: %p(VA)\n", (void *)si->pt_base);
    printk("nr_pt_frames: 0x%lx\n", si->nr_pt_frames);
    printk("    mfn_list: %p(VA)\n", (void *)si->mfn_list);
    printk("   mod_start: 0x%lx(VA)\n", si->mod_start);
    printk("     mod_len: %lu\n", si->mod_len);
    printk("       flags: 0x%x\n", (unsigned int)si->flags);
    printk("    cmd_line: %s\n", cmdline);
    printk("       stack: %p-%p\n", stack, stack + sizeof(stack));
}
#else
static void hpc_init(void)
{
    uint32_t eax, ebx, ecx, edx, base;

    for ( base = XEN_CPUID_FIRST_LEAF;
          base < XEN_CPUID_FIRST_LEAF + 0x10000; base += 0x100 )
    {
        cpuid(base, &eax, &ebx, &ecx, &edx);

        if ( (ebx == XEN_CPUID_SIGNATURE_EBX) &&
             (ecx == XEN_CPUID_SIGNATURE_ECX) &&
             (edx == XEN_CPUID_SIGNATURE_EDX) &&
             ((eax - base) >= 2) )
            break;
    }

    cpuid(base + 2, &eax, &ebx, &ecx, &edx);
    wrmsrl(ebx, (unsigned long)&hypercall_page);
    barrier();
}

static void get_cmdline(void *p)
{
    struct hvm_start_info *si = p;

    if ( si->cmdline_paddr )
        strncpy(cmdline, to_virt(si->cmdline_paddr), MAX_CMDLINE_SIZE - 1);
}

static void print_start_of_day(void *p)
{
    struct hvm_start_info *si = p;

    printk("Xen Minimal OS (hvm)!\n");
    printk("  start_info: %p(VA)\n", si);
    printk("  shared_inf: %p(VA)\n", HYPERVISOR_shared_info);
    printk("     modlist: 0x%lx(PA)\n", (unsigned long)si->modlist_paddr);
    printk("  nr_modules: %u\n", si->nr_modules);
    printk("       flags: 0x%x\n", (unsigned int)si->flags);
    printk("    cmd_line: %s\n", cmdline);
    printk("       stack: %p-%p\n", stack, stack + sizeof(stack));
    arch_print_memmap();
}
#endif

/*
 * INITIAL C ENTRY POINT.
 */
void
arch_init(void *par)
{
	static char hello[] = "Bootstrapping...\n";

#ifdef CONFIG_PARAVIRT
	start_info_ptr = par;
#endif

	hpc_init();
	(void)HYPERVISOR_console_io(CONSOLEIO_write, strlen(hello), hello);

	trap_init();

	/*Initialize floating point unit */
	fpu_init();

	/* Initialize SSE */
	sse_init();

	/* Setup memory management info from start_info. */
	arch_mm_preinit(par);

	/* WARN: don't do printk before here, it uses information from
	   shared_info. Use xprintk instead. */
	get_console();
	get_xenbus();
	get_cmdline(par);

	/* Grab the shared_info pointer and put it in a safe place. */
	HYPERVISOR_shared_info = map_shared_info();

	/* print out some useful information  */
	print_start_of_day(par);

	start_kernel();
}

void arch_pre_suspend(void)
{
#ifdef CONFIG_PARAVIRT
   /* Replace xenstore and console mfns with the correspondent pfns */
    start_info_ptr->store_mfn =
        virt_to_pfn(mfn_to_virt(start_info_ptr->store_mfn));
    start_info_ptr->console.domU.mfn =
        virt_to_pfn(mfn_to_virt(start_info_ptr->console.domU.mfn));
#endif
    unmap_shared_info();

    arch_mm_pre_suspend();
}

int arch_suspend(void)
{
    /*
     * This hypercall returns 1 if the suspend
     * was cancelled and 0 if resuming in a new domain
     */
#ifdef CONFIG_PARAVIRT
    return HYPERVISOR_suspend(virt_to_mfn(start_info_ptr));
#else
    return HYPERVISOR_suspend(0);
#endif
}

void arch_post_suspend(int canceled)
{
#if CONFIG_PARAVIRT
    if (canceled) {
        start_info_ptr->store_mfn = pfn_to_mfn(start_info_ptr->store_mfn);
        start_info_ptr->console.domU.mfn = pfn_to_mfn(start_info_ptr->console.domU.mfn);
    }
#endif

    HYPERVISOR_shared_info = map_shared_info();
#ifndef CONFIG_PARAVIRT
    xen_callback_vector();
#endif
    arch_mm_post_suspend(canceled);
}

void
arch_fini(void)
{
	/* Reset traps */
	trap_fini();

#ifdef __i386__
	HYPERVISOR_set_callbacks(0, 0, 0, 0);
#else
	HYPERVISOR_set_callbacks(0, 0, 0);
#endif
}

void
arch_do_exit(void)
{
	stack_walk();
}
