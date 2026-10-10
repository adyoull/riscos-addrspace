/*
 * main.c - AddrSpace's desktop front end: an icon bar icon, and a window
 * listing the dynamic areas, what they reserve of the logical address
 * space and who they probably belong to (areas.c), read again every few
 * seconds with the changes marked.
 *
 *   the window:  three lines of totals (the areas' reservations, the free
 *                space when measured, the cap on new areas), then the
 *                list: Name, Owner, Size, Reserved, Base, Notes. Click a
 *                column's heading to sort by it (again to turn it round).
 *                New areas are green, ones that grew yellow, shrank blue;
 *                gone ones stay a while, in grey. A heap whose program
 *                isn't running is red.
 *   the menu:    Info, Refresh every, Refresh now, Measure free space,
 *                Save list (a text file, opened), Log changes (to a file
 *                in the scrap directory: every change, as it's seen,
 *                with the time), Quit.
 *
 * AddrSpace must not change what it measures: its heap is in its
 * WimpSlot (no __dynamic_da_name), it never mallocs (the lists are
 * static), and the only areas it makes are Measure free space's, which
 * are removed before it next reads the list.
 * Part of riscos-addrspace. GPL v2 or later.
 */
#include <kernel.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "areas.h"
#include "main.h"
#include "proginfo.h"
#include "version.h"

#define OS_CLI                     0x05
#define OS_File                    0x08
#define OS_ReadMonotonicTime       0x42
#define Wimp_Initialise            0x400C0
#define Wimp_CreateWindow          0x400C1
#define Wimp_CreateIcon            0x400C2
#define Wimp_OpenWindow            0x400C5
#define Wimp_CloseWindow           0x400C6
#define Wimp_Poll                  0x400C7
#define Wimp_RedrawWindow          0x400C8
#define Wimp_GetRectangle          0x400CA
#define Wimp_GetWindowState        0x400CB
#define Wimp_GetPointerInfo        0x400CF
#define Wimp_ForceRedraw           0x400D1
#define Wimp_CreateMenu            0x400D4
#define Wimp_SetExtent             0x400D7
#define Wimp_CloseDown             0x400DD
#define Wimp_ReportError           0x400DF
#define Wimp_PollIdle              0x400E1
#define Wimp_PlotIcon              0x400E2
#define Hourglass_On               0x406C0
#define Hourglass_Off              0x406C1

#define APP       "AddrSpace"
#define ICON      "!addrspace"
#define PURPOSE   "Logical address space"

#define KEEP_S    30                /* changes stay marked, and gone areas listed, this long */

#ifdef __riscos__
#define SEP "."
#else
#define SEP "/"                     /* the host test's files */
#endif

/* The window's layout (work area units: y down from 0) */
#define LINE_H    44
#define TOP_PAD   8
#define HEAD_Y1   (-(TOP_PAD + 3 * LINE_H + 12))
#define HEAD_H    44
#define ROW_H     40
#define ROWS_Y1   (HEAD_Y1 - HEAD_H - 4)
static const int col_x[COL_COUNT + 1] = { 8, 400, 680, 864, 1048, 1212, 2040 };
static const char *const col_name[COL_COUNT] = { "Name", "Owner", "Size", "Reserved", "Base", "Notes" };
#define WA_W      (2040 + 8)

static _kernel_oserror *swi(int n, _kernel_swi_regs *r) { return _kernel_swi(n, r, r); }

static struct {
    int task, win, bar_icon, proginfo;
    int open;                       /* the window is open */
    snap_t snap[2];
    int cur;                        /* snap[cur] is the latest reading */
    int have, failed;
    char err[160];
    probe_t probe;
    int probed, probe_failed;
    char probe_when[16];
    int sort_col, sort_desc;
    int every;                      /* seconds between readings */
    int next_cs;
    int logging;
    char title[40];
    char line[3][200];
    int rows_shown;                 /* the extent's rows */
} S;

static const int every_s[] = { 1, 2, 5, 10, 30 };
#define N_EVERY ((int)(sizeof(every_s) / sizeof(every_s[0])))

/* ---- small things ----------------------------------------------------------------- */

