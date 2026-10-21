/*
 * ui_test.c - AddrSpace's front end (src/main.c) against a scripted fake
 * Wimp, with fake_os.c's dynamic areas and tasks: the icon bar icon, the
 * window opening and reading the list, what's plotted (and in which
 * colours), reading again as time passes (changes marked), sorting by a
 * heading, the menu (Read every, Measure free space, Save list, Log
 * changes) and Quit.
 * Built for arm-linux and run under qemu-arm (tests/host/run.sh): Wimp
 * blocks hold 32-bit pointers, as on RISC OS.
 * Part of riscos-addrspace. GPL v2 or later.
 */
#include <kernel.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_os.h"
#include "main.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                           printf(__VA_ARGS__); printf("\n"); } } while (0)
#define MB (1u << 20)

static const char *outdir;
static int fake_cs = 1000;
static int win_handle = 0x2000, win_made, bar_made, proginfo_made;
static char bar_sprite[16];
static int win_open, opens, closes;
static int wstate[9];                   /* handle, vis x0 y0 x1 y1, scroll x y, behind, flags */
static int extent[4];
static int forced, forced_tallest, forced_height;     /* Wimp_ForceRedraw: calls; the tallest band, all bands' height */
static int *menu_block, menus;
static char cli[400];
static int idle_bad;                    /* a PollIdle without a time, or a Poll with nulls while idle */
static int last_reason, last_r2;

/* What was plotted: text, colours */
typedef struct { char text[200]; int fg, bg; int y; } plotted_t;
static plotted_t plotted[2000];
static int nplotted;

static int step;
static int poll_script(int *block, int *reason);

static _kernel_oserror err = { 1, "fake error" };

_kernel_oserror *_kernel_swi(int swi, _kernel_swi_regs *in, _kernel_swi_regs *out)
{
    long r[10];
    swi &= ~0x20000;
    if (out != in)
        *out = *in;
    switch (swi) {
    case 0x42:                          /* OS_ReadMonotonicTime */
        out->r[0] = fake_cs;
        return NULL;
    case 0x05:                          /* OS_CLI */
        snprintf(cli, sizeof(cli), "%s", (const char *)(uintptr_t)in->r[0]);
        return NULL;
    case 0x08:                          /* OS_File 18 */
        return NULL;
    case 0x406C0: case 0x406C1:         /* Hourglass */
        return NULL;
    case 0x400C0:                       /* Wimp_Initialise */
        CHECK(in->r[1] == 0x4B534154 && !strcmp((const char *)(uintptr_t)in->r[2], "AddrSpace"), "Wimp_Initialise");
        out->r[0] = 380;
        out->r[1] = 0x55;
        return NULL;
    case 0x400C1: {                     /* Wimp_CreateWindow */
        const int *w = (const int *)(uintptr_t)in->r[1];
        const char *title = (const char *)(uintptr_t)w[18];
        if (!strcmp(title, "Logical address space")) {
            win_made++;
            memcpy(&wstate[1], w, 4 * sizeof(int));
            wstate[0] = win_handle;
            memcpy(extent, &w[10], sizeof(extent));
            out->r[0] = win_handle;
        } else {
            proginfo_made++;
            out->r[0] = 0x3000;
        }
        return NULL;
    }
    case 0x400C2: {                     /* Wimp_CreateIcon */
        const int *b = (const int *)(uintptr_t)in->r[1];
        CHECK(b[0] == -1, "on the icon bar");
        snprintf(bar_sprite, sizeof(bar_sprite), "%.12s", (const char *)&b[6]);
        bar_made++;
        out->r[0] = 0;
        return NULL;
    }
    case 0x400C5: {                     /* Wimp_OpenWindow */
        const int *b = (const int *)(uintptr_t)in->r[1];
        if (b[0] == win_handle) {
            memcpy(wstate, b, 7 * sizeof(int));
            win_open = 1;
            opens++;
        }
        return NULL;
    }
    case 0x400C6:                       /* Wimp_CloseWindow */
        win_open = 0;
        closes++;
        return NULL;
    case 0x400C7: case 0x400E1: {       /* Wimp_Poll, Wimp_PollIdle */
        int *block = (int *)(uintptr_t)in->r[1], reason;
        last_reason = swi;
        last_r2 = (int)in->r[2];
        if (swi == 0x400E1 && (in->r[0] & 1))
            idle_bad++;
        out->r[0] = poll_script(block, &reason);
        (void)reason;
        return NULL;
    }
    case 0x400C8: case 0x400CA: {       /* Wimp_RedrawWindow, Wimp_GetRectangle */
        int *b = (int *)(uintptr_t)in->r[1];
        static int more;
        if (swi == 0x400C8) {
            memcpy(&b[1], &wstate[1], 6 * sizeof(int));
            b[7] = wstate[1]; b[8] = wstate[2]; b[9] = wstate[3]; b[10] = wstate[4];
            more = 1;
            nplotted = 0;
        } else {
            more = 0;
        }
        out->r[0] = more;
        return NULL;
    }
    case 0x400CB: {                     /* Wimp_GetWindowState */
        int *b = (int *)(uintptr_t)in->r[1];
        if (b[0] != win_handle)
            return &err;
        memcpy(b, wstate, 7 * sizeof(int));
        b[7] = -1;
        b[8] = win_open ? 1 << 16 : 0;
        return NULL;
    }
    case 0x400CF: {                     /* Wimp_GetPointerInfo: Select */
        int *b = (int *)(uintptr_t)in->r[1];
        memset(b, 0, 5 * sizeof(int));
        b[2] = 4;
        return NULL;
    }
    case 0x400D1:                       /* Wimp_ForceRedraw */
        forced++;
        if (in->r[4] - in->r[2] > forced_tallest)
            forced_tallest = (int)(in->r[4] - in->r[2]);
        forced_height += (int)(in->r[4] - in->r[2]);
        return NULL;
    case 0x400D4:                       /* Wimp_CreateMenu */
        menu_block = (int *)(uintptr_t)in->r[1];
        menus++;
        return NULL;
    case 0x400D7:                       /* Wimp_SetExtent */
        memcpy(extent, (const int *)(uintptr_t)in->r[1], sizeof(extent));
        return NULL;
    case 0x400DD:                       /* Wimp_CloseDown */
        return NULL;
    case 0x400DF:                       /* Wimp_ReportError */
        printf("report: %s\n", ((_kernel_oserror *)(uintptr_t)in->r[0])->errmess);
        return NULL;
    case 0x400E2: {                     /* Wimp_PlotIcon */
        const int *ic = (const int *)(uintptr_t)in->r[1];
        unsigned f = (unsigned)ic[4];
        if (nplotted < 2000) {
            plotted_t *p = &plotted[nplotted++];
            snprintf(p->text, sizeof(p->text), "%s", (const char *)(uintptr_t)ic[5]);
            p->fg = (int)(f >> 24 & 15);
            p->bg = (int)(f >> 28 & 15);
            p->y = ic[3];
        }
        return NULL;
    }
    }
    /* the rest: the areas and tasks */
    for (int i = 0; i < 10; i++)
        r[i] = in->r[i];
    if (fake_os_swi(swi, r))
        return &err;
    for (int i = 0; i < 10; i++)
        out->r[i] = r[i];
    return NULL;
}

