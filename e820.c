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

/*
 * E820 type based bitmask for deciding how to round entries to page
 * boundaries: A set bit means the type relates to a resource managed by
 * Mini-OS (e.g. RAM), so rounding needs to be done to only include pages
 * completely of the related type (narrowing). All other types need to be
 * rounded to include all pages with parts of that type (widening).
 */
#define E820_NARROW ((1U << E820_RAM) | (1U << E820_NVS) | (1 << E820_PMEM))

/* Private type used to mark a range temporarily as reserved (lowest prio). */
#define E820_TMP_RESERVED    0

static void e820_remove_entry(int idx)
{
    int i;

    e820_entries--;
    for ( i = idx; i < e820_entries; i++ )
        e820_map[i] = e820_map[i + 1];
}

static void e820_insert_entry_at(int idx, unsigned long addr,
                                 unsigned long size, unsigned int type)
{
    int i;

    if ( e820_entries == E820_MAX )
    {
        xprintk("E820 memory map overflow\n");
        do_exit();
    }

    e820_entries++;
    for ( i = e820_entries - 1; i > idx; i-- )
        e820_map[i] = e820_map[i - 1];

    e820_map[idx].addr = addr;
    e820_map[idx].size = size;
    e820_map[idx].type = type;
}

static void e820_insert_entry(unsigned long addr, unsigned long size,
                              unsigned int type)
{
    int i;

    for ( i = 0; i < e820_entries && addr > e820_map[i].addr; i++ );

    e820_insert_entry_at(i, addr, size, type);
}

static void e820_swap_entries(int idx1, int idx2)
{
    struct e820entry entry;

    entry = e820_map[idx1];
    e820_map[idx1] = e820_map[idx2];
    e820_map[idx2] = entry;
}

/*
 * Do a memory map sanitizing sweep:
 * - sort the entries by start address
 * - remove overlaps of entries (higher type value wins)
 * - merge adjacent entries of same type
 */
static void e820_process_entries(void)
{
    int i, j;
    unsigned long end, start;
    unsigned int type;

    /* Sort entries. */
    for ( i = 1; i < e820_entries; i++ )
        for ( j = i; j > 0 && e820_map[j - 1].addr > e820_map[j].addr; j-- )
            e820_swap_entries(j - 1, j);

    /* Handle overlapping entries (higher type values win). */
    for ( i = 1; i < e820_entries; i++ )
    {
        if ( e820_map[i - 1].addr + e820_map[i - 1].size <= e820_map[i].addr )
            continue;
        if ( e820_map[i - 1].addr < e820_map[i].addr )
        {
            e820_insert_entry_at(i - 1, e820_map[i - 1].addr,
                                 e820_map[i].addr - e820_map[i - 1].addr,
                                 e820_map[i - 1].type);
            e820_map[i].addr += e820_map[i - 1].size;
            e820_map[i].size -= e820_map[i - 1].size;
            i++;
        }
        if ( e820_map[i - 1].type < e820_map[i].type )
            e820_swap_entries(i - 1, i);
        if ( e820_map[i - 1].size >= e820_map[i].size )
        {
            e820_remove_entry(i);
            i--;
        }
        else
        {
            start = e820_map[i].addr + e820_map[i - 1].size;
            end = e820_map[i].addr + e820_map[i].size;
            type = e820_map[i].type;
            e820_remove_entry(i);
            e820_insert_entry(start, end - start, type);
        }
    }

    /* Merge adjacent entries. */
    for ( i = 0; i < e820_entries - 1; i++ )
    {
        if ( e820_map[i].type == e820_map[i + 1].type &&
             e820_map[i].addr + e820_map[i].size >= e820_map[i + 1].addr )
        {
            if ( e820_map[i].addr + e820_map[i].size <
                 e820_map[i + 1].addr + e820_map[i + 1].size )
            {
                e820_map[i].size = e820_map[i + 1].addr - e820_map[i].addr +
                                   e820_map[i + 1].size;
            }
            e820_remove_entry(i + 1);
            i--;
        }
    }
}

/*
 * Transform memory map into a well sorted map without any overlaps.
 * - sort map entries by start address
 * - handle overlaps
 * - merge adjacent entries of same type (possibly removing boundary in the
 *   middle of a page)
 * - trim entries to page boundaries (depending on type either expanding
 *   the entry or narrowing it down)
 * - repeat first 3 sanitizing steps
 * - make remaining temporarily reserved entries permanently reserved
 */
