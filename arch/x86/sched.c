/* 
 ****************************************************************************
 * (C) 2005 - Grzegorz Milos - Intel Research Cambridge
 ****************************************************************************
 *
 *        File: sched.c
 *      Author: Grzegorz Milos
 *     Changes: Robert Kaiser
 *              
 *        Date: Aug 2005
 * 
 * Environment: Xen Minimal OS
 * Description: simple scheduler for Mini-Os
 *
 * The scheduler is non-preemptive (cooperative), and schedules according 
 * to Round Robin algorithm.
 *
 ****************************************************************************
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

#include <mini-os/os.h>
#include <mini-os/hypervisor.h>
#include <mini-os/time.h>
#include <mini-os/mm.h>
#include <mini-os/types.h>
#include <mini-os/lib.h>
#include <mini-os/xmalloc.h>
#include <mini-os/list.h>
#include <mini-os/sched.h>
#include <mini-os/semaphore.h>


#ifdef SCHED_DEBUG
#define DEBUG(_f, _a...) \
    printk("MINI_OS(file=sched.c, line=%d) " _f "\n", __LINE__, ## _a)
#else
#define DEBUG(_f, _a...)    ((void)0)
#endif


void dump_stack(struct thread *thread)
{
    unsigned long *bottom = (unsigned long *)(thread->stack + STACK_SIZE); 
    unsigned long *pointer = (unsigned long *)thread->sp;
    int count;

    if ( thread == current )
        asm ( "mov %%"ASM_SP",%0" : "=r" (pointer) );

    printk("The stack for \"%s\"\n", thread->name);
    for(count = 0; count < 25 && pointer < bottom; count ++)
    {
        printk("[0x%p] 0x%lx\n", pointer, *pointer);
        pointer++;
    }
    
    if(pointer < bottom) printk(" ... continues.\n");
}

/* Gets run when a new thread is scheduled the first time ever, 
   defined in x86_[32/64].S */
extern void thread_starter(void);

/* Pushes the specified value onto the stack of the specified thread */
static void stack_push(struct thread *thread, unsigned long value)
{
    thread->sp -= sizeof(unsigned long);
    *((unsigned long *)thread->sp) = value;
}

/* Architecture specific setup of thread creation */
struct thread* arch_create_thread(char *name, void (*function)(void *),
                                  void *data)
{
    struct thread *thread;
    
    thread = xmalloc(struct thread);
    /* We can't use lazy allocation here since the trap handler runs on the stack */
    thread->stack = (char *)alloc_pages(STACK_SIZE_PAGE_ORDER);
    thread->name = name;
    printk("Thread \"%s\": pointer: 0x%p, stack: 0x%p\n", name, thread, 
            thread->stack);
    
    thread->sp = (unsigned long)thread->stack + STACK_SIZE;
    /* Save pointer to the thread on the stack, used by current macro */
    *((unsigned long *)thread->stack) = (unsigned long)thread;

    /* Must ensure that (%rsp + 8) is 16-byte aligned at the start of thread_starter. */
    thread->sp -= sizeof(unsigned long);
    
    stack_push(thread, (unsigned long) function);
    stack_push(thread, (unsigned long) data);
    thread->ip = (unsigned long) thread_starter;
    return thread;
}

void run_idle_thread(void)
{
    /* Switch stacks and run the thread */
    asm volatile ( "mov %[sp], %%"ASM_SP"\n\t"
                   "jmp *%[ip]\n\t"
                   :
                   : [sp] "m" (idle_thread->sp),
                     [ip] "m" (idle_thread->ip) );
}

unsigned long __local_irq_save(void)
{
    unsigned long flags;

    local_irq_save(flags);
    return flags;
}
EXPORT_SYMBOL(__local_irq_save);

void __local_irq_restore(unsigned long flags)
{
    local_irq_restore(flags);
}
EXPORT_SYMBOL(__local_irq_restore);

unsigned long __local_save_flags(void)
{
    unsigned long flags;

    local_save_flags(flags);
    return flags;
}
EXPORT_SYMBOL(__local_save_flags);

void __local_irq_disable(void)
{
    local_irq_disable();
}
EXPORT_SYMBOL(__local_irq_disable);

void __local_irq_enable(void)
{
    local_irq_enable();
}
EXPORT_SYMBOL(__local_irq_enable);
