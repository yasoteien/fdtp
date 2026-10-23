// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_SHEET_MODEL_H
#define FDTP_SHEET_MODEL_H
/* The table model shared by sheetview.c (screen), sheet_xlsx.c (OOXML load
 * and save), sheet_csv.c and sheet_formula.c. Cells are stored sparsely in
 * one array sorted by (row, col); display strings are made only for the
 * cells on the screen. Rows and columns are 0-based here, 1-based on screen. */
#include "jis.h"
#include "zipfile.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define SHEET_MAX_ROWS 1048576u
#define SHEET_MAX_COLS 16384u
#define SHEET_MAX_CELLS 8000000u
#define SHEET_NONE UINT32_MAX

typedef struct { char *data; size_t len, cap; } Arena;
/* Appends n bytes and a NUL; returns the offset or SHEET_NONE. */
uint32_t arena_add(Arena *a, const char *s, size_t n);
static inline const char *arena_str(const Arena *a, uint32_t at) { return at == SHEET_NONE ? "" : a->data + at; }
void arena_free(Arena *a);

typedef enum { SK_EMPTY, SK_NUM, SK_STR, SK_SST, SK_BOOL, SK_ERR } SheetKind;
enum {
    SF_FORMULA = 1,         /* formula text in .formula */
    SF_CALC = 2,            /* the formula can be computed here */
    SF_EDITED = 4,          /* changed by the user since the last save */
    SF_RECALC = 8,          /* value changed by recalculation */
    SF_REWRITE = 16,        /* former shared-formula child: write a plain <f> */
    SF_ORIG_FORMULA = 32,   /* the file has a formula in this cell */
    SF_QUOTED = 64,         /* csv: the field was quoted */
    SF_ARRAY = 128,         /* array/data-table master: not editable */
    SF_SHARED = 256         /* shared formula member; .aux is the si */
};
typedef struct {
    uint32_t row, col;
    uint8_t kind, eval, affected, pad;
    uint16_t flags;
    uint32_t style;
    uint32_t text;          /* SK_STR/SK_ERR: arena offset; SK_SST: index */
    uint32_t formula;       /* arena offset (without '='), or SHEET_NONE */
    uint32_t aux;           /* shared formula si */
    uint32_t gen;           /* recalculation generation of eval/affected */
    double num;
} SheetCell;

typedef struct { uint32_t r1, c1, r2, c2; } SheetRange;
typedef struct { uint32_t si, row, col, text; } SheetShared;

typedef struct {
    char *name;             /* display name */
    char *part;             /* zip entry name (xlsx) */
    uint32_t sheet_id;
    int loaded, dirty;
    SheetCell *cells;
    size_t count, cap;
    Arena arena;
    SheetRange *merges, *arrays;
    size_t nmerges, narrays;
    SheetShared *shared;
    size_t nshared;
    uint32_t *row_fields;   /* csv: fields per row */
    size_t nrows;
    uint32_t gen;
    /* view */
    uint32_t cur_row, cur_col, top_row, left_col;
    unsigned char *widths;  /* SHEET_MAX_COLS entries, 0 = default */
} Sheet;

typedef enum { FMT_GENERAL, FMT_FIXED, FMT_PERCENT, FMT_DATE, FMT_TIME, FMT_DATETIME } FormatKind;
typedef struct { uint8_t kind, decimals, seconds; } CellFormat;

typedef enum { BOOK_CSV, BOOK_XLSX } BookType;
typedef struct {
    BookType type;
    char *path;
    Sheet *sheets;
    int nsheets, current;
    int readonly, backup_done;
    char readonly_reason[128];
    time_t mtime;
    off_t size;
    /* xlsx */
    ZipArchive *zip;
    Arena sst;
    uint32_t *sst_off;
    size_t nsst;
    CellFormat *xf;
    size_t nxf;
    int date1904;
    char *calc_chain;       /* part names found through relationships */
    char *workbook_rels;
    /* csv */
    char delim;
    JisEncoding encoding;
    int bom, final_newline;
    char newline[3];
} Book;

