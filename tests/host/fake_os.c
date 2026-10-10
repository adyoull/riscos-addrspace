/*
 * fake_os.c - the RISC OS calls areas.c makes, faked for the host tests:
 * a list of dynamic areas, a list of tasks, the caps on new areas, and a
 * logical address space with free pieces that OS_DynamicArea 0 takes from
 * (and 1 gives back), as RISC OS does.
 * Part of riscos-addrspace. GPL v2 or later.
 */
#include "fake_os.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

fake_area fake_areas[FAKE_AREAS];
int fake_nareas;
char fake_tasks[FAKE_TASKS][32];
int fake_ntasks;
long fake_clamp[3] = { 128 << 20, 128 << 20, 0x40000000 };
int fake_no_reason24;           /* a RISC OS without OS_DynamicArea 24 */
int fake_enum_fails;            /* OS_DynamicArea 3 fails */
int fake_task_calls;
uint32_t fake_free[FAKE_PIECES];        /* the free pieces of address space */
int fake_npieces;
int fake_made, fake_removed, fake_refuse_remove;
int fake_nmodules, fake_module_calls;
static struct { uint32_t hdr[5]; char title[32]; } fake_modules[FAKE_MODULES];   /* a module's header: title offset at &10 */
static int next_num = 1000;

void fake_os_reset(void)
{
    fake_nareas = fake_ntasks = 0;
    fake_clamp[0] = fake_clamp[1] = 128 << 20;
    fake_clamp[2] = 0x40000000;
    fake_no_reason24 = fake_enum_fails = fake_task_calls = 0;
    fake_npieces = 0;
    fake_made = fake_removed = fake_refuse_remove = 0;
    fake_nmodules = fake_module_calls = 0;
    next_num = 1000;
}

fake_area *fake_add(int num, const char *name, uint32_t base, uint32_t size, uint32_t max, uint32_t flags)
{
    fake_area *a = &fake_areas[fake_nareas++];
    memset(a, 0, sizeof(*a));
    a->num = num;
    snprintf(a->name, sizeof(a->name), "%s", name);
    a->base = base;
    a->size = size;
    a->max = max;
    a->flags = flags;
    return a;
}

void fake_remove(const char *name)
{
    for (int i = 0; i < fake_nareas; i++)
        if (!strcmp(fake_areas[i].name, name)) {
            memmove(&fake_areas[i], &fake_areas[i + 1], (size_t)(fake_nareas - i - 1) * sizeof(fake_area));
            fake_nareas--;
            return;
        }
}

fake_area *fake_find(const char *name)
{
    for (int i = 0; i < fake_nareas; i++)
        if (!strcmp(fake_areas[i].name, name))
            return &fake_areas[i];
    return NULL;
}

void fake_task(const char *name)
{
    snprintf(fake_tasks[fake_ntasks++], sizeof(fake_tasks[0]), "%s", name);
}

void fake_module(const char *title)
{
    memset(&fake_modules[fake_nmodules], 0, sizeof(fake_modules[0]));
    if (title) {
        fake_modules[fake_nmodules].hdr[4] = 5 * 4;
        snprintf(fake_modules[fake_nmodules].title, sizeof(fake_modules[0].title), "%s", title);
    }
    fake_nmodules++;
}

int fake_os_swi(int swi, long *r)
{
    switch (swi) {
    case 0x1E:                          /* OS_Module 12: enumerate, R1 = which (the instance in R2 is 0 here) */
        fake_module_calls++;
        if (r[0] != 12 || r[1] < 0 || r[1] >= fake_nmodules)
            return 1;                   /* "No more modules" */
        r[3] = (long)(uintptr_t)fake_modules[r[1]].hdr;
        r[1]++;
        r[2] = 0;
        return 0;
    case 0x51:                          /* OS_ReadMemMapInfo */
        r[0] = 4096;
        r[1] = 1 << 20;                 /* 4GB */
        return 0;
    case 0x42681: {                     /* TaskManager_EnumerateTasks: 16 bytes each */
        uint32_t *b = (uint32_t *)(uintptr_t)r[1];
        long room = r[2], i = r[0];
        fake_task_calls++;
        while (i < fake_ntasks && room >= 16) {
            b[0] = 0x1000 + (uint32_t)i;
            b[1] = (uint32_t)(uintptr_t)fake_tasks[i];
            b[2] = 640 << 10;
            b[3] = 0;
            b += 4;
            room -= 16;
            i++;
        }
        r[0] = i < fake_ntasks ? i : -1;
        r[1] = (long)(uintptr_t)b;
        r[2] = room;
        return 0;
    }
    case 0x66:                          /* OS_DynamicArea */
        switch (r[0]) {
        case 0: {                       /* create: from the first free piece big enough */
            uint32_t want = (uint32_t)r[5];
            if ((long)want == -1 || want > (uint32_t)fake_clamp[1])
                want = (uint32_t)fake_clamp[1];
            for (int i = 0; i < fake_npieces; i++)
                if (fake_free[i] >= want && fake_nareas < FAKE_AREAS) {
                    fake_area *a = fake_add(next_num++, (const char *)(uintptr_t)r[8], 0x80000000u, 0, want,
                                            (uint32_t)r[4]);
                    a->piece = i;
                    fake_free[i] -= want;
                    fake_made++;
                    r[1] = a->num;
                    r[3] = (long)a->base;
                    r[5] = (long)want;
                    return 0;
                }
            return 1;                   /* "Unable to allocate logical address space" */
        }
        case 1:                         /* remove */
            for (int i = 0; i < fake_nareas; i++)
                if (fake_areas[i].num == r[1]) {
                    if (fake_refuse_remove)
                        return 1;
                    fake_free[fake_areas[i].piece] += fake_areas[i].max;
                    memmove(&fake_areas[i], &fake_areas[i + 1], (size_t)(fake_nareas - i - 1) * sizeof(fake_area));
                    fake_nareas--;
                    fake_removed++;
                    return 0;
                }
            return 1;
        case 2:
        case 24:
            if (r[0] == 24 && fake_no_reason24)
                return 1;
            for (int i = 0; i < fake_nareas; i++)
                if (fake_areas[i].num == r[1]) {
                    fake_area *a = &fake_areas[i];
                    int pmp = (a->flags & (1u << 20)) != 0;
                    r[2] = (long)(r[0] == 2 && pmp ? a->phys_pages * 4096u : a->size);
                    r[3] = (long)a->base;
                    r[4] = (long)a->flags;
                    r[5] = (long)(r[0] == 2 && pmp ? a->phys_max_pages * 4096u : a->max);
                    r[6] = r[0] == 24 ? (long)(pmp ? a->phys_pages : a->size / 4096) : 0;
                    r[7] = r[0] == 24 ? (long)(pmp ? a->phys_max_pages : a->max / 4096) : 0;
                    r[8] = (long)(uintptr_t)a->name;
                    return 0;
                }
            return 1;
        case 3:                         /* enumerate */
            if (fake_enum_fails)
                return 1;
            if (r[1] == -1) {
                r[1] = fake_nareas ? fake_areas[0].num : -1;
                return 0;
            }
            for (int i = 0; i < fake_nareas; i++)
                if (fake_areas[i].num == r[1]) {
                    r[1] = i + 1 < fake_nareas ? fake_areas[i + 1].num : -1;
                    return 0;
                }
            return 1;
        case 8:                         /* the caps: read only */
            r[1] = fake_clamp[0];
            r[2] = fake_clamp[1];
            r[3] = fake_clamp[2];
            return 0;
        }
        return 1;
    }
    return 1;
}
