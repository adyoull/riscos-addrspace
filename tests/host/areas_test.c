/*
 * areas_test.c - areas.c against fake_os.c: reading the list (owners,
 * left-behind heaps, physical memory pools, doubly-mapped areas, a RISC OS
 * without OS_DynamicArea 24, many tasks), changes, sorting, measuring the
 * free space, and the text written.
 * Built for arm-linux and run under qemu-arm (tests/host/run.sh): the
 * Task Manager's blocks hold 32-bit pointers, as on RISC OS.
 * Part of riscos-addrspace. GPL v2 or later.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "areas.h"
#include "fake_os.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                           printf(__VA_ARGS__); printf("\n"); } } while (0)

int as_swi(int swi, long *r) { return fake_os_swi(swi, r); }
const char *as_swi_error(void) { return "fake error"; }

#define MB (1u << 20)
#define K  1024u

static snap_t a, b, c;

static const area_t *get(const snap_t *s, const char *name)
{
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->a[i].name, name))
            return &s->a[i];
    return NULL;
}

static void machine(void)
{
    fake_os_reset();
    fake_add(0, "System heap", 0xFAFF0000u, 64 * K, 32 * MB, 0x80);
    fake_add(1, "Module area", 0x20000000u, 12 * MB, 256 * MB, 0x80);
    fake_add(5, "RAM disc", 0, 0, 0, 0x80 | (1u << 20) | (1u << 8));    /* a PMP with no logical space */
    fake_add(6, "Free pool", 0, 0, 0, 0x80 | (1u << 20) | (1u << 8));
    fake_find("RAM disc")->phys_pages = 1000;
    fake_find("RAM disc")->phys_max_pages = 524288;                     /* 2GB: physical, not logical */
    fake_add(300, "Matinee Heap", 0x40000000u, 40 * MB, 128 * MB, 0x80);
    fake_add(301, "Matinee Heap 2", 0x48000000u, 4 * MB, 128 * MB, 0x80);
    fake_add(302, "Iris Heap", 0x50000000u, 90 * MB, 128 * MB, 0x80);        /* Iris has quit */
    fake_add(303, "mmap#0", 0x58000000u, 128 * MB + 4 * K, 128 * MB + 4 * K, 0x80);
    fake_add(304, "ReelEGL Heap", 0x60000000u, 8 * MB, 128 * MB, 0x80);
    fake_add(305, "Netsurf cache", 0x68000000u, 1 * MB, 16 * MB, 0x80);
    fake_add(306, "Screen copy", 0x70000000u, 8 * MB, 8 * MB, 0x80 | (1u << 6));  /* doubly mapped */
    fake_add(307, "OldApp MMap", 0x72000000u, 1 * MB, 3 * MB, 0x80);          /* UnixLib's own mmap, app gone */
    fake_task("Filer");
    fake_task("Matinee");
    fake_task("ReelEGL 0.1.27");
    fake_task("NetSurf");
    for (int i = 0; i < 20; i++) {                  /* more than one Task Manager call's worth */
        char t[32];
        snprintf(t, sizeof(t), "Task%d", i);
        fake_task(t);
    }
}

