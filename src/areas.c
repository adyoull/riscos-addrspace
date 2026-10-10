/*
 * areas.c - the dynamic areas and the free logical address space (see
 * areas.h).
 * Part of riscos-addrspace. GPL v2 or later.
 */
#include "areas.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- reading ---------------------------------------------------------------------- */

/* A name from another program's memory: up to the first control
   character, at most size - 1 letters */
static void copy_name(char *out, size_t size, const char *in)
{
    size_t i = 0;
    for (; in && i + 1 < size && (unsigned char)in[i] >= 32; i++)
        out[i] = in[i];
    out[i] = 0;
}

static void read_tasks(snap_t *s)
{
    uint32_t buf[64];               /* 16 tasks a time: handle, name, slot, flags */
    long r[10], next = 0;
    s->ntasks = 0;
    do {
        int got;
        memset(r, 0, sizeof(r));
        r[0] = next;
        r[1] = (long)(uintptr_t)buf;
        r[2] = (long)sizeof(buf);
        if (as_swi(TaskManager_EnumerateTasks, r))
            return;                 /* no Task Manager: no owners */
        got = (int)(((long)sizeof(buf) - r[2]) / 16);
        for (int i = 0; i < got && s->ntasks < TASKS_MAX; i++)
            copy_name(s->task[s->ntasks++], sizeof(s->task[0]), (const char *)(uintptr_t)buf[i * 4 + 1]);
        if (got <= 0)
            return;
        next = r[0];
    } while (next >= 0 && s->ntasks < TASKS_MAX);
}

/* The modules' titles (OS_Module 12: each module's base; its title's offset
   is at base + &10). A module with more than one instance is listed once. */
static void read_modules(snap_t *s)
{
    long r[10], num = 0, inst = 0;
    s->nmodules = 0;
    for (int guard = 0; guard < 4 * MODULES_MAX && s->nmodules < MODULES_MAX; guard++) {
        const uint32_t *hdr;
        char t[32];
        int k;
        memset(r, 0, sizeof(r));
        r[0] = 12;
        r[1] = num;
        r[2] = inst;
        if (as_swi(OS_Module, r))
            return;                 /* no more */
        num = r[1];
        inst = r[2];
        hdr = (const uint32_t *)(uintptr_t)r[3];
        if (!hdr || !hdr[4] || hdr[4] >= (1u << 20))
            continue;
        copy_name(t, sizeof(t), (const char *)hdr + hdr[4]);
        if (!t[0])
            continue;
        for (k = 0; k < s->nmodules && strcmp(s->module[k], t); k++)
            ;
        if (k == s->nmodules)
            memcpy(s->module[s->nmodules++], t, sizeof(t));
    }
}

/* Modules' areas whose names don't start with the module's title */
static const struct { const char *name, *module; } module_areas[] = {
    { "mmap#", "ARMEABISupport" },              /* a program's mmap */
    { "UnixLib stacks", "ARMEABISupport" },     /* UnixLib programs' stacks */
    { "Shared Libraries", "SOManager" },
    { "SO Manager", "SOManager" },
};

/* The length of a heap's program name: "Iris Heap", "Iris Heap 2" -> 4;
   "Iris MMap" (UnixLib's own mmap areas, before ARMEABISupport) -> 4 with
   *mmap set. 0 = neither. */
static size_t program_part(const char *name, int *mmap)
{
    size_t n = strlen(name);
    *mmap = 0;
    while (n && isdigit((unsigned char)name[n - 1]))
        n--;
    if (n < strlen(name)) {         /* " Heap 2": digits after a space */
        if (!n || name[n - 1] != ' ')
            return 0;
        n--;
    }
    if (n > 5 && !strncmp(name + n - 5, " Heap", 5))
        return n - 5;
    if (n == strlen(name) && n > 5 && !strncmp(name + n - 5, " MMap", 5)) {
        *mmap = 1;
        return n - 5;
    }
    return 0;
}

/* The running task that a program name (len letters) is: the same name (in
   any case), or one of them followed by a space and more ("ReelEGL" and
   "ReelEGL 0.1"). NULL if none. */
static const char *task_called(const snap_t *s, const char *name, size_t len)
{
    for (int i = 0; i < s->ntasks; i++) {
        const char *t = s->task[i];
        size_t tl = strlen(t);
        if (!tl)
            continue;
        if (tl == len && !strncasecmp(t, name, len))
            return t;
        if (tl > len && !strncasecmp(t, name, len) && t[len] == ' ')
            return t;
        if (len > tl && !strncasecmp(t, name, tl) && name[tl] == ' ')
            return t;
    }
    return NULL;
}

