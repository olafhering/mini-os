#ifndef _SHUTDOWN_H_
#define _SHUTDOWN_H_

#include <mini-os/hypervisor.h>

void init_shutdown(void);
void fini_shutdown(void);
void kernel_suspend(void);

#endif
