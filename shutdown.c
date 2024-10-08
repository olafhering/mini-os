/*
 *          MiniOS
 *
 *   file: shutdown.c
 *
 * Authors: Joao Martins <joao.martins@neclab.eu>
 *
 *
 * Copyright (c) 2014, NEC Europe Ltd., NEC Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * THIS HEADER MAY NOT BE EXTRACTED OR MODIFIED IN ANY WAY.
 */

#include <mini-os/os.h>
#include <mini-os/events.h>
#include <mini-os/kernel.h>
#include <mini-os/sched.h>
#include <mini-os/shutdown.h>
#include <mini-os/lib.h>
#include <mini-os/xenbus.h>
#include <mini-os/xmalloc.h>


#ifdef CONFIG_XENBUS
static const char *path = "control/shutdown";
static const char *token = "control/shutdown";
static xenbus_event_queue events = NULL;
static int end_shutdown_thread = 0;

/* This should be overridden by the application we are linked against. */
__attribute__((weak)) void app_shutdown(unsigned reason)
{
    printk("Shutdown requested: %d\n", reason);
    if (reason == SHUTDOWN_suspend) {
        kernel_suspend();
    } else {
        struct sched_shutdown sched_shutdown = { .reason = reason };
        HYPERVISOR_sched_op(SCHEDOP_shutdown, &sched_shutdown);
    }
}

static void shutdown_thread(void *p)
{
    char *shutdown, *err;
    unsigned int shutdown_reason;

    free(xenbus_watch_path_token(XBT_NIL, path, token, &events));

    for ( ;; ) {
        xenbus_wait_for_watch(&events);
        if ((err = xenbus_read(XBT_NIL, path, &shutdown))) {
            free(err);
            free(xenbus_unwatch_path_token(XBT_NIL, path, token));
            printk("Shutdown Xenstore node not available.\n");
            return;
        }

        if (end_shutdown_thread)
            break;

        if (!strcmp(shutdown, "")) {
            /*
             * Avoid spurious event on xenbus.
             * Watches will fire directly after setting them up once.
             */
            free(shutdown);
            continue;
        } else if (!strcmp(shutdown, "poweroff")) {
            shutdown_reason = SHUTDOWN_poweroff;
        } else if (!strcmp(shutdown, "reboot")) {
            shutdown_reason = SHUTDOWN_reboot;
        } else if (!strcmp(shutdown, "suspend")) {
            shutdown_reason = SHUTDOWN_suspend;
        } else {
            shutdown_reason = SHUTDOWN_crash;
        }
        free(shutdown);

        /* Acknowledge shutdown request */
        if ((err = xenbus_write(XBT_NIL, path, ""))) {
            free(err);
            do_exit();
        }

        app_shutdown(shutdown_reason);
    }
}

void init_shutdown(void)
{
    end_shutdown_thread = 0;
    create_thread("shutdown", shutdown_thread, NULL);
}

void fini_shutdown(void)
{
    end_shutdown_thread = 1;
    xenbus_release_wait_for_watch(&events);
    free(xenbus_unwatch_path_token(XBT_NIL, path, token));
}
#endif

void kernel_suspend(void)
{
    int rc;

    printk("MiniOS will suspend ...\n");

    pre_suspend();
    arch_pre_suspend();

    rc = arch_suspend();

    arch_post_suspend(rc);
    post_suspend(rc);

    if (rc) {
        printk("MiniOS suspend canceled!");
    } else {
        printk("MiniOS resumed from suspend!\n");
    }
}