/* ---- what was plotted ----------------------------------------------------------- */

static const plotted_t *plotted_text(const char *t)
{
    for (int i = 0; i < nplotted; i++)
        if (strstr(plotted[i].text, t))
            return &plotted[i];
    return NULL;
}

/* The row (its name's cell) that shows an area */
static const plotted_t *row_of(const char *name)
{
    char t[64];
    snprintf(t, sizeof(t), " %s", name);
    for (int i = 0; i < nplotted; i++)
        if (!strcmp(plotted[i].text, t))
            return &plotted[i];
    return NULL;
}

static int redraw_events;
static void ask_redraw(int *block, int *reason)
{
    block[0] = win_handle;
    *reason = 1;
    redraw_events++;
}

static int file_has(const char *path, const char *what)
{
    static char buf[65536];
    FILE *f = fopen(path, "r");
    size_t n;
    if (!f)
        return 0;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    return strstr(buf, what) != NULL;
}

static void machine(void)
{
    fake_os_reset();
    fake_add(0, "System heap", 0xFAFF0000u, 64 << 10, 32 * MB, 0x80);
    fake_add(1, "Module area", 0x20000000u, 12 * MB, 256 * MB, 0x80);
    fake_add(300, "Matinee Heap", 0x40000000u, 40 * MB, 128 * MB, 0x80);
    fake_add(302, "Iris Heap", 0x50000000u, 90 * MB, 128 * MB, 0x80);
    fake_add(304, "ReelEGL Heap", 0x60000000u, 8 * MB, 128 * MB, 0x80);
    fake_task("Filer");
    fake_task("Matinee");
    fake_task("ReelEGL");
    fake_task("AddrSpace");
    fake_free[0] = 700 * MB;
    fake_npieces = 1;
}

/* ---- the script ------------------------------------------------------------------- */