static int now_cs(void)
{
    _kernel_swi_regs r;
    return swi(OS_ReadMonotonicTime, &r) ? (int)(clock() * 100 / CLOCKS_PER_SEC) : (int)r.r[0];
}

static void hourglass(int on)
{
    _kernel_swi_regs r;
    swi(on ? Hourglass_On : Hourglass_Off, &r);
}

static void report(const char *fmt, ...)
{
    _kernel_oserror e;
    _kernel_swi_regs r;
    va_list a;
    va_start(a, fmt);
    e.errnum = 0;
    vsnprintf(e.errmess, sizeof(e.errmess), fmt, a);
    va_end(a);
    r.r[0] = (intptr_t)&e;
    r.r[1] = 1 | 16;                /* OK, no "Error from" */
    r.r[2] = (intptr_t)APP;
    swi(Wimp_ReportError, &r);
}

static void clock_text(char *out, size_t size, int with_date)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (!tm || !strftime(out, size, with_date ? "%d-%b-%Y %H:%M:%S" : "%H:%M:%S", tm))
        snprintf(out, size, "?");
}

static void set_type(const char *path, int type)
{
    _kernel_swi_regs r;
    r.r[0] = 18;                    /* OS_File 18: set the filetype */
    r.r[1] = (intptr_t)path;
    r.r[2] = type;
    swi(OS_File, &r);
}

/* <Wimp$ScrapDir>.leaf, or 0 if there's no scrap directory */
static int scrap_path(const char *leaf, char *out, size_t size)
{
    const char *scrap = getenv("Wimp$ScrapDir");
    if (!scrap || !*scrap) {
        report("<Wimp$ScrapDir> isn't set: is !Scrap missing?");
        return 0;
    }
    snprintf(out, size, "%s" SEP "%s", scrap, leaf);
    return 1;
}

/* ---- reading ---------------------------------------------------------------------- */

static snap_t *cur(void)
{
    return &S.snap[S.cur];
}

static void log_changes(void)
{
    char path[300], when[32];
    FILE *f;
    if (!S.logging || !scrap_path("AddrSpaceLog", path, sizeof(path)))
        return;
    if ((f = fopen(path, "a")) == NULL)
        return;
    clock_text(when, sizeof(when), 0);
    areas_log(f, cur(), when);
    fclose(f);
}

static void make_lines(void)
{
    const snap_t *s = cur();
    char a[40], b[40];
    if (!S.have) {
        snprintf(S.line[0], sizeof(S.line[0]), "The dynamic areas couldn't be read: %s", S.err);
    } else {
        fmt_mb(s->span, a, sizeof(a));
        fmt_mb(s->size, b, sizeof(b));
        snprintf(S.line[0], sizeof(S.line[0]), "%d dynamic areas reserve %s of logical address space; %s is in them.",
                 s->n_now, a, b);
        if (S.failed)
            snprintf(S.line[0] + strlen(S.line[0]), sizeof(S.line[0]) - strlen(S.line[0]),
                     " (The last reading failed: %s)", S.err);
    }
    if (S.probe_failed) {
        snprintf(S.line[1], sizeof(S.line[1]), "Free, at %s: none. Not even a 1MB area could be made.", S.probe_when);
    } else if (S.probed) {
        char big[80];
        fmt_mb(S.probe.total, a, sizeof(a));
        areas_largest(&S.probe, S.have ? s->clamp[1] : 0, big, sizeof(big));
        snprintf(S.line[1], sizeof(S.line[1]), "Free, at %s: %s%s. The largest free piece: %s.", S.probe_when,
                 S.probe.full ? "at least " : "", a, big);
    } else {
        snprintf(S.line[1], sizeof(S.line[1]), "Free: not measured yet (choose Measure free space on the menu).");
    }
    if (S.have && s->clamp[1] > 0) {
        fmt_mb((uint32_t)s->clamp[1], a, sizeof(a));
        snprintf(b, sizeof(b), "%s", a);
    } else {
        snprintf(b, sizeof(b), S.have && s->clamp[1] == -1 ? "as much as there's memory" : "?");
    }
    snprintf(S.line[2], sizeof(S.line[2]), "A new area can reserve at most %s. %d tasks. Read every %d s%s.", b,
             S.have ? s->ntasks : 0, S.every, S.logging ? "; changes are logged" : "");
}

