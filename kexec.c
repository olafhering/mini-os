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

int kexec(void *kernel, unsigned long kernel_size, const char *cmdline)
{
    return ENOSYS;
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
