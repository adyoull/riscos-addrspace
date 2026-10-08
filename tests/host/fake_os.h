/*
 * fake_os.h - the RISC OS calls areas.c makes, faked (fake_os.c).
 * Part of riscos-addrspace. GPL v2 or later.
 */
#ifndef FAKE_OS_H
#define FAKE_OS_H

#include <stdint.h>

#define FAKE_AREAS  700
#define FAKE_TASKS  40
#define FAKE_PIECES 16

typedef struct {
    int num;
    char name[40];
    uint32_t base, size, max, flags;
    uint32_t phys_pages, phys_max_pages;    /* a physical memory pool's */
    int piece;                              /* made by OS_DynamicArea 0: from this free piece */
} fake_area;

extern fake_area fake_areas[FAKE_AREAS];
extern int fake_nareas;
extern char fake_tasks[FAKE_TASKS][32];
extern int fake_ntasks;
extern long fake_clamp[3];
extern int fake_no_reason24, fake_enum_fails, fake_task_calls;
extern uint32_t fake_free[FAKE_PIECES];
extern int fake_npieces;
extern int fake_made, fake_removed, fake_refuse_remove;

void fake_os_reset(void);
fake_area *fake_add(int num, const char *name, uint32_t base, uint32_t size, uint32_t max, uint32_t flags);
void fake_remove(const char *name);
fake_area *fake_find(const char *name);
void fake_task(const char *name);
/* A SWI: 0 = done, 1 = an error */
int fake_os_swi(int swi, long *r);

#endif