static void set_extent(void)
{
    _kernel_swi_regs r;
    int b[4], rows = S.have ? cur()->n : 0;
    if (rows == S.rows_shown)
        return;
    S.rows_shown = rows;
    b[0] = 0;
    b[1] = ROWS_Y1 - rows * ROW_H - 8;
    if (b[1] > -600)
        b[1] = -600;
    b[2] = WA_W;
    b[3] = 0;
    r.r[0] = S.win;
    r.r[1] = (intptr_t)b;
    swi(Wimp_SetExtent, &r);
}

static void redraw_all(void)
{
    _kernel_swi_regs r;
    if (!S.open)
        return;
    r.r[0] = S.win;
    r.r[1] = 0;
    r.r[2] = -0x10000000;
    r.r[3] = WA_W;
    r.r[4] = 0;
    swi(Wimp_ForceRedraw, &r);
}

/* Reads the list again, marks what changed, logs it, shows it */
static void refresh(void)
{
    int next = 1 - S.cur;
    snap_t *n = &S.snap[next];
    int keep = KEEP_S / (S.every ? S.every : 1);
    if (keep < 2)
        keep = 2;
    if (areas_read(n) != 0) {
        S.failed = 1;
        snprintf(S.err, sizeof(S.err), "%s", as_swi_error());
    } else {
        S.failed = 0;
        areas_compare(n, S.have ? &S.snap[S.cur] : NULL, keep);
        areas_sort(n, S.sort_col, S.sort_desc);
        S.cur = next;
        S.have = 1;
        log_changes();
    }
    S.next_cs = now_cs() + S.every * 100;
    make_lines();
    set_extent();
    redraw_all();
}

static void measure(void)
{
    const snap_t *s = cur();
    uint32_t piece = 128u << 20;
    if (S.have && s->clamp[1] == -1)
        piece = 512u << 20;         /* no cap: what the UnixLib programs ask for */
    else if (S.have && s->clamp[1] > 0)
        piece = (uint32_t)s->clamp[1] > (1u << 30) ? 1u << 30 : (uint32_t)s->clamp[1];
    hourglass(1);
    S.probe_failed = areas_probe(&S.probe, piece) != 0;
    hourglass(0);
    S.probed = !S.probe_failed;
    clock_text(S.probe_when, sizeof(S.probe_when), 0);
    if (S.probe.left)
        report("%d of the areas made to measure the free space couldn't be removed (they're called \"" PROBE_NAME
               "\").", S.probe.left);
    if (S.logging) {
        char path[300], a[40], big[80];
        FILE *f;
        if (scrap_path("AddrSpaceLog", path, sizeof(path)) && (f = fopen(path, "a")) != NULL) {
            if (S.probe_failed) {
                fprintf(f, "%s  free: none\n", S.probe_when);
            } else {
                fmt_mb(S.probe.total, a, sizeof(a));
                areas_largest(&S.probe, S.have ? s->clamp[1] : 0, big, sizeof(big));
                fprintf(f, "%s  free: %s%s; the largest free piece: %s\n", S.probe_when,
                        S.probe.full ? "at least " : "", a, big);
            }
            fclose(f);
        }
    }
    refresh();
}

static void save_list(void)
{
    char path[300], when[40], cmd[340];
    _kernel_swi_regs r;
    FILE *f;
    if (!S.have || !scrap_path("AddrSpaceList", path, sizeof(path)))
        return;
    if ((f = fopen(path, "w")) == NULL) {
        report("Couldn't write %s", path);
        return;
    }
    clock_text(when, sizeof(when), 1);
    fprintf(f, APP " " ADDRSPACE_VERSION "\n");
    areas_write(f, cur(), S.probed ? &S.probe : NULL, when);
    if (S.probed)
        fprintf(f, "(The free space was measured at %s.)\n", S.probe_when);
    fclose(f);
    set_type(path, 0xFFF);
    snprintf(cmd, sizeof(cmd), "Filer_Run %s", path);
    r.r[0] = (intptr_t)cmd;
    swi(OS_CLI, &r);
}

