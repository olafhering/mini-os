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

MINIOS_TAILQ_HEAD(thread_list, struct thread);

struct thread *idle_thread = NULL;
static struct thread_list exited_threads = MINIOS_TAILQ_HEAD_INITIALIZER(exited_threads);
static struct thread_list thread_list = MINIOS_TAILQ_HEAD_INITIALIZER(thread_list);
static int threads_started;

struct thread *main_thread;

void schedule(void)
{
    struct thread *prev, *next, *thread, *tmp;
    unsigned long flags;

    if (irqs_disabled()) {
        printk("Must not call schedule() with IRQs disabled\n");
        BUG();
    }

    prev = current;
    local_irq_save(flags); 

    do {
        /* Examine all threads.
           Find a runnable thread, but also wake up expired ones and find the
           time when the next timeout expires, else use 10 seconds. */
        s_time_t now = NOW();
        s_time_t min_wakeup_time = now + SECONDS(10);
        next = NULL;
        MINIOS_TAILQ_FOREACH_SAFE(thread, &thread_list, thread_list, tmp)
        {
            if (!is_runnable(thread) && thread->wakeup_time != 0LL)
            {
                if (thread->wakeup_time <= now)
                    wake(thread);
                else if (thread->wakeup_time < min_wakeup_time)
                    min_wakeup_time = thread->wakeup_time;
            }
            if(is_runnable(thread)) 
            {
                next = thread;
                /* Put this thread on the end of the list */
                MINIOS_TAILQ_REMOVE(&thread_list, thread, thread_list);
                MINIOS_TAILQ_INSERT_TAIL(&thread_list, thread, thread_list);
                break;
            }
        }
        if (next)
            break;
        /* block until the next timeout expires, or for 10 secs, whichever comes first */
        block_domain(min_wakeup_time);
        /* handle pending events if any */
        force_evtchn_callback();
    } while(1);
    local_irq_restore(flags);
    /* Interrupting the switch is equivalent to having the next thread
       inturrupted at the return instruction. And therefore at safe point. */
    if(prev != next) switch_threads(prev, next);

    MINIOS_TAILQ_FOREACH_SAFE(thread, &exited_threads, thread_list, tmp)
    {
        if(thread != prev)
        {
            MINIOS_TAILQ_REMOVE(&exited_threads, thread, thread_list);
            free_pages(thread->stack, STACK_SIZE_PAGE_ORDER);
            xfree(thread);
        }
    }
}
EXPORT_SYMBOL(schedule);

struct thread* create_thread(char *name, void (*function)(void *), void *data)
{
    struct thread *thread;
    unsigned long flags;
    /* Call architecture specific setup. */
    thread = arch_create_thread(name, function, data);
    /* Not runable, not exited, not sleeping */
    thread->flags = 0;
    thread->wakeup_time = 0LL;
#ifdef HAVE_LIBC
    _REENT_INIT_PTR((&thread->reent))
#endif
    set_runnable(thread);
    local_irq_save(flags);
    MINIOS_TAILQ_INSERT_TAIL(&thread_list, thread, thread_list);
    local_irq_restore(flags);
    return thread;
}
EXPORT_SYMBOL(create_thread);

#ifdef HAVE_LIBC
struct _reent *__getreent(void)
{
    struct _reent *_reent;

    if (!threads_started)
        _reent = _impure_ptr;
    else
        _reent = &get_current()->reent;

#ifndef NDEBUG
#if defined(__x86_64__) || defined(__x86__)
    {
#ifdef __x86_64__
        register unsigned long sp asm ("rsp");
#else
        register unsigned long sp asm ("esp");
#endif
        if ((sp & (STACK_SIZE-1)) < STACK_SIZE / 16) {
            static int overflowing;
            if (!overflowing) {
                overflowing = 1;
                printk("stack overflow\n");
                BUG();
            }
        }
    }
#endif
#else
#error Not implemented yet
#endif
    return _reent;
}
EXPORT_SYMBOL(__getreent);
#endif

void exit_thread(void)
{
    unsigned long flags;
    struct thread *thread = current;
    printk("Thread \"%s\" exited.\n", thread->name);
    local_irq_save(flags);
    /* Remove from the thread list */
    MINIOS_TAILQ_REMOVE(&thread_list, thread, thread_list);
    clear_runnable(thread);
    /* Put onto exited list */
    MINIOS_TAILQ_INSERT_HEAD(&exited_threads, thread, thread_list);
    local_irq_restore(flags);
    /* Schedule will free the resources */
    while(1)
    {
        schedule();
        printk("schedule() returned!  Trying again\n");
    }
}
EXPORT_SYMBOL(exit_thread);

void block(struct thread *thread)
{
    thread->wakeup_time = 0LL;
    clear_runnable(thread);
}
EXPORT_SYMBOL(block);

void msleep(uint32_t millisecs)
{
    struct thread *thread = get_current();
    thread->wakeup_time = NOW()  + MILLISECS(millisecs);
    clear_runnable(thread);
    schedule();
}
EXPORT_SYMBOL(msleep);

void wake(struct thread *thread)
{
    thread->wakeup_time = 0LL;
    set_runnable(thread);
}
EXPORT_SYMBOL(wake);

void idle_thread_fn(void *unused)
{
    threads_started = 1;
    while (1) {
        block(current);
        schedule();
    }
}

void init_sched(void)
{
    printk("Initialising scheduler\n");

    idle_thread = create_thread("Idle", idle_thread_fn, NULL);
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