static int poll_script(int *block, int *reason)
{
    char path[400];
    int x, y, desc;
    const snap_t *s;
    const plotted_t *p;
    memset(block, 0, 64);
    *reason = 0;
    switch (step++) {
    case 0:
        CHECK(win_made == 1 && bar_made == 1 && proginfo_made == 1, "made: %d %d %d", win_made, bar_made, proginfo_made);
        CHECK(!strcmp(bar_sprite, "!addrspace"), "sprite %s", bar_sprite);
        CHECK(ui_test_snap() == NULL, "nothing read before the window opens");
        CHECK(last_reason == 0x400C7, "closed: Wimp_Poll, without nulls");
        /* Select on the icon bar icon */
        block[0] = 640; block[1] = 20; block[2] = 4; block[3] = -2; block[4] = 0;
        return 6;
    case 1:
        CHECK(win_open && opens == 1, "the window opened");
        s = ui_test_snap();
        CHECK(s && s->n == 5, "read: %d", s ? s->n : -1);
        CHECK(strstr(ui_test_line(0), "5 dynamic areas reserve 672MB of logical address space; 150"), "%s",
              ui_test_line(0));
        CHECK(strstr(ui_test_line(1), "not measured yet"), "%s", ui_test_line(1));
        CHECK(strstr(ui_test_line(2), "at most 128MB. 4 tasks. Read every 2 s."), "%s", ui_test_line(2));
        CHECK(extent[1] <= -600, "extent %d", extent[1]);
        ask_redraw(block, reason);
        return 1;
    case 2:
        CHECK(plotted_text("5 dynamic areas reserve"), "line 0 plotted");
        CHECK(plotted_text("Reserved v"), "sorted by Reserved, biggest first");
        p = row_of("Iris Heap");
        CHECK(p && p->bg == 0, "Iris Heap's row");
        p = plotted_text("left behind?");
        CHECK(p && p->fg == 11, "left behind, in red");
        p = plotted_text("(Iris)");
        CHECK(p && p->fg == 11, "(Iris), in red");
        CHECK(plotted_text("131,072K "), "a size");
        CHECK(plotted_text(" &40000000"), "a base");
        CHECK(last_reason == 0x400E1 && last_r2 == 1000 + 200, "PollIdle until the next reading: %d", last_r2);
        /* time passes, nothing changes */
        forced = forced_tallest = forced_height = 0;
        fake_cs += 200;
        return 0;
    case 3:
        CHECK(ui_test_snap() && forced == 0, "read again, nothing changed: nothing redrawn (%d)", forced);
        /* things change; time passes */
        fake_find("Matinee Heap")->size += 8 * MB;
        fake_add(305, "ReelEGL Heap 2", 0x68000000u, 0, 128 * MB, 0x80);
        fake_remove("Iris Heap");
        fake_cs += 200;
        return 0;
    case 4:
        s = ui_test_snap();
        CHECK(s && s->n_now == 5 && s->n == 6, "read again: %d %d", s ? s->n_now : -1, s ? s->n : -1);
        CHECK(forced >= 2, "redrawn");
        /* only what changed: the totals line and rows, not the whole window (6 rows of 40 and 2 lines of 44 at most) */
        CHECK(forced_tallest > 0 && forced_tallest < 0x1000 && forced_height <= 6 * 40 + 2 * 44,
              "only what changed redrawn: %d bands, tallest %d, %d in all", forced, forced_tallest, forced_height);
        ask_redraw(block, reason);
        return 1;
    case 5:
        p = row_of("ReelEGL Heap 2");
        CHECK(p && p->bg == 10, "new: green");
        p = row_of("Matinee Heap");
        CHECK(p && p->bg == 9, "grew: yellow");
        p = row_of("Iris Heap");
        CHECK(p && p->fg == 4, "gone: grey");
        CHECK(plotted_text("grew from 40,960K"), "grew from");
        /* click on the Name heading */
        CHECK(ui_test_heading_xy(COL_NAME, &x, &y) == 0, "heading");
        block[0] = x; block[1] = y; block[2] = 4; block[3] = win_handle; block[4] = -1;
        return 6;
    case 6:
        CHECK(ui_test_sort(&desc) == COL_NAME && !desc, "sorted by name");
        s = ui_test_snap();
        CHECK(!strcmp(s->a[0].name, "Matinee Heap") && !strcmp(s->a[s->n_now - 1].name, "System heap"), "%s .. %s",
              s->a[0].name, s->a[s->n_now - 1].name);
        ui_test_heading_xy(COL_NAME, &x, &y);
        block[0] = x; block[1] = y; block[2] = 4; block[3] = win_handle; block[4] = -1;
        return 6;
    case 7:
        CHECK(ui_test_sort(&desc) == COL_NAME && desc, "turned round");
        /* Menu over the window */
        block[0] = 400; block[1] = 600; block[2] = 2; block[3] = win_handle; block[4] = -1;
        return 6;
    case 8:
        CHECK(menus == 1 && menu_block, "menu");
        {
            const char *t = (const char *)(uintptr_t)menu_block[7 + MB_MEASURE * 6 + 3];
            CHECK(!strcmp(t, "Measure free space"), "item: %s", t);
            CHECK(menu_block[7 + MB_INFO * 6 + 1] == 0x3000, "Info's window");
        }
        block[0] = MB_MEASURE; block[1] = -1;
        return 9;
    case 9:
        CHECK(ui_test_probe() && ui_test_probe()->total == 700 * MB, "measured");
        CHECK(fake_nareas == 5 + 0, "the probe's areas went: %d", fake_nareas);
        CHECK(strstr(ui_test_line(1), "700MB. The largest free piece: at least 128MB, the most a new area can reserve."),
              "%s", ui_test_line(1));
        block[0] = MB_EVERY; block[1] = 2; block[2] = -1;
        return 9;
    case 10:
        CHECK(ui_test_every() == 5, "every %d", ui_test_every());
        CHECK(strstr(ui_test_line(2), "Read every 5 s"), "%s", ui_test_line(2));
        block[0] = MB_SAVE; block[1] = -1;
        return 9;
    case 11:
        snprintf(path, sizeof(path), "%s/AddrSpaceList", outdir);
        CHECK(file_has(path, "AddrSpace 0.1"), "saved");
        CHECK(file_has(path, "By owner"), "by owner");
        CHECK(file_has(path, "Free logical address space (measured): 700MB"), "free");
        CHECK(file_has(path, "700MB; the largest free piece: at least 128MB"), "the largest free piece");
        CHECK(strstr(cli, "Filer_Run ") && strstr(cli, "AddrSpaceList"), "opened: %s", cli);
        block[0] = MB_LOG; block[1] = -1;
        return 9;
    case 12:
        CHECK(ui_test_logging(), "logging");
        snprintf(path, sizeof(path), "%s/AddrSpaceLog", outdir);
        CHECK(file_has(path, "logging from") && file_has(path, "Changes:"), "the log's start");
        fake_add(306, "Iris Heap", 0x50000000u, 1 * MB, 128 * MB, 0x80);
        fake_task("Iris");
        fake_cs += 500;
        return 0;
    case 13:
        snprintf(path, sizeof(path), "%s/AddrSpaceLog", outdir);
        CHECK(file_has(path, "Iris Heap") && file_has(path, "new; UnixLib heap"), "logged");
        block[0] = MB_MEASURE; block[1] = -1;
        return 9;
    case 14:
        snprintf(path, sizeof(path), "%s/AddrSpaceLog", outdir);
        CHECK(file_has(path, "  free: 700MB; the largest free piece: at least 128MB, the most a new area can reserve\n"),
              "the measurement logged");
        /* closed: still read, for the log */
        block[0] = win_handle;
        return 3;
    case 15:
        CHECK(!win_open && closes == 1, "closed");
        CHECK(last_reason == 0x400E1, "logging: PollIdle while closed");
        CHECK(strstr(ui_test_line(2), "changes are logged"), "%s", ui_test_line(2));
        block[0] = MB_LOG; block[1] = -1;
        return 9;
    case 16:
        CHECK(!ui_test_logging(), "not logging");
        block[0] = 0;
        return 0;
    case 17:
        CHECK(last_reason == 0x400C7, "closed, not logging: Wimp_Poll");
        block[0] = 256; block[4] = 0;       /* Message_Quit */
        return 17;
    }
    CHECK(0, "the script ran out");
    block[4] = 0;
    return 17;
}

int main(int argc, char **argv)
{
    char scrap[300];
    outdir = argc > 1 ? argv[1] : "/tmp";
    snprintf(scrap, sizeof(scrap), "%s", outdir);
    setenv("Wimp$ScrapDir", scrap, 1);
    unsetenv("AddrSpace$Log");
    {
        char p[400];
        snprintf(p, sizeof(p), "%s/AddrSpaceLog", outdir);
        remove(p);
        snprintf(p, sizeof(p), "%s/AddrSpaceList", outdir);
        remove(p);
    }
    machine();
    CHECK(addrspace_main(0, NULL) == 0, "quit");
    CHECK(step == 18, "the script: %d steps", step);
    CHECK(!idle_bad, "PollIdle with nulls masked");
    printf("ui_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