static void set_logging(int on)
{
    char path[300], when[40];
    FILE *f;
    if (on && scrap_path("AddrSpaceLog", path, sizeof(path))) {
        if ((f = fopen(path, "a")) == NULL) {
            report("Couldn't write %s", path);
            return;
        }
        clock_text(when, sizeof(when), 1);
        fprintf(f, "\n" APP " " ADDRSPACE_VERSION ": logging from %s\n", when);
        if (S.have)
            areas_write(f, cur(), NULL, when);
        fprintf(f, "\nChanges:\n");
        fclose(f);
        set_type(path, 0xFFF);
        S.logging = 1;
    } else {
        S.logging = 0;
    }
    make_lines();
    redraw_all();
}

/* ---- the window ------------------------------------------------------------------- */

typedef struct { int box[4]; unsigned flags; int data[3]; } icon_t;

/* Plots text in a box: fg and bg are Wimp colours; flags adds to it */
static void text_box(int x0, int y0, int x1, int y1, const char *text, int fg, int bg, unsigned flags)
{
    static char buf[256];
    icon_t ic;
    _kernel_swi_regs r;
    snprintf(buf, sizeof(buf), "%s", text);
    ic.box[0] = x0; ic.box[1] = y0; ic.box[2] = x1; ic.box[3] = y1;
    ic.flags = 0x131u | flags | (unsigned)fg << 24 | (unsigned)bg << 28;   /* text, v centred, filled, indirected */
    ic.data[0] = (int)(intptr_t)buf;
    ic.data[1] = -1;
    ic.data[2] = (int)strlen(buf) + 1;
    r.r[1] = (intptr_t)&ic;
    swi(Wimp_PlotIcon, &r);
}

static void row_colours(const area_t *a, int *fg, int *bg)
{
    *fg = 7;
    *bg = 0;
    switch (a->change) {
    case CH_NEW:    *bg = 10; break;    /* light green */
    case CH_GREW:   *bg = 9; break;     /* yellow */
    case CH_SHRANK: *bg = 15; break;    /* light blue */
    case CH_GONE:   *fg = 4; break;     /* grey */
    }
}

static void draw_row(int i, const area_t *a)
{
    char t[200];
    int y1 = ROWS_Y1 - i * ROW_H, y0 = y1 - ROW_H, fg, bg;
    row_colours(a, &fg, &bg);
    for (int c = 0; c < COL_COUNT; c++) {
        unsigned just = 0;
        int f = fg;
        switch (c) {
        case COL_NAME:  snprintf(t, sizeof(t), " %s", a->name); break;
        case COL_OWNER: snprintf(t, sizeof(t), " %s", a->owner); break;
        case COL_SIZE:  fmt_k(a->size, t, sizeof(t)); strcat(t, " "); just = 0x200; break;
        case COL_SPAN:  fmt_k(a->span, t, sizeof(t)); strcat(t, " "); just = 0x200; break;
        case COL_BASE:  snprintf(t, sizeof(t), " &%08X", (unsigned)a->base); break;
        case COL_NOTES:
            t[0] = ' ';
            areas_notes(a, t + 1, sizeof(t) - 1);
            if (a->kind == KIND_HEAP_LEFT && a->change != CH_GONE)
                f = 11;                 /* red */
            break;
        }
        if (c == COL_OWNER && a->kind == KIND_HEAP_LEFT && a->change != CH_GONE)
            f = 11;
        text_box(col_x[c], y0, col_x[c + 1] - 4, y1 - 2, t, f, bg, just);
    }
}

static void draw_heading(void)
{
    char t[40];
    for (int c = 0; c < COL_COUNT; c++) {
        snprintf(t, sizeof(t), "%s%s", col_name[c], c == S.sort_col ? (S.sort_desc ? " v" : " ^") : "");
        text_box(col_x[c], HEAD_Y1 - HEAD_H, col_x[c + 1] - 4, HEAD_Y1, t, 7, 1, 0x0C);     /* border, centred */
    }
}