/* sheet_model.c-like helpers live in sheetview.c's companions: */
SheetCell *sheet_find(Sheet *s, uint32_t row, uint32_t col);
/* First cell at or after (row, col) in storage order; s->count if none. */
size_t sheet_lower(const Sheet *s, uint32_t row, uint32_t col);
/* Inserts an empty cell when absent. NULL on allocation failure/limit. */
SheetCell *sheet_insert(Sheet *s, uint32_t row, uint32_t col);
void sheet_free(Sheet *s);
void book_free(Book *b);
const char *cell_string(const Book *b, const Sheet *s, const SheetCell *c);
void sheet_address(uint32_t row, uint32_t col, char *out, size_t n);
/* "B12" / "$B$12" → 0-based; returns chars consumed or 0. */
size_t sheet_parse_address(const char *s, uint32_t *row, uint32_t *col);
void sheet_column_name(uint32_t col, char *out, size_t n);
int sheet_parse_range(const char *s, SheetRange *r);
/* Shortest text that reads back as the same double. */
void sheet_number_text(double v, char *out, size_t n);
/* Strict number syntax (sign, digits, point, exponent). */
int sheet_parse_number(const char *s, double *v);

/* sheet_csv.c */
int csv_open(Book *b, const char *path, char delim, char *err, size_t errlen);
int csv_save(Book *b, char *err, size_t errlen);

/* sheet_xlsx.c */
int xlsx_open(Book *b, const char *path, char *err, size_t errlen);
int xlsx_load_sheet(Book *b, int index, char *err, size_t errlen);
int xlsx_save(Book *b, char *err, size_t errlen);
void xlsx_unload_sheet(Book *b, int index);
CellFormat xlsx_format(const Book *b, uint32_t style);

/* sheet_formula.c */
typedef enum { FORMULA_OK, FORMULA_SYNTAX, FORMULA_FUNCTION, FORMULA_UNSUPPORTED } FormulaCheck;
FormulaCheck formula_check(const char *text);
/* Re-evaluates every computable formula that depends on edited cells and
 * stores changed results (SF_RECALC). Iterative (no recursion), so chains
 * of any length work. Returns 1 when a reference is circular (the cell's
 * present value is used), 2 on allocation failure, 0 otherwise. */
int sheet_recalc(Book *b, Sheet *s);
/* Moves relative references by (drow, dcol). NULL when a reference would
 * leave the sheet or on allocation failure. Caller frees. */
char *formula_shift(const char *text, long drow, long dcol);

/* sheet_undo.c (revision 1 item 38): one cell's edit as an undo step, with
 * every cell the edit changed (recalculated formulas, a shared formula's
 * members) and the shared-formula masters it let go. */
typedef struct SheetCapture SheetCapture;
typedef struct SheetStep SheetStep;
/* The cell (row, col) and the cells an edit of it could change, before the
 * edit. NULL when out of memory. */
SheetCapture *sheet_capture(const Book *b, Sheet *s, uint32_t row, uint32_t col);
void sheet_capture_free(SheetCapture *cap);
/* After the edit: what changed since cap (which is freed). NULL with *same
 * set when nothing did, NULL without it when out of memory. */
SheetStep *sheet_step_make(const Book *b, Sheet *s, int sheet, uint32_t row, uint32_t col, SheetCapture *cap, int *same);
/* Back to before the step (backward) or to after it, on s (the step's
 * sheet, loaded). force: the file was saved since the step was made, so
 * the cells are marked to be written again. -1 when out of memory. */
int sheet_step_apply(Book *b, Sheet *s, const SheetStep *st, int backward, int force);
void sheet_step_where(const SheetStep *st, int *sheet, uint32_t *row, uint32_t *col);
void sheet_step_free(void *st);
#endif