static void classify(const snap_t *s, area_t *a)
{
    size_t len;
    int mmap;
    const char *t;
    a->kind = KIND_OTHER;
    a->owner[0] = 0;
    if (a->num >= 0 && a->num < 256) {      /* numbered by RISC OS for its own */
        a->kind = KIND_OS;
        snprintf(a->owner, sizeof(a->owner), "RISC OS");
        return;
    }
    if (!strncmp(a->name, "mmap#", 5)) {    /* ARMEABISupport's, for a program's mmap */
        a->kind = KIND_MMAP;
        snprintf(a->owner, sizeof(a->owner), "ARMEABISupport");
        return;
    }
    if ((len = program_part(a->name, &mmap)) != 0) {
        t = task_called(s, a->name, len);
        a->kind = t ? (mmap ? KIND_UNIXLIB_MMAP : KIND_HEAP) : KIND_HEAP_LEFT;
        if (t)
            copy_name(a->owner, sizeof(a->owner), t);
        else {                      /* "(Iris)": not running */
            size_t l = len > sizeof(a->owner) - 3 ? sizeof(a->owner) - 3 : len;
            a->owner[0] = '(';
            memcpy(a->owner + 1, a->name, l);
            a->owner[l + 1] = ')';
            a->owner[l + 2] = 0;
        }
        return;
    }
    /* else: the longest task name that starts the area's, as a word */
    {
        size_t best = 0;
        for (int i = 0; i < s->ntasks; i++) {
            size_t tl = strlen(s->task[i]);
            if (tl > best && !strncasecmp(a->name, s->task[i], tl) && !isalnum((unsigned char)a->name[tl])) {
                best = tl;
                copy_name(a->owner, sizeof(a->owner), s->task[i]);
            }
        }
        if (best)
            return;
    }
    /* a module's: one of those named above, or the longest module title
       that starts the area's name, as a word */
    for (size_t i = 0; i < sizeof(module_areas) / sizeof(module_areas[0]); i++) {
        size_t l = strlen(module_areas[i].name);
        if (!strncasecmp(a->name, module_areas[i].name, l) && !isalnum((unsigned char)a->name[l])) {
            a->kind = KIND_MODULE;
            snprintf(a->owner, sizeof(a->owner), "%s", module_areas[i].module);
            return;
        }
    }
    {
        size_t best = 0;
        for (int i = 0; i < s->nmodules; i++) {
            size_t ml = strlen(s->module[i]);
            if (ml > best && !strncasecmp(a->name, s->module[i], ml) && !isalnum((unsigned char)a->name[ml])) {
                best = ml;
                a->kind = KIND_MODULE;
                copy_name(a->owner, sizeof(a->owner), s->module[i]);
            }
        }
    }
}

int areas_read(snap_t *s)
{
    long r[10], num = -1;
    s->n = s->n_now = 0;
    s->span = s->size = 0;
    s->pagesize = 4096;
    s->ram = 0;
    memset(s->clamp, 0, sizeof(s->clamp));

    memset(r, 0, sizeof(r));
    if (!as_swi(OS_ReadMemMapInfo, r) && r[0] > 0) {
        s->pagesize = (uint32_t)r[0];
        s->ram = (uint64_t)(uint32_t)r[0] * (uint32_t)r[1];
    }
    memset(r, 0, sizeof(r));
    r[0] = 8;                       /* read the caps (0s: change nothing) */
    if (!as_swi(OS_DynamicArea, r)) {
        s->clamp[0] = r[1];
        s->clamp[1] = r[2];
        s->clamp[2] = r[3];
    }
    read_tasks(s);
    read_modules(s);

    s->reason24 = 1;
    for (;;) {
        area_t *a;
        memset(r, 0, sizeof(r));
        r[0] = 3;
        r[1] = num;
        if (as_swi(OS_DynamicArea, r))
            return s->n ? 0 : -1;
        num = r[1];
        if (num == -1 || s->n >= AREAS_MAX)
            break;
        a = &s->a[s->n];
        memset(a, 0, sizeof(*a));
        memset(r, 0, sizeof(r));
        r[0] = s->reason24 ? 24 : 2;
        r[1] = num;
        if (as_swi(OS_DynamicArea, r)) {
            if (!s->reason24)
                continue;           /* gone between the two calls */
            s->reason24 = 0;        /* a RISC OS without 24 */
            memset(r, 0, sizeof(r));
            r[0] = 2;
            r[1] = num;
            if (as_swi(OS_DynamicArea, r))
                continue;
        }
        a->num = (int)num;
        a->size = (uint32_t)r[2];
        a->base = (uint32_t)r[3];
        a->flags = (uint32_t)r[4];
        a->max = (uint32_t)r[5];
        if (s->reason24 && (a->flags & DA_PMP))
            a->phys = (uint32_t)((uint64_t)(uint32_t)r[6] * s->pagesize);
        copy_name(a->name, sizeof(a->name), (const char *)(uintptr_t)r[8]);
        a->span = a->max;
        if ((a->flags & DA_DOUBLY) && a->max)
            a->span = a->max > 0x7FFFFFFFu ? 0xFFFFFFFFu : a->max * 2;
        classify(s, a);
        s->span += a->span;
        s->size += a->size;
        s->n++;
    }
    s->n_now = s->n;
    return 0;
}

