/*
 * main.h - AddrSpace's front end (main.c): its menu's items, and what the
 * host test (tests/host/ui_test.c) looks at.
 * Part of riscos-addrspace. GPL v2 or later.
 */
#ifndef ADDRSPACE_MAIN_H
#define ADDRSPACE_MAIN_H

#include "areas.h"

/* The icon bar menu's items (the window's menu is the same) */
enum { MB_INFO, MB_EVERY, MB_READ, MB_MEASURE, MB_SAVE, MB_LOG, MB_QUIT, MB_COUNT };

int addrspace_main(int argc, char **argv);

#ifdef ADDRSPACE_TEST
const snap_t *ui_test_snap(void);           /* the latest reading, or NULL */
const char *ui_test_line(int k);            /* the totals' lines, 0..2 */
int ui_test_window(int *open);              /* the window's handle */
int ui_test_sort(int *desc);                /* the column sorted by */
int ui_test_logging(void);
int ui_test_every(void);
const probe_t *ui_test_probe(void);         /* the last measurement, or NULL */
int ui_test_heading_xy(int c, int *x, int *y);      /* a heading's middle, on the screen */
#endif

#endif