static void test_read(void)
{
    const area_t *x;
    machine();
    CHECK(areas_read(&a) == 0, "read");
    CHECK(a.n == fake_nareas && a.n_now == a.n, "%d areas", a.n);
    CHECK(a.ntasks == fake_ntasks, "%d tasks", a.ntasks);
    CHECK(fake_task_calls >= 2, "the Task Manager asked %d times", fake_task_calls);
    CHECK(a.reason24, "read with 24");
    CHECK(a.pagesize == 4096 && a.ram == 4ull << 30, "memory %u %llu", (unsigned)a.pagesize, (unsigned long long)a.ram);
    CHECK(a.clamp[1] == 128 << 20, "cap");

    x = get(&a, "System heap");
    CHECK(x && x->kind == KIND_OS && !strcmp(x->owner, "RISC OS"), "System heap's owner");
    x = get(&a, "RAM disc");
    CHECK(x && x->span == 0 && x->phys == 1000 * 4096u, "RAM disc: span %u phys %u", x ? x->span : 1, x ? x->phys : 0);
    x = get(&a, "Matinee Heap");
    CHECK(x && x->kind == KIND_HEAP && !strcmp(x->owner, "Matinee") && x->span == 128 * MB, "Matinee Heap");
    x = get(&a, "Matinee Heap 2");
    CHECK(x && x->kind == KIND_HEAP && !strcmp(x->owner, "Matinee"), "Matinee Heap 2");
    x = get(&a, "Iris Heap");
    CHECK(x && x->kind == KIND_HEAP_LEFT && !strcmp(x->owner, "(Iris)"), "Iris Heap: %d %s", x ? x->kind : -1,
          x ? x->owner : "");
    x = get(&a, "ReelEGL Heap");
    CHECK(x && x->kind == KIND_HEAP && !strcmp(x->owner, "ReelEGL 0.1.27"), "ReelEGL Heap: %s", x ? x->owner : "");
    x = get(&a, "mmap#0");
    CHECK(x && x->kind == KIND_MMAP && !x->owner[0], "mmap#0");
    x = get(&a, "Netsurf cache");
    CHECK(x && x->kind == KIND_OTHER && !strcmp(x->owner, "NetSurf"), "Netsurf cache: %s", x ? x->owner : "");
    x = get(&a, "Screen copy");
    CHECK(x && x->span == 16 * MB, "doubly mapped: span %u", x ? x->span : 0);
    x = get(&a, "OldApp MMap");
    CHECK(x && x->kind == KIND_HEAP_LEFT && !strcmp(x->owner, "(OldApp)"), "OldApp MMap");
    {
        uint64_t span = 32 * MB + 256 * MB + 4 * 128 * MB + 128 * MB + 4 * K + 16 * MB + 16 * MB + 3 * MB;
        CHECK(a.span == span, "span %llu, not %llu", (unsigned long long)a.span, (unsigned long long)span);
    }

    /* without OS_DynamicArea 24: 2 instead (a pool's sizes are physical then) */
    fake_no_reason24 = 1;
    CHECK(areas_read(&b) == 0 && !b.reason24 && b.n == a.n, "read with 2");
    x = get(&b, "RAM disc");
    CHECK(x && x->max == 524288u * 4096u, "RAM disc's max through 2: %u", x ? x->max : 0);
    fake_no_reason24 = 0;

    /* the list can't be read */
    fake_enum_fails = 1;
    CHECK(areas_read(&b) == -1, "a failed read");
    fake_enum_fails = 0;

    /* names: heaps' program parts */
    fake_os_reset();
    fake_add(300, "Heap", 0, 0, MB, 0);             /* no program name */
    fake_add(301, "My Heap X", 0, 0, MB, 0);        /* not a heap's name */
    fake_add(302, "Big Heap2", 0, 0, MB, 0);        /* digits without a space */
    fake_add(303, "Long program name here Heap 12", 0, 0, MB, 0);
    CHECK(areas_read(&b) == 0 && b.n == 4, "names");
    CHECK(get(&b, "Heap")->kind == KIND_OTHER, "Heap");
    CHECK(get(&b, "My Heap X")->kind == KIND_OTHER, "My Heap X");
    CHECK(get(&b, "Big Heap2")->kind == KIND_OTHER, "Big Heap2");
    x = get(&b, "Long program name here Heap 12");
    CHECK(x->kind == KIND_HEAP_LEFT && !strcmp(x->owner, "(Long program name here)"), "long: %s", x->owner);
}