static void redraw(int *b)
{
    _kernel_swi_regs r;
    int more;
    r.r[1] = (intptr_t)b;
    if (swi(Wimp_RedrawWindow, &r))
        return;
    more = (int)r.r[0];
    while (more) {
        int oy = b[4] - b[6];           /* the work area's origin, on the screen */
        int cy1 = b[10] - oy, cy0 = b[8] - oy;
        const snap_t *s = cur();
        for (int k = 0; k < 3; k++) {
            int y1 = -TOP_PAD - k * LINE_H;
            if (y1 - LINE_H <= cy1 && y1 >= cy0)
                text_box(col_x[0], y1 - LINE_H, WA_W - 8, y1, S.line[k], 7, 1, 0);
        }
        if (HEAD_Y1 - HEAD_H <= cy1 && HEAD_Y1 >= cy0)
            draw_heading();
        if (S.have) {
            int first = (ROWS_Y1 - cy1) / ROW_H - 1, last = (ROWS_Y1 - cy0) / ROW_H + 1;
            if (first < 0)
                first = 0;
            for (int i = first; i < s->n && i <= last; i++)
                draw_row(i, &s->a[i]);
        }
        r.r[1] = (intptr_t)b;
        if (swi(Wimp_GetRectangle, &r))
            break;
        more = (int)r.r[0];
    }
}

static void open_window(void)
{
    int st[9];
    _kernel_swi_regs r;
    st[0] = S.win;
    r.r[1] = (intptr_t)st;
    if (swi(Wimp_GetWindowState, &r))
        return;
    st[7] = -1;                         /* in front */
    swi(Wimp_OpenWindow, &r);
    if (!S.open) {
        S.open = 1;
        refresh();
    }
}

static void make_window(void)
{
    struct {
        int vis[4];
        int sx, sy, behind, flags;
        unsigned char tfg, tbg, wfg, wbg, sofg, sibg, tfocus, xflags;
        int ext[4];
        int tflags, wbutton, sprites;
        short minw, minh;
        int title[3];
        int nicons;
    } w;
    _kernel_swi_regs r;
    memset(&w, 0, sizeof(w));
    snprintf(S.title, sizeof(S.title), "Logical address space");
    w.vis[0] = 120; w.vis[1] = 200; w.vis[2] = 120 + 1500; w.vis[3] = 200 + 900;
    w.behind = -1;
    w.flags = (int)0xFF000002u;         /* new format, moveable; back, close, title, toggle, scroll bars, adjust size */
    w.tfg = 7; w.tbg = 2; w.wfg = 7; w.wbg = 1; w.sofg = 3; w.sibg = 1; w.tfocus = 12;
    w.ext[0] = 0; w.ext[1] = -900; w.ext[2] = WA_W; w.ext[3] = 0;
    w.tflags = 0x0000013D;              /* text, border, centred, filled, indirected */
    w.wbutton = 3 << 12;                /* click */
    w.sprites = 1;
    w.minw = 600; w.minh = 300;
    w.title[0] = (int)(intptr_t)S.title;
    w.title[1] = -1;
    w.title[2] = (int)sizeof(S.title);
    r.r[1] = (intptr_t)&w;
    S.win = swi(Wimp_CreateWindow, &r) ? -1 : (int)r.r[0];
}

/* The column whose heading is at work area x, y; -1 if none */
static int heading_at(int x, int y)
{
    if (y > HEAD_Y1 || y < HEAD_Y1 - HEAD_H)
        return -1;
    for (int c = 0; c < COL_COUNT; c++)
        if (x >= col_x[c] && x < col_x[c + 1])
            return c;
    return -1;
}

static void sort_by(int c)
{
    if (c == S.sort_col)
        S.sort_desc = !S.sort_desc;
    else {
        S.sort_col = c;
        S.sort_desc = c == COL_SIZE || c == COL_SPAN;   /* the biggest first */
    }
    if (S.have)
        areas_sort(cur(), S.sort_col, S.sort_desc);
    redraw_all();
}

/* ---- the menu --------------------------------------------------------------------- */

typedef struct { int flags, sub; unsigned iflags; int data[3]; } mitem_t;
typedef struct {
    char title[12];
    unsigned char tfg, tbg, wfg, wbg;
    int width, height, gap;
    mitem_t item[10];
} wmenu_t;
typedef struct { wmenu_t m; char text[10][40]; int n; } menu_t;

static menu_t m_bar, m_every;
static int menu_x, menu_y;