static void e820_sanitize(void)
{
    int i;
    unsigned long end, start;

    /* Sanitize memory map in current form. */
    e820_process_entries();

    /* Adjust map entries to page boundaries. */
    for ( i = 0; i < e820_entries; i++ )
    {
        start = e820_map[i].addr;
        end = start + e820_map[i].size;
        if ( (1U << e820_map[i].type) & E820_NARROW )
        {
            if ( start & (PAGE_SIZE - 1) )
            {
                start = round_pgup(start);
                e820_insert_entry_at(i, start - PAGE_SIZE, PAGE_SIZE,
                                     E820_TMP_RESERVED);
                i++;
            }
            if ( end & (PAGE_SIZE - 1) )
            {
                end = round_pgdown(end);
                e820_insert_entry_at(i + 1, end, PAGE_SIZE, E820_TMP_RESERVED);
                i++;
            }
        }
        else
        {
            start = round_pgdown(start);
            end = round_pgup(end);
        }
        e820_map[i].addr = start;
        e820_map[i].size = end - start;
    }

    /* Sanitize memory map (again). */
    e820_process_entries();

    /* Make remaining temporarily reserved entries permanently reserved. */
    for ( i = 0; i < e820_entries; i++ )
        if ( e820_map[i].type == E820_TMP_RESERVED )
            e820_map[i].type = E820_RESERVED;
}

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

    e820_sanitize();
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

unsigned long e820_get_reserved_pfns(int pages)
{
    int i;
    unsigned long last = 0, needed = (long)pages << PAGE_SHIFT;

    for ( i = 0; i < e820_entries && e820_map[i].addr < last + needed; i++ )
        last = e820_map[i].addr + e820_map[i].size;

    if ( i == 0 || e820_map[i - 1].type != E820_RESERVED )
        e820_insert_entry_at(i, last, needed, E820_RESERVED);
    else
        e820_map[i - 1].size += needed;

    return last >> PAGE_SHIFT;
}

void e820_put_reserved_pfns(unsigned long start_pfn, int pages)
{
    int i;
    unsigned long addr = start_pfn << PAGE_SHIFT;
    unsigned long size = (long)pages << PAGE_SHIFT;

    for ( i = 0;
          i < e820_entries && addr >= e820_map[i].addr + e820_map[i].size;
          i++ );

    BUG_ON(i == e820_entries || e820_map[i].type != E820_RESERVED ||
           addr + size > e820_map[i].addr + e820_map[i].size);

    if ( addr == e820_map[i].addr )
    {
        e820_map[i].addr += size;
        e820_map[i].size -= size;
        if ( e820_map[i].size == 0 )
            e820_remove_entry(i);
        return;
    }

    if ( addr + size == e820_map[i].addr + e820_map[i].size )
    {
        e820_map[i].size -= size;
        return;
    }

    e820_insert_entry_at(i + 1, addr + size,
                         e820_map[i].addr + e820_map[i].size - addr - size,
                         E820_RESERVED);
    e820_map[i].size = addr - e820_map[i].addr;
}
#endif

unsigned long e820_get_maxpfn(unsigned long pages)
{
    int i;
    unsigned long pfns = 0, start = 0;

    if ( !e820_entries )
        e820_get_memmap();

    for ( i = 0; i < e820_entries; i++ )
    {
        if ( e820_map[i].type != E820_RAM )
            continue;
        pfns = e820_map[i].size >> PAGE_SHIFT;
        start = e820_map[i].addr >> PAGE_SHIFT;
        if ( pages <= pfns )
            return start + pages;
        pages -= pfns;
    }

    return start + pfns;
}

unsigned long e820_get_max_contig_pages(unsigned long pfn, unsigned long pages)
{
    int i;
    unsigned long end;

    for ( i = 0; i < e820_entries && e820_map[i].addr <= (pfn << PAGE_SHIFT);
          i++ )
    {
        end = (e820_map[i].addr + e820_map[i].size) >> PAGE_SHIFT;
        if ( e820_map[i].type != E820_RAM || end <= pfn )
            continue;

        return ((end - pfn) > pages) ? pages : end - pfn;
    }

    return 0;
}