static void test_changes(void)
{
    const area_t *x;
    machine();
    areas_read(&a);
    areas_compare(&a, NULL, 3);
    for (int i = 0; i < a.n; i++)
        CHECK(a.a[i].change == CH_NONE, "first reading: no changes");

    fake_find("Matinee Heap")->size += 8 * MB;
    fake_find("ReelEGL Heap")->size -= 4 * MB;
    fake_remove("Iris Heap");
    fake_add(320, "Iris Heap", 0x50000000u, 1 * MB, 128 * MB, 0x80);     /* Iris again */
    fake_task("Iris");
    fake_remove("mmap#0");
    fake_add(310, "ReelEGL Heap 2", 0x78000000u, 0, 128 * MB, 0x80);
    areas_read(&b);
    areas_compare(&b, &a, 3);
    x = get(&b, "Matinee Heap");
    CHECK(x->change == CH_GREW && x->was == 40 * MB, "grew");
    x = get(&b, "ReelEGL Heap");
    CHECK(x->change == CH_SHRANK && x->was == 8 * MB, "shrank");
    CHECK(get(&b, "ReelEGL Heap 2")->change == CH_NEW, "new");
    CHECK(b.n == b.n_now + 2, "two gone: %d %d", b.n, b.n_now);
    for (int i = b.n_now; i < b.n; i++)
        CHECK(b.a[i].change == CH_GONE && (b.a[i].num == 302 || b.a[i].num == 303), "gone %d", b.a[i].num);
    /* the same name with a new number: a new area */
    x = NULL;
    for (int i = 0; i < b.n_now; i++)
        if (b.a[i].num == 320)
            x = &b.a[i];
    CHECK(x && x->change == CH_NEW && x->kind == KIND_HEAP, "Iris's new heap");
    CHECK(b.span == a.span - (128 * MB + 4 * K) + 128 * MB, "totals leave the gone out");

    /* marks last keep readings; gone areas then leave the list */
    areas_read(&c);
    areas_compare(&c, &b, 3);
    CHECK(get(&c, "Matinee Heap")->change == CH_GREW && get(&c, "Matinee Heap")->age == 1, "still marked");
    CHECK(c.n == c.n_now + 2, "gone still listed");
    areas_read(&a);
    areas_compare(&a, &c, 3);
    areas_read(&b);
    areas_compare(&b, &a, 3);
    CHECK(get(&b, "Matinee Heap")->change == CH_NONE, "the mark goes");
    CHECK(b.n == b.n_now, "gone ones leave: %d %d", b.n, b.n_now);

    /* a gone area that comes back is new, and listed once */
    fake_remove("Netsurf cache");
    areas_read(&a);
    areas_compare(&a, &b, 5);
    fake_add(305, "Netsurf cache", 0x68000000u, 1 * MB, 16 * MB, 0x80);
    areas_read(&c);
    areas_compare(&c, &a, 5);
    {
        int n = 0;
        for (int i = 0; i < c.n; i++)
            if (!strcmp(c.a[i].name, "Netsurf cache"))
                n++, x = &c.a[i];
        CHECK(n == 1 && x->change == CH_NEW, "back: %d", n);
    }
}

static int sorted(const snap_t *s, int col, int desc)
{
    for (int i = 1; i < s->n_now; i++) {
        const area_t *p = &s->a[i - 1], *q = &s->a[i];
        long long d = 0;
        switch (col) {
        case COL_NAME: d = strcasecmp(p->name, q->name); break;
        case COL_SIZE: d = (long long)p->size - q->size; break;
        case COL_SPAN: d = (long long)p->span - q->span; break;
        case COL_BASE: d = (long long)p->base - q->base; break;
        }
        if (desc ? d < 0 : d > 0)
            return 0;
    }
    return 1;
}

static void test_sort(void)
{
    machine();
    areas_read(&a);
    fake_remove("mmap#0");
    areas_read(&b);
    areas_compare(&b, &a, 3);
    for (int col = 0; col < COL_COUNT; col++)
        for (int d = 0; d < 2; d++) {
            areas_sort(&b, col, d);
            CHECK(sorted(&b, col, d), "sorted by %d %d", col, d);
            CHECK(b.a[b.n - 1].change == CH_GONE, "gone ones last");
        }
    areas_sort(&b, COL_OWNER, 0);
    CHECK(!b.a[b.n_now - 1].owner[0], "no owner last");
    CHECK(strcasecmp(b.a[0].owner, b.a[1].owner) <= 0, "owners in order");
}