/* ---- changes ---------------------------------------------------------------------- */

static const area_t *find(const snap_t *s, int n, const area_t *a)
{
    for (int i = 0; i < n; i++)
        if (s->a[i].num == a->num && !strcmp(s->a[i].name, a->name))
            return &s->a[i];
    return NULL;
}

void areas_compare(snap_t *now, const snap_t *prev, int keep)
{
    for (int i = 0; i < now->n_now; i++) {
        area_t *a = &now->a[i];
        const area_t *p = prev ? find(prev, prev->n_now, a) : NULL;
        a->was = a->size;
        if (!prev) {
            a->change = CH_NONE;
        } else if (!p) {
            a->change = CH_NEW;
            a->age = 0;
        } else if (p->size != a->size) {
            a->change = a->size > p->size ? CH_GREW : CH_SHRANK;
            a->was = p->size;
            a->age = 0;
        } else {
            a->change = p->change;
            a->was = p->was;
            a->age = p->age + 1;
            if (a->age >= keep)
                a->change = CH_NONE;
        }
    }
    now->n = now->n_now;
    for (int i = 0; prev && i < prev->n && now->n < AREAS_MAX; i++) {
        const area_t *p = &prev->a[i];
        area_t *g;
        int was_there = i < prev->n_now;
        if (!was_there && p->age + 1 >= keep)
            continue;               /* gone long enough ago */
        if (find(now, now->n_now, p))
            continue;               /* still there (or back: then it's marked new) */
        g = &now->a[now->n++];
        *g = *p;
        g->age = was_there ? 0 : p->age + 1;
        g->change = CH_GONE;
    }
}

/* ---- sorting ---------------------------------------------------------------------- */

static int sort_col, sort_desc;

static int cmp_u(uint64_t a, uint64_t b)
{
    return a < b ? -1 : a > b;
}

static int cmp(const void *x, const void *y)
{
    const area_t *a = x, *b = y;
    int c = 0;
    switch (sort_col) {
    case COL_NAME:  c = strcasecmp(a->name, b->name); break;
    case COL_OWNER:
        c = (!a->owner[0]) - (!b->owner[0]);        /* no owner last, either way */
        if (sort_desc)
            c = -c;
        if (!c)
            c = strcasecmp(a->owner, b->owner);
        break;
    case COL_SIZE:  c = cmp_u(a->size, b->size); break;
    case COL_SPAN:  c = cmp_u(a->span, b->span); break;
    case COL_BASE:  c = cmp_u(a->base, b->base); break;
    case COL_NOTES: c = a->kind - b->kind; break;
    }
    if (sort_desc)
        c = -c;
    if (!c)
        c = cmp_u(a->base, b->base);
    if (!c)
        c = a->num - b->num;
    return c;
}

void areas_sort(snap_t *s, int col, int descending)
{
    sort_col = col;
    sort_desc = descending;
    qsort(s->a, (size_t)s->n_now, sizeof(area_t), cmp);
    if (s->n > s->n_now)
        qsort(s->a + s->n_now, (size_t)(s->n - s->n_now), sizeof(area_t), cmp);
}

/* ---- text ------------------------------------------------------------------------- */

static void commas(uint64_t v, char *out, size_t size)
{
    char t[32], r[40];
    int n = snprintf(t, sizeof(t), "%llu", (unsigned long long)v), o = 0;
    for (int i = 0; i < n; i++) {
        if (i && (n - i) % 3 == 0)
            r[o++] = ',';
        r[o++] = t[i];
    }
    r[o] = 0;
    snprintf(out, size, "%s", r);
}

void fmt_k(uint64_t bytes, char *out, size_t size)
{
    char c[40];
    commas((bytes + 1023) / 1024, c, sizeof(c));
    snprintf(out, size, "%sK", c);
}

