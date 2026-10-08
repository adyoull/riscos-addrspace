/*
 * areas.h - what is using RISC OS's logical address space: its dynamic
 * areas, who they probably belong to, and how much space is left.
 *
 * Each dynamic area reserves a range of logical addresses as big as its
 * maximum size when it's made, however little it holds; that, not the
 * memory in it, is what runs out ("Unable to allocate logical address
 * space"). So the list is of reservations:
 *
 *   read:     OS_DynamicArea 3 lists the areas; OS_DynamicArea 24 (or 2 on
 *             a RISC OS without it) gives each one's name, base, size and
 *             maximum. 24 gives a physical memory pool's logical sizes (2
 *             gives its physical ones). A doubly-mapped area reserves its
 *             maximum twice.
 *   owners:   the tasks running (TaskManager_EnumerateTasks); an area
 *             whose name starts with a task's name is put down to it. A
 *             UnixLib program's heap is "<name> Heap" (and "<name> Heap 2",
 *             3...): one whose program isn't running was left behind.
 *             Areas numbered below 256 are RISC OS's own.
 *   changes:  each reading is compared with the one before: new areas,
 *             ones that grew or shrank, and ones that have gone (kept in
 *             the list for a while).
 *   free:     measured, not worked out: areas are made (each 0 bytes in
 *             size, its maximum as big as RISC OS allows, halved down to
 *             1MB when there's no room) until no more fit, then removed.
 *             Their maximums add up to the space that was free.
 *
 * Every SWI goes through as_swi() (sys.c on RISC OS; the tests have a fake
 * one). Nothing here allocates memory: a program that reads its heap from
 * a dynamic area, or mallocs big blocks, would change what it measures.
 * Part of riscos-addrspace. GPL v2 or later.
 */
#ifndef ADDRSPACE_AREAS_H
#define ADDRSPACE_AREAS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define AREAS_MAX   320
#define TASKS_MAX   128
#define PROBE_MAX   512
#define PROBE_NAME  "AddrSpace probe"

#define OS_ReadMemMapInfo           0x51
#define OS_DynamicArea              0x66
#define TaskManager_EnumerateTasks  0x42681

/* A SWI, always the X form: r[0..9] in and out. 0 = done; else an error
   (as_swi_error() says what). */
int as_swi(int swi, long *r);
const char *as_swi_error(void);

/* What happened to an area since the reading before */
enum { CH_NONE, CH_NEW, CH_GREW, CH_SHRANK, CH_GONE };
/* What it is (as far as can be told) */
enum { KIND_OTHER, KIND_OS, KIND_HEAP, KIND_HEAP_LEFT, KIND_MMAP, KIND_UNIXLIB_MMAP };
/* The list's columns (and what it can be sorted by) */
enum { COL_NAME, COL_OWNER, COL_SIZE, COL_SPAN, COL_BASE, COL_NOTES, COL_COUNT };

/* Area flags (OS_DynamicArea 0's R4) */
#define DA_DOUBLY   (1u << 6)
#define DA_SPARSE   (1u << 10)
#define DA_PMP      (1u << 20)

typedef struct {
    int num;
    char name[32];
    char owner[32];         /* a task's name, "RISC OS", or "" */
    uint32_t base;          /* logical */
    uint32_t size;          /* logical, bytes in it now */
    uint32_t max;           /* logical, the most it can grow to */
    uint32_t span;          /* the address space it reserves (max, twice if doubly mapped) */
    uint32_t phys;          /* a physical memory pool's pages, in bytes (0 for other areas) */
    uint32_t flags;
    uint32_t was;           /* its size at the reading before (CH_GREW, CH_SHRANK) */
    int kind;
    int change, age;        /* age: readings since the change */
} area_t;

typedef struct {
    area_t a[AREAS_MAX];
    int n;                  /* areas there now are the first n_now; gone ones follow */
    int n_now;
    uint64_t span, size;    /* totals, of the areas there now */
    uint32_t pagesize;
    uint64_t ram;           /* the memory fitted */
    long clamp[3];          /* OS_DynamicArea 8: the cap on new areas' maximums: -1 means
                               "the RAM size"; [0] for areas asking for -1, [1] for others,
                               [2] for sparse ones; 0 = not known */
    char task[TASKS_MAX][32];
    int ntasks;
    int reason24;           /* read with OS_DynamicArea 24 (else 2) */
} snap_t;

/* Reads the areas and tasks now into s. 0 = done; -1 = the list couldn't
   be read (as_swi_error()). */
int areas_read(snap_t *s);

/* Marks now's changes since prev (NULL: the first reading, no marks), and
   adds prev's areas that have gone; a change stays marked, and a gone area
   in the list, for keep readings. */
void areas_compare(snap_t *now, const snap_t *prev, int keep);

/* Sorts the list (the areas there now, then the gone ones) by a column */
void areas_sort(snap_t *s, int col, int descending);

/* The area's notes: its change, and what it is */
void areas_notes(const area_t *a, char *out, size_t size);

/* The free address space, measured (above) */
typedef struct {
    uint64_t total;         /* bytes free */
    uint32_t biggest;       /* the most one new area could reserve (the cap, if there was room) */
    int n_biggest;          /* how many areas that big there was room for */
    int pieces;             /* areas made, in all */
    int full;               /* stopped at PROBE_MAX areas: there's at least total free */
    int left;               /* areas that couldn't be removed afterwards (0!) */
} probe_t;
/* piece: the maximum asked for first (RISC OS gives the least of it and
   its cap). 0 = done; -1 = not one area could be made. */
int areas_probe(probe_t *p, uint32_t piece);

/* Sizes as text: "131,072K"; "1,234.5MB" */
void fmt_k(uint64_t bytes, char *out, size_t size);
void fmt_mb(uint64_t bytes, char *out, size_t size);

/* The whole reading as text (Save list): totals, the cap, the areas, the
   tasks, and the free space if measured (p may be NULL) */
void areas_write(FILE *f, const snap_t *s, const probe_t *p, const char *when);
/* One line for each change in s (the log), each starting with when */
int areas_log(FILE *f, const snap_t *s, const char *when);

#endif