static void test_probe(void)
{
    probe_t p;
    machine();
    fake_free[0] = 300 * MB;
    fake_free[1] = 1000 * MB;
    fake_free[2] = 512 * K;                 /* too small to count */
    fake_npieces = 3;
    CHECK(areas_probe(&p, 128 * MB) == 0, "probe");
    CHECK(p.total == 1300 * MB, "free %llu MB", (unsigned long long)(p.total >> 20));
    CHECK(p.biggest == 128 * MB && p.n_biggest == 2 + 7, "%u MB x %d", (unsigned)(p.biggest >> 20), p.n_biggest);
    CHECK(!p.full && !p.left, "full %d left %d", p.full, p.left);
    CHECK(fake_made == p.pieces && fake_removed == p.pieces, "made %d removed %d", fake_made, fake_removed);
    CHECK(fake_free[0] == 300 * MB && fake_free[1] == 1000 * MB, "all given back");
    for (int i = 0; i < fake_nareas; i++)
        CHECK(strcmp(fake_areas[i].name, PROBE_NAME), "a probe area left");

    /* a lower cap: smaller areas; asking for more gets the cap */
    fake_clamp[1] = 32 << 20;
    areas_probe(&p, 128 * MB);
    CHECK(p.total == 1300 * MB && p.biggest == 32 * MB && p.n_biggest == 9 + 31, "capped: %u x %d",
          (unsigned)(p.biggest >> 20), p.n_biggest);
    fake_clamp[1] = 128 << 20;

    /* nothing free */
    fake_free[0] = fake_free[1] = 0;
    CHECK(areas_probe(&p, 128 * MB) == -1 && p.total == 0, "none");

    /* more than PROBE_MAX pieces: stops, and says so */
    fake_free[0] = 600 * MB;
    fake_clamp[1] = 1 << 20;
    areas_probe(&p, 1 * MB);
    CHECK(p.full && p.total == (uint64_t)PROBE_MAX * MB, "full %d %llu", p.full, (unsigned long long)p.total);
    fake_clamp[1] = 128 << 20;

    /* areas that won't go are counted */
    fake_free[0] = 256 * MB;
    fake_refuse_remove = 1;
    areas_probe(&p, 128 * MB);
    CHECK(p.left == 2, "left %d", p.left);
}

static void test_text(void)
{
    char t[64], buf[16384];
    FILE *f;
    size_t n;
    probe_t p;
    fmt_k(0, t, sizeof(t));
    CHECK(!strcmp(t, "0K"), "%s", t);
    fmt_k(128 * MB, t, sizeof(t));
    CHECK(!strcmp(t, "131,072K"), "%s", t);
    fmt_k(4000000000u, t, sizeof(t));
    CHECK(!strcmp(t, "3,906,250K"), "%s", t);
    fmt_mb(128 * MB, t, sizeof(t));
    CHECK(!strcmp(t, "128MB"), "%s", t);
    fmt_mb(1536ull * MB + 512 * K, t, sizeof(t));
    CHECK(!strcmp(t, "1,536.5MB"), "%s", t);

    machine();
    fake_free[0] = 256 * MB;
    fake_npieces = 1;
    areas_read(&a);
    areas_probe(&p, 128 * MB);
    f = tmpfile();
    areas_write(f, &a, &p, "now");
    rewind(f);
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    CHECK(strstr(buf, "Dynamic areas: 12, reserving"), "totals");
    CHECK(strstr(buf, "A new area can reserve at most: 128MB"), "the cap");
    CHECK(strstr(buf, "Free logical address space (measured): 256MB; room for 2 more 128MB areas"), "free");
    CHECK(strstr(buf, "By owner"), "by owner");
    {
        const char *m = strstr(buf, "\n  Matinee ");
        const char *e = m ? strchr(m + 1, '\n') : NULL;
        const char *k = m ? strstr(m, "2 areas  reserving     262,144K") : NULL;
        CHECK(m && k && k < e, "Matinee's total");
    }
    CHECK(strstr(buf, "  (Iris) "), "Iris's");
    CHECK(strstr(buf, "its program isn't running: left behind?"), "left behind");
    CHECK(strstr(buf, "Tasks running (24): Filer, Matinee"), "tasks");

    fake_remove("Iris Heap");
    areas_read(&b);
    areas_compare(&b, &a, 3);
    f = tmpfile();
    CHECK(areas_log(f, &b, "12:00:00") == 1, "one change");
    rewind(f);
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    CHECK(strstr(buf, "12:00:00  Iris Heap") && strstr(buf, "gone"), "logged: %s", buf);
    CHECK(strstr(buf, "(11 areas now"), "logged totals");
    f = tmpfile();
    areas_compare(&a, &b, 3);
    CHECK(areas_log(f, &a, "x") == 1, "back");       /* (Iris Heap, new again) */
    fclose(f);
}

int main(void)
{
    test_read();
    test_changes();
    test_sort();
    test_probe();
    test_text();
    printf("areas_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