void fmt_mb(uint64_t bytes, char *out, size_t size)
{
    char c[40];
    uint64_t tenths = (bytes * 10 + (1 << 19)) >> 20;
    commas(tenths / 10, c, sizeof(c));
    if (tenths % 10)
        snprintf(out, size, "%s.%dMB", c, (int)(tenths % 10));
    else
        snprintf(out, size, "%sMB", c);
}

static void add(char *out, size_t size, const char *part)
{
    size_t n = strlen(out);
    if (n + 1 >= size)
        return;
    snprintf(out + n, size - n, "%s%s", n ? "; " : "", part);
}

void areas_notes(const area_t *a, char *out, size_t size)
{
    char t[96], k[32];
    if (!size)
        return;
    *out = 0;
    switch (a->change) {
    case CH_NEW:    add(out, size, "new"); break;
    case CH_GONE:   add(out, size, "gone"); break;
    case CH_GREW:
    case CH_SHRANK:
        fmt_k(a->was, k, sizeof(k));
        snprintf(t, sizeof(t), "%s from %s", a->change == CH_GREW ? "grew" : "shrank", k);
        add(out, size, t);
        break;
    }
    switch (a->kind) {
    case KIND_HEAP:         add(out, size, "UnixLib heap"); break;
    case KIND_HEAP_LEFT:    add(out, size, "its program isn't running: left behind?"); break;
    case KIND_UNIXLIB_MMAP: add(out, size, "UnixLib mmap"); break;
    case KIND_MMAP:         add(out, size, "a program's mmap"); break;
    case KIND_MODULE:       add(out, size, "a module's area"); break;
    }
    if (a->flags & DA_PMP) {
        fmt_k(a->phys, k, sizeof(k));
        snprintf(t, sizeof(t), "physical memory pool (%s)", k);
        add(out, size, t);
    }
    if (a->flags & DA_SPARSE)
        add(out, size, "sparse");
    if (a->flags & DA_DOUBLY)
        add(out, size, "doubly mapped: reserves twice its maximum");
}

void areas_largest(const probe_t *p, long cap, char *out, size_t size)
{
    char a[40], b[40];
    fmt_mb(p->biggest, a, sizeof(a));
    if (p->refused) {
        uint32_t under = p->refused;
        if (cap > 0 && (uint32_t)cap < under)
            under = (uint32_t)cap;
        fmt_mb(under, b, sizeof(b));
        snprintf(out, size, "between %s and %s", a, b);
    } else if (cap > 0 && p->biggest >= (uint32_t)cap) {
        snprintf(out, size, "at least %s, the most a new area can reserve", a);
    } else {
        snprintf(out, size, "at least %s", a);
    }
}

static void clamp_text(long c, char *out, size_t size)
{
    if (c == -1)
        snprintf(out, size, "the memory size");
    else if (c == 0)
        snprintf(out, size, "?");
    else
        fmt_mb((uint32_t)c, out, size);
}