static void menu_begin(menu_t *m, const char *title)
{
    memset(m, 0, sizeof(*m));
    snprintf(m->m.title, sizeof(m->m.title), "%.11s", title);
    m->m.tfg = 7; m->m.tbg = 2; m->m.wfg = 7; m->m.wbg = 0;
    m->m.height = 44;
    m->m.width = (int)strlen(m->m.title) * 16 + 16;
}

static void menu_add(menu_t *m, const char *text, int tick, int sub, int dots)
{
    mitem_t *i = &m->m.item[m->n];
    int w;
    if (m->n >= 10)
        return;
    snprintf(m->text[m->n], sizeof(m->text[0]), "%s", text);
    i->flags = (tick ? 1 : 0) | (dots ? 2 : 0);
    i->sub = sub;
    i->iflags = 0x07000121u;            /* text, filled, indirected */
    i->data[0] = (int)(intptr_t)m->text[m->n];
    i->data[1] = -1;
    i->data[2] = (int)sizeof(m->text[0]);
    w = (int)strlen(m->text[m->n]) * 16 + 32;
    if (w > m->m.width)
        m->m.width = w;
    m->n++;
}

static void menu_end(menu_t *m)
{
    if (m->n)
        m->m.item[m->n - 1].flags |= 0x80;      /* the last */
}

static void menu_build(void)
{
    char t[40];
    menu_begin(&m_every, "Read every");
    for (int i = 0; i < N_EVERY; i++) {
        snprintf(t, sizeof(t), "%d second%s", every_s[i], every_s[i] == 1 ? "" : "s");
        menu_add(&m_every, t, every_s[i] == S.every, -1, 0);
    }
    menu_end(&m_every);
    menu_begin(&m_bar, APP);
    menu_add(&m_bar, "Info", 0, S.proginfo, 0);
    menu_add(&m_bar, "Read every", 0, (int)(intptr_t)&m_every.m, 0);
    menu_add(&m_bar, "Read now", 0, -1, 0);
    menu_add(&m_bar, "Measure free space", 0, -1, 0);
    menu_add(&m_bar, "Save list", 0, -1, 0);
    menu_add(&m_bar, "Log changes", S.logging, -1, 0);
    menu_add(&m_bar, "Quit", 0, -1, 0);
    m_bar.m.item[MB_READ].flags |= 2;   /* lines under Read now and Log changes */
    m_bar.m.item[MB_LOG].flags |= 2;
    menu_end(&m_bar);
}

static void open_menu(int x, int y)
{
    _kernel_swi_regs r;
    menu_build();
    menu_x = x;
    menu_y = y;
    r.r[1] = (intptr_t)&m_bar.m;
    r.r[2] = x;
    r.r[3] = y;
    swi(Wimp_CreateMenu, &r);
}

/* 1 = quit */
static int menu_select(const int *sel)
{
    int p[5];
    _kernel_swi_regs r;
    switch (sel[0]) {
    case MB_EVERY:
        if (sel[1] >= 0 && sel[1] < N_EVERY) {
            S.every = every_s[sel[1]];
            S.next_cs = now_cs() + S.every * 100;
            make_lines();
            redraw_all();
        }
        break;
    case MB_READ:    refresh(); break;
    case MB_MEASURE: measure(); break;
    case MB_SAVE:    save_list(); break;
    case MB_LOG:     set_logging(!S.logging); break;
    case MB_QUIT:    return 1;
    }
    r.r[1] = (intptr_t)p;
    if (!swi(Wimp_GetPointerInfo, &r) && (p[2] & 1))      /* Adjust: the menu stays */
        open_menu(menu_x, menu_y);
    return 0;
}

/* ---- the program ------------------------------------------------------------------ */

static void iconbar_icon(void)
{
    struct { int w, x0, y0, x1, y1, flags; char name[12]; } b;
    _kernel_swi_regs r;
    memset(&b, 0, sizeof(b));
    b.w = -1;
    b.x1 = 68; b.y1 = 68;
    b.flags = 0x301A;                   /* sprite, centred, click */
    strcpy(b.name, ICON);
    r.r[0] = 0;
    r.r[1] = (intptr_t)&b;
    swi(Wimp_CreateIcon, &r);
    S.bar_icon = (int)r.r[0];
}

