/* -*-  Mode:C; c-basic-offset:4; tab-width:4 -*-
 *
 * (C) 2021 - Juergen Gross, SUSE Software Solutions Germany GmbH
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

#include <mini-os/types.h>
#include <mini-os/lib.h>
#include <mini-os/console.h>
#include <mini-os/os.h>
#include <mini-os/posix/limits.h>
#include <mini-os/e820.h>
#include <xen/memory.h>

#ifdef CONFIG_E820_TRIVIAL
struct e820entry e820_map[1] = {
    {
        .addr = 0,
        .size = ULONG_MAX - 1,
        .type = E820_RAM
    }
};

unsigned e820_entries = 1;

static void e820_get_memmap(void)
{
}

#else
struct e820entry e820_map[E820_MAX];
unsigned e820_entries;

static char *e820_types[E820_TYPES] = {
    [E820_RAM]      = "RAM",
    [E820_RESERVED] = "Reserved",
    [E820_ACPI]     = "ACPI",
    [E820_NVS]      = "NVS",
    [E820_UNUSABLE] = "Unusable",
    [E820_PMEM]     = "PMEM"
};

static void e820_get_memmap(void)
{
    long ret;
    struct xen_memory_map memmap;

    memmap.nr_entries = E820_MAX;
    set_xen_guest_handle(memmap.buffer, e820_map);
    ret = HYPERVISOR_memory_op(XENMEM_memory_map, &memmap);
    if ( ret < 0 )
    {
        xprintk("could not get memory map\n");
        do_exit();
    }
    e820_entries = memmap.nr_entries;
}

void arch_print_memmap(void)
{
    int i;
    unsigned long from, to;
    char *type;
    char buf[12];

    printk("Memory map:\n");
    for ( i = 0; i < e820_entries; i++ )
    {
        if ( e820_map[i].type >= E820_TYPES || !e820_types[e820_map[i].type] )
        {
            snprintf(buf, sizeof(buf), "%8x", e820_map[i].type);
            type = buf;
        }
        else
        {
            type = e820_types[e820_map[i].type];
        }
        from = e820_map[i].addr;
        to = from + e820_map[i].size - 1;
        printk("%012lx-%012lx: %s\n", from, to, type);
    }
}
#endif

unsigned long e820_get_maxpfn(void)
{
    int i;
    unsigned long pfn, max = 0;

    e820_get_memmap();

    for ( i = 0; i < e820_entries; i++ )
    {
        if ( e820_map[i].type != E820_RAM )
            continue;
        pfn = (e820_map[i].addr + e820_map[i].size) >> PAGE_SHIFT;
        if ( pfn > max )
            max = pfn;
    }

    return max;
}
