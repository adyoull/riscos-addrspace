/*
 * sys.c - as_swi() on RISC OS: the X form of a SWI, through UnixLib's
 * _kernel_swi (which sets the X bit itself).
 * Part of riscos-addrspace. GPL v2 or later.
 */
#include "areas.h"

#include <kernel.h>
#include <string.h>

static char last_error[252];

int as_swi(int swi, long *r)
{
    _kernel_swi_regs in, out;
    _kernel_oserror *e;
    for (int i = 0; i < 10; i++)
        in.r[i] = (int)r[i];
    e = _kernel_swi(swi, &in, &out);
    if (e) {
        snprintf(last_error, sizeof(last_error), "%s", e->errmess);
        return 1;
    }
    for (int i = 0; i < 10; i++)
        r[i] = out.r[i];
    return 0;
}

const char *as_swi_error(void)
{
    return last_error;
}