static void click(const int *b)
{
    if (b[3] == -2) {                   /* the icon bar */
        if (b[2] & 2)
            open_menu(b[0] - 64, 96 + 7 * 44);
        else
            open_window();
        return;
    }
    if (b[3] != S.win)
        return;
    if (b[2] & 2) {
        open_menu(b[0] - 64, b[1]);
        return;
    }
    {
        int st[9], c;
        _kernel_swi_regs r;
        st[0] = S.win;
        r.r[1] = (intptr_t)st;
        if (swi(Wimp_GetWindowState, &r))
            return;
        c = heading_at(b[0] - (st[1] - st[5]), b[1] - (st[4] - st[6]));
        if (c >= 0)
            sort_by(c);
    }
}

int addrspace_main(int argc, char **argv)
{
    static const int messages[] = { 0 };
    int block[64];
    _kernel_swi_regs r;
    (void)argc;
    (void)argv;

    memset(&S, 0, sizeof(S));
    S.every = 2;
    S.sort_col = COL_SPAN;
    S.sort_desc = 1;
    r.r[0] = 380;
    r.r[1] = 0x4B534154;                /* "TASK" */
    r.r[2] = (intptr_t)APP;
    r.r[3] = (intptr_t)messages;
    if (swi(Wimp_Initialise, &r))
        return 1;
    S.task = (int)r.r[1];
    make_window();
    iconbar_icon();
    S.proginfo = proginfo_create(APP, PURPOSE, APP_AUTHOR, ADDRSPACE_VERSION " (" ADDRSPACE_DATE ")");
    if (getenv("AddrSpace$Log") && *getenv("AddrSpace$Log") && strcmp(getenv("AddrSpace$Log"), "off")) {
        refresh();
        set_logging(1);
    }
    make_lines();

    for (;;) {
        int reason = Wimp_Poll, mask = 1 << 11 | 1 << 12;
        if (S.open || S.logging) {
            reason = Wimp_PollIdle;
            r.r[2] = S.next_cs;
        } else {
            mask |= 1;                  /* nothing to read for */
        }
        r.r[0] = mask;
        r.r[1] = (intptr_t)block;
        if (swi(reason, &r))
            continue;
        switch (r.r[0]) {
        case 0:
            if (now_cs() - S.next_cs >= 0)
                refresh();
            break;
        case 1:
            redraw(block);
            break;
        case 2:
            r.r[1] = (intptr_t)block;
            swi(Wimp_OpenWindow, &r);
            break;
        case 3:
            if (block[0] == S.win) {
                r.r[1] = (intptr_t)block;
                swi(Wimp_CloseWindow, &r);
                S.open = 0;
            }
            break;
        case 6:
            click(block);
            break;
        case 9:
            if (menu_select(block))
                goto quit;
            break;
        case 17: case 18:
            if (block[4] == 0)          /* Message_Quit */
                goto quit;
            break;
        }
    }
quit:
    r.r[0] = S.task;
    r.r[1] = 0x4B534154;
    swi(Wimp_CloseDown, &r);
    return 0;
}

#ifndef ADDRSPACE_NO_MAIN
int main(int argc, char **argv)
{
    return addrspace_main(argc, argv);
}
#endif

#ifdef ADDRSPACE_TEST
const snap_t *ui_test_snap(void) { return S.have ? cur() : NULL; }
const char *ui_test_line(int k) { return k >= 0 && k < 3 ? S.line[k] : ""; }
int ui_test_window(int *open) { *open = S.open; return S.win; }
int ui_test_sort(int *desc) { *desc = S.sort_desc; return S.sort_col; }
int ui_test_logging(void) { return S.logging; }
int ui_test_every(void) { return S.every; }
const probe_t *ui_test_probe(void) { return S.probed ? &S.probe : NULL; }
int ui_test_heading_xy(int c, int *x, int *y)
{
    int st[9];
    _kernel_swi_regs r;
    st[0] = S.win;
    r.r[1] = (intptr_t)st;
    if (c < 0 || c >= COL_COUNT || swi(Wimp_GetWindowState, &r))
        return -1;
    *x = (col_x[c] + col_x[c + 1]) / 2 + st[1] - st[5];
    *y = HEAD_Y1 - HEAD_H / 2 + st[4] - st[6];
    return 0;
}
#endif