void areas_write(FILE *f, const snap_t *s, const probe_t *p, const char *when)
{
    char a[40], b[40], c[40], notes[160], big[80];
    /* the owners' totals */
    typedef struct { char owner[32]; int n; uint64_t span, size; } owner_t;
    static owner_t by[AREAS_MAX];
    int nby = 0;

    fmt_mb(s->span, a, sizeof(a));
    fmt_mb(s->size, b, sizeof(b));
    fprintf(f, "Logical address space, %s\n\n", when);
    fmt_mb(s->ram, c, sizeof(c));
    fprintf(f, "Memory fitted: %s, in pages of %uK\n", c, (unsigned)(s->pagesize / 1024));
    fprintf(f, "Dynamic areas: %d, reserving %s of logical address space; %s in them\n", s->n_now, a, b);
    if (s->clamp[1]) {
        clamp_text(s->clamp[1], a, sizeof(a));
        clamp_text(s->clamp[0], b, sizeof(b));
        clamp_text(s->clamp[2], c, sizeof(c));
        fprintf(f, "A new area can reserve at most: %s (asking for any size: %s; sparse: %s)\n", a, b, c);
    }
    if (p) {
        fmt_mb(p->total, a, sizeof(a));
        areas_largest(p, s->clamp[1], big, sizeof(big));
        fprintf(f, "Free logical address space (measured): %s%s; the largest free piece: %s\n", p->full ? "at least " : "",
                a, big);
    }
    if (!s->reason24)
        fprintf(f, "(read with OS_DynamicArea 2: a physical memory pool's sizes are physical)\n");
    fprintf(f, "\n%-10s %-31s %-20s %12s %12s %-10s %s\n", "Area", "Name", "Owner", "Size", "Reserved", "Base", "Notes");
    for (int i = 0; i < s->n; i++) {
        const area_t *r = &s->a[i];
        fmt_k(r->size, a, sizeof(a));
        fmt_k(r->span, b, sizeof(b));
        areas_notes(r, notes, sizeof(notes));
        fprintf(f, "%-10d %-31s %-20s %12s %12s &%08X  %s\n", r->num, r->name, r->owner, a, b, (unsigned)r->base, notes);
        if (i >= s->n_now)
            continue;
        {
            int k;
            const char *o = r->owner[0] ? r->owner : "(not known)";
            for (k = 0; k < nby && strcmp(by[k].owner, o); k++)
                ;
            if (k == nby) {
                snprintf(by[k].owner, sizeof(by[k].owner), "%s", o);
                by[k].n = 0;
                by[k].span = by[k].size = 0;
                nby++;
            }
            by[k].n++;
            by[k].span += r->span;
            by[k].size += r->size;
        }
    }
    /* biggest first */
    for (int i = 1; i < nby; i++)
        for (int j = i; j > 0 && by[j].span > by[j - 1].span; j--) {
            owner_t t = by[j];
            by[j] = by[j - 1];
            by[j - 1] = t;
        }
    fprintf(f, "\nBy owner (owners in brackets aren't running):\n");
    for (int i = 0; i < nby; i++) {
        fmt_k(by[i].span, a, sizeof(a));
        fmt_k(by[i].size, b, sizeof(b));
        fprintf(f, "  %-24s %3d area%s  reserving %12s, %12s in them\n", by[i].owner, by[i].n, by[i].n == 1 ? " " : "s",
                a, b);
    }
    fprintf(f, "\nTasks running (%d):", s->ntasks);
    for (int i = 0; i < s->ntasks; i++)
        fprintf(f, "%s %s", i ? "," : "", s->task[i]);
    fprintf(f, "\n");
}

int areas_log(FILE *f, const snap_t *s, const char *when)
{
    char a[40], b[40], notes[160];
    int n = 0;
    for (int i = 0; i < s->n; i++) {
        const area_t *r = &s->a[i];
        if (r->change == CH_NONE || r->age)
            continue;
        fmt_k(r->size, a, sizeof(a));
        fmt_k(r->span, b, sizeof(b));
        areas_notes(r, notes, sizeof(notes));
        fprintf(f, "%s  %-31s %-20s size %s, reserves %s at &%08X: %s\n", when, r->name, r->owner[0] ? r->owner : "-", a, b,
                (unsigned)r->base, notes);
        n++;
    }
    if (n) {
        fmt_mb(s->span, a, sizeof(a));
        fmt_mb(s->size, b, sizeof(b));
        fprintf(f, "%s  (%d areas now, reserving %s; %s in them)\n", when, s->n_now, a, b);
    }
    return n;
}

/* ---- the free space --------------------------------------------------------------- */

int areas_probe(probe_t *p, uint32_t piece)
{
    static long made[PROBE_MAX];
    static uint32_t got[PROBE_MAX];
    uint32_t want = piece ? piece : 128u << 20;
    long r[10];
    int n = 0;
    uint32_t refused = 0;

    memset(p, 0, sizeof(*p));
    while (n < PROBE_MAX) {
        memset(r, 0, sizeof(r));
        r[0] = 0;                   /* create */
        r[1] = -1;
        r[2] = 0;                   /* nothing in it */
        r[3] = -1;                  /* wherever there's room */
        r[4] = 0x80;                /* not draggable in the Task Manager */
        r[5] = (long)want;
        r[8] = (long)(uintptr_t)PROBE_NAME;
        if (as_swi(OS_DynamicArea, r)) {
            if (want <= (1u << 20))
                break;
            refused = want;
            want /= 2;
            continue;
        }
        if (!n)
            p->refused = refused;   /* the first area made is the biggest */
        made[n] = r[1];
        got[n] = (uint32_t)r[5];
        p->total += got[n];
        if (got[n] > p->biggest)
            p->biggest = got[n];
        n++;
    }
    p->pieces = n;
    p->full = n == PROBE_MAX;
    for (int i = 0; i < n; i++) {
        if (got[i] == p->biggest)
            p->n_biggest++;
        memset(r, 0, sizeof(r));
        r[0] = 1;                   /* remove */
        r[1] = made[i];
        if (as_swi(OS_DynamicArea, r))
            p->left++;
    }
    return n ? 0 : -1;
}
