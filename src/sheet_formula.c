// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
/* Formulas: SUM AVERAGE MIN MAX COUNT, + - * / ^ %, unary signs, brackets,
 * cell references and ranges (A1:B3, A:A, 1:3). Anything else is detected
 * (so that the cached result from the file is shown) but not computed. */
#include "sheet_model.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { P_SYNTAX = 1, P_FUNCTION = 2, P_UNSUPPORTED = 4 };
enum { E_DIV0 = 1, E_VALUE, E_REF, E_NAME, E_NA, E_NUM, E_NULL };
static const char *const error_text[] = {"", "#DIV/0!", "#VALUE!", "#REF!", "#NAME?", "#N/A", "#NUM!", "#NULL!"};
typedef enum { V_NUM, V_STR, V_BOOL, V_ERR, V_EMPTY, V_REF } VKind;
/* Strings are referred to by owner and offset: the arena may move while a
 * formula that references a string result is being evaluated. */
typedef struct { VKind k; int err, sst; double num; uint32_t off; SheetRange r; } Val;
/* Formula cells still to evaluate. Evaluation is iterative: a formula that
 * meets formula cells not yet computed pushes them, is abandoned, and is
 * evaluated again once they are done, so long chains need no C stack. */
typedef struct { SheetCell **v; size_t n, cap; int failed; } Stack;
enum { EV_QUEUED = 1, EV_ACTIVE = 2, EV_DONE = 3 };
typedef struct {
    Book *b;
    Sheet *s;               /* NULL: syntax check only */
    const char *p;
    int problems, affected, circular, missing;
    Stack *stack;
} Ev;

static Val num(double v) { Val x = {V_NUM, 0, 0, v, 0, {0, 0, 0, 0}}; return x; }
static Val error(int e) { Val x = {V_ERR, e, 0, 0, 0, {0, 0, 0, 0}}; return x; }
static Val empty(void) { Val x = {V_EMPTY, 0, 0, 0, 0, {0, 0, 0, 0}}; return x; }
static void spaces(Ev *e) { while (*e->p == ' ' || *e->p == '\t' || *e->p == '\n' || *e->p == '\r') ++e->p; }
static int error_code(const char *t)
{
    for (int i = 1; i <= E_NULL; ++i) if (!strcmp(t, error_text[i])) return i;
    return E_VALUE;
}
static const char *val_string(Ev *e, const Val *v)
{
    if (v->sst) return v->off < e->b->nsst ? arena_str(&e->b->sst, e->b->sst_off[v->off]) : "";
    return arena_str(&e->s->arena, v->off);
}

static Val eval_formula(Ev *e);
static Val stored(Ev *e, SheetCell *c)
{
    Val v = empty();
    switch (c->kind) {
    case SK_NUM: v = num(c->num); break;
    case SK_BOOL: v.k = V_BOOL; v.num = c->num; break;
    case SK_STR: v.k = V_STR; v.off = c->text; break;
    case SK_SST: v.k = V_STR; v.sst = 1; v.off = c->text; break;
    case SK_ERR: v = error(error_code(arena_str(&e->s->arena, c->text))); break;
    default: break;
    }
    return v;
}
static int same_value(Ev *e, SheetCell *c, const Val *v, const char *str)
{
    switch (v->k) {
    case V_NUM: return c->kind == SK_NUM && c->num == v->num;
    case V_BOOL: return c->kind == SK_BOOL && c->num == v->num;
    case V_ERR: return c->kind == SK_ERR && !strcmp(arena_str(&e->s->arena, c->text), error_text[v->err]);
    case V_STR: return (c->kind == SK_STR || c->kind == SK_SST) && !strcmp(cell_string(e->b, e->s, c), str);
    default: return 0;
    }
}
static void commit(Ev *e, SheetCell *c, Val v)
{
    char *copy = NULL;
    if (v.k == V_EMPTY) v = num(0);
    if (v.k == V_STR) { copy = strdup(val_string(e, &v)); if (!copy) v = error(E_VALUE); }
    if (same_value(e, c, &v, copy)) { free(copy); return; }
    switch (v.k) {
    case V_NUM: c->kind = SK_NUM; c->num = v.num; break;
    case V_BOOL: c->kind = SK_BOOL; c->num = v.num; break;
    case V_ERR: c->kind = SK_ERR; c->text = arena_add(&e->s->arena, error_text[v.err], strlen(error_text[v.err])); break;
    case V_STR: c->kind = SK_STR; c->text = arena_add(&e->s->arena, copy, strlen(copy)); break;
    default: break;
    }
    if ((c->kind == SK_STR || c->kind == SK_ERR) && c->text == SHEET_NONE) c->kind = SK_EMPTY;
    free(copy);
    if (!(c->flags & SF_EDITED)) c->flags |= SF_RECALC;
    e->s->dirty = 1;
}
static void push(Ev *e, SheetCell *c)
{
    Stack *st = e->stack;
    if (st->n == st->cap) {
        size_t cap = st->cap ? st->cap * 2 : 64;
        SheetCell **p = realloc(st->v, cap * sizeof(*p));
        if (!p) { st->failed = 1; return; }
        st->v = p; st->cap = cap;
    }
    st->v[st->n++] = c;
}
static Val cell_value(Ev *e, SheetCell *c)
{
    if ((c->flags & (SF_FORMULA | SF_CALC)) == (SF_FORMULA | SF_CALC)) {
        int state = c->gen == e->s->gen ? c->eval : 0;
        if (state == 0 || state == EV_QUEUED) {
            /* Not computed yet: compute it first, then this formula again. */
            c->gen = e->s->gen; c->eval = EV_QUEUED;
            push(e, c);
            e->missing = 1;
            return num(0);
        }
        /* An active cell below on the stack waits for this one: a cycle.
         * Its present value is used, as Excel does without iteration. */
        if (state == EV_ACTIVE) { e->circular = 1; return stored(e, c); }
        if (c->affected) e->affected = 1;
    } else if (c->flags & SF_EDITED) e->affected = 1;
    return stored(e, c);
}
/* Computes root and whatever it needs. */
static int evaluate(Book *b, Sheet *s, SheetCell *root, Stack *st)
{
    int circular = 0;
    Ev seed = {b, s, "", 0, 0, 0, 0, st};
    root->gen = s->gen; root->eval = EV_QUEUED;
    push(&seed, root);
    while (st->n && !st->failed) {
        SheetCell *c = st->v[st->n - 1];
        if (c->eval == EV_DONE) { st->n--; continue; }
        c->eval = EV_ACTIVE;
        Ev e = {b, s, arena_str(&s->arena, c->formula), 0, 0, 0, 0, st};
        Val v = eval_formula(&e);
        circular |= e.circular;
        if (e.missing) continue;
        st->n--;
        c->eval = EV_DONE;
        c->affected = (uint8_t)(e.affected || (c->flags & SF_EDITED));
        if (c->affected && !e.problems) commit(&e, c, v);
    }
    return circular;
}
static Val deref(Ev *e, Val v)
{
    if (v.k != V_REF) return v;
    if (!e->s) return empty();
    if (v.r.r1 != v.r.r2 || v.r.c1 != v.r.c2) return error(E_VALUE);
    SheetCell *c = sheet_find(e->s, v.r.r1, v.r.c1);
    return c ? cell_value(e, c) : empty();
}
static int to_number(Ev *e, Val v, double *out, int *err)
{
    switch (v.k) {
    case V_NUM: case V_BOOL: *out = v.num; return 1;
    case V_EMPTY: *out = 0; return 1;
    case V_ERR: *err = v.err; return 0;
    case V_STR: {
        const char *t = val_string(e, &v);
        while (*t == ' ') ++t;
        char buf[64];
        size_t n = strlen(t);
        while (n && t[n - 1] == ' ') --n;
        if (n && n < sizeof(buf)) { memcpy(buf, t, n); buf[n] = 0; if (sheet_parse_number(buf, out)) return 1; }
        *err = E_VALUE; return 0;
    }
    default: *err = E_VALUE; return 0;
    }
}

/* ---- lexing helpers ---- */
static int ident_char(char c) { return isalnum((unsigned char)c) || c == '_' || c == '.' || c == '$' || c == '\\' || (unsigned char)c >= 0x80; }
/* Cell reference "$A$1": returns its length and position, 0 if not one. */
static size_t match_cell(const char *p, size_t n, uint32_t *row, uint32_t *col, int *abs_row, int *abs_col)
{
    size_t i = 0;
    *abs_col = *abs_row = 0;
    if (i < n && p[i] == '$') { *abs_col = 1; ++i; }
    size_t l0 = i;
    while (i < n && isalpha((unsigned char)p[i])) ++i;
    if (i == l0 || i - l0 > 3) return 0;
    if (i < n && p[i] == '$') { *abs_row = 1; ++i; }
    size_t d0 = i;
    while (i < n && isdigit((unsigned char)p[i])) ++i;
    if (i == d0 || i - d0 > 7 || i != n) return 0;
    return sheet_parse_address(p, row, col) ? n : 0;
}
static size_t match_column(const char *p, size_t n, uint32_t *col, int *abs_col)
{
    size_t i = 0;
    uint64_t c = 0;
    *abs_col = 0;
    if (i < n && p[i] == '$') { *abs_col = 1; ++i; }
    size_t l0 = i;
    while (i < n && isalpha((unsigned char)p[i])) { c = c * 26 + (uint64_t)(toupper((unsigned char)p[i]) - 'A' + 1); ++i; }
    if (i == l0 || i - l0 > 3 || i != n || c > SHEET_MAX_COLS) return 0;
    *col = (uint32_t)c - 1;
    return n;
}
static size_t match_row(const char *p, size_t n, uint32_t *row, int *abs_row)
{
    size_t i = 0;
    uint64_t r = 0;
    *abs_row = 0;
    if (i < n && p[i] == '$') { *abs_row = 1; ++i; }
    size_t d0 = i;
    while (i < n && isdigit((unsigned char)p[i])) { r = r * 10 + (uint64_t)(p[i] - '0'); ++i; if (r > SHEET_MAX_ROWS) return 0; }
    if (i == d0 || i != n || r < 1) return 0;
    *row = (uint32_t)r - 1;
    return n;
}
static size_t word(const char *p)
{
    size_t n = 0;
    while (ident_char(p[n])) ++n;
    return n;
}
static const char *after_spaces(const char *p)
{
    while (*p == ' ') ++p;
    return p;
}
/* p at an opening quote; returns past the closing one (a doubled quote is
 * part of the text), or at the end when it is missing. */
static const char *skip_quoted(const char *p)
{
    char q = *p++;
    while (*p && !(*p == q && p[1] != q)) p += *p == q ? 2 : 1;
    return *p ? p + 1 : p;
}
static void skip_group(Ev *e, char open, char close)
{
    int level = 0;
    while (*e->p) {
        if (*e->p == '"') {
            e->p = skip_quoted(e->p);
            continue;
        }
        if (*e->p == open) ++level;
        else if (*e->p == close && --level == 0) { ++e->p; return; }
        ++e->p;
    }
    e->problems |= P_SYNTAX;
}

/* ---- parser/evaluator ---- */
static Val expr(Ev *e);
typedef struct { double sum, min, max; long count; int err, have; } Agg;
static void agg_number(Agg *a, double v)
{
    a->sum += v; a->count++;
    if (!a->have || v < a->min) a->min = v;
    if (!a->have || v > a->max) a->max = v;
    a->have = 1;
}
static void agg_range(Ev *e, Agg *a, SheetRange r)
{
    if (!e->s) return;
    Sheet *s = e->s;
    size_t i = sheet_lower(s, r.r1, r.c1);
    while (i < s->count) {
        SheetCell *c = &s->cells[i];
        if (c->row > r.r2) break;
        if (c->col < r.c1) { i = sheet_lower(s, c->row, r.c1); continue; }
        if (c->col > r.c2) {
            if (c->row >= SHEET_MAX_ROWS - 1) break;
            i = sheet_lower(s, c->row + 1, r.c1);
            continue;
        }
        Val v = cell_value(e, c);
        if (v.k == V_NUM) agg_number(a, v.num);
        else if (v.k == V_ERR && !a->err) a->err = v.err;
        ++i;
    }
}
static Val function(Ev *e, const char *name, size_t n)
{
    static const char *const names[] = {"SUM", "AVERAGE", "MIN", "MAX", "COUNT"};
    int which = -1;
    for (int i = 0; i < 5; ++i) if (strlen(names[i]) == n && !strncasecmp(name, names[i], n)) which = i;
    if (which < 0) { e->problems |= P_FUNCTION; skip_group(e, '(', ')'); return error(E_NAME); }
    ++e->p; /* ( */
    Agg a = {0, 0, 0, 0, 0, 0};
    spaces(e);
    if (*e->p == ')') { e->problems |= P_SYNTAX; ++e->p; return error(E_VALUE); }
    for (;;) {
        Val v = expr(e);
        if (v.k == V_REF) agg_range(e, &a, v.r);
        else if (which == 4) { if (v.k == V_NUM || v.k == V_BOOL) a.count++; else if (v.k == V_STR) { double d; int er = 0; if (to_number(e, v, &d, &er)) a.count++; } }
        else {
            double d; int er = 0;
            if (to_number(e, v, &d, &er)) agg_number(&a, d);
            else if (!a.err) a.err = er;
        }
        spaces(e);
        if (*e->p == ',') { ++e->p; continue; }
        if (*e->p == ')') { ++e->p; break; }
        e->problems |= P_SYNTAX;
        return error(E_VALUE);
    }
    if (which == 4) return num((double)a.count);
    if (a.err) return error(a.err);
    switch (which) {
    case 0: return num(a.sum);
    case 1: return a.count ? num(a.sum / (double)a.count) : error(E_DIV0);
    case 2: return num(a.have ? a.min : 0);
    default: return num(a.have ? a.max : 0);
    }
}
static Val reference(Ev *e, const char *w, size_t n)
{
    uint32_t r1, c1, r2, c2;
    int ar, ac;
    const char *colon = after_spaces(w + n);
    if (match_cell(w, n, &r1, &c1, &ar, &ac)) {
        e->p = w + n;
        Val v = {V_REF, 0, 0, 0, 0, {r1, c1, r1, c1}};
        if (*colon == ':') {
            const char *q = after_spaces(colon + 1);
            size_t m = word(q);
            if (match_cell(q, m, &r2, &c2, &ar, &ac)) {
                e->p = q + m;
                v.r.r1 = r1 < r2 ? r1 : r2; v.r.r2 = r1 < r2 ? r2 : r1;
                v.r.c1 = c1 < c2 ? c1 : c2; v.r.c2 = c1 < c2 ? c2 : c1;
            } else { e->problems |= P_UNSUPPORTED; e->p = q + m; }
        }
        return v;
    }
    if (*colon == ':') {
        const char *q = after_spaces(colon + 1);
        size_t m = word(q);
        if (match_column(w, n, &c1, &ac) && match_column(q, m, &c2, &ac)) {
            e->p = q + m;
            Val v = {V_REF, 0, 0, 0, 0, {0, c1 < c2 ? c1 : c2, SHEET_MAX_ROWS - 1, c1 < c2 ? c2 : c1}};
            return v;
        }
        if (match_row(w, n, &r1, &ar) && match_row(q, m, &r2, &ar)) {
            e->p = q + m;
            Val v = {V_REF, 0, 0, 0, 0, {r1 < r2 ? r1 : r2, 0, r1 < r2 ? r2 : r1, SHEET_MAX_COLS - 1}};
            return v;
        }
    }
    return error(0);
}
static Val primary(Ev *e)
{
    spaces(e);
    const char *p = e->p;
    if (*p == '(') {
        ++e->p;
        Val v = expr(e);
        spaces(e);
        if (*e->p == ')') ++e->p; else e->problems |= P_SYNTAX;
        return v;
    }
    if (*p == '"') {
        e->problems |= P_UNSUPPORTED;
        const char *end = skip_quoted(e->p);
        if (end[-1] != '"' || end == e->p + 1) e->problems |= P_SYNTAX;
        e->p = end;
        return error(E_VALUE);
    }
    if (*p == '{') { e->problems |= P_UNSUPPORTED; skip_group(e, '{', '}'); return error(E_VALUE); }
    if (*p == '[') { e->problems |= P_UNSUPPORTED; skip_group(e, '[', ']'); return error(E_VALUE); }
    if (*p == '#') {
        e->problems |= P_UNSUPPORTED;
        ++e->p;
        while (isalnum((unsigned char)*e->p) || *e->p == '/') ++e->p;
        if (*e->p == '!' || *e->p == '?') ++e->p;
        return error(E_VALUE);
    }
    if (*p == '\'') {
        /* 'Sheet name'!A1 */
        e->problems |= P_UNSUPPORTED;
        e->p = skip_quoted(e->p);
        if (*e->p == '!') {
            ++e->p;
            e->p += word(e->p);
            if (*after_spaces(e->p) == ':') {
                e->p = after_spaces(after_spaces(e->p) + 1);
                e->p += word(e->p);
            }
        } else e->problems |= P_SYNTAX;
        return error(E_REF);
    }
    size_t n = word(p);
    if (!n) {
        e->problems |= P_SYNTAX;
        if (*e->p) ++e->p;
        return error(E_VALUE);
    }
    const char *next = after_spaces(p + n);
    if (*next == '!') {
        e->problems |= P_UNSUPPORTED;
        e->p = next + 1; e->p += word(e->p);
        if (*after_spaces(e->p) == ':') { e->p = after_spaces(after_spaces(e->p) + 1); e->p += word(e->p); }
        return error(E_REF);
    }
    if (*next == '(' && !isdigit((unsigned char)p[0])) { e->p = next; return function(e, p, n); }
    Val r = reference(e, p, n);
    if (r.k == V_REF) return r;
    if (isdigit((unsigned char)p[0]) || p[0] == '.') {
        char *end;
        double d = strtod(p, &end);
        if (end == p) { e->problems |= P_SYNTAX; e->p = p + n; return error(E_VALUE); }
        e->p = end;
        return num(d);
    }
    e->p = p + n;
    if (n == 4 && !strncasecmp(p, "TRUE", 4)) { Val v = {V_BOOL, 0, 0, 1, 0, {0, 0, 0, 0}}; return v; }
    if (n == 5 && !strncasecmp(p, "FALSE", 5)) { Val v = {V_BOOL, 0, 0, 0, 0, {0, 0, 0, 0}}; return v; }
    e->problems |= P_UNSUPPORTED;   /* a defined name */
    return error(E_NAME);
}
static Val arithmetic(Ev *e, Val a, Val b, char op)
{
    double x, y;
    int err = 0;
    a = deref(e, a); b = deref(e, b);
    if (!to_number(e, a, &x, &err)) return error(err);
    if (!to_number(e, b, &y, &err)) return error(err);
    double r;
    switch (op) {
    case '+': r = x + y; break;
    case '-': r = x - y; break;
    case '*': r = x * y; break;
    case '/':
        if (y == 0) return error(E_DIV0);
        r = x / y;
        break;
    default:
        if (x == 0 && y < 0) return error(E_DIV0);
        r = pow(x, y);
        break;
    }
    return isfinite(r) ? num(r) : error(E_NUM);
}
static Val unary(Ev *e)
{
    spaces(e);
    if (*e->p == '-' || *e->p == '+') {
        char op = *e->p++;
        Val v = unary(e);
        if (op == '+') return v;
        return arithmetic(e, num(0), v, '-');
    }
    if (*e->p == '@') { e->problems |= P_UNSUPPORTED; ++e->p; }
    Val v = primary(e);
    spaces(e);
    while (*e->p == '%') { ++e->p; v = arithmetic(e, v, num(100), '/'); spaces(e); }
    return v;
}
static Val power(Ev *e)
{
    Val v = unary(e);
    for (;;) {
        spaces(e);
        if (*e->p != '^') return v;
        ++e->p;
        v = arithmetic(e, v, unary(e), '^');
    }
}
static Val term(Ev *e)
{
    Val v = power(e);
    for (;;) {
        spaces(e);
        char op = *e->p;
        if (op != '*' && op != '/') return v;
        ++e->p;
        v = arithmetic(e, v, power(e), op);
    }
}
static Val sum(Ev *e)
{
    Val v = term(e);
    for (;;) {
        spaces(e);
        char op = *e->p;
        if (op != '+' && op != '-') return v;
        ++e->p;
        v = arithmetic(e, v, term(e), op);
    }
}
static Val expr(Ev *e)
{
    Val v = sum(e);
    for (;;) {
        spaces(e);
        char op = *e->p;
        if (op != '=' && op != '<' && op != '>' && op != '&') return v;
        e->problems |= P_UNSUPPORTED;
        ++e->p;
        if (*e->p == '=' || *e->p == '>') ++e->p;
        sum(e);
        v = error(E_VALUE);
    }
}
static Val eval_formula(Ev *e)
{
    Val v = expr(e);
    spaces(e);
    if (*e->p) e->problems |= P_SYNTAX;
    return deref(e, v);
}

FormulaCheck formula_check(const char *text)
{
    Ev e = {NULL, NULL, text, 0, 0, 0, 0, NULL};
    eval_formula(&e);
    if (e.problems & P_FUNCTION) return FORMULA_FUNCTION;
    if (e.problems & P_UNSUPPORTED) return FORMULA_UNSUPPORTED;
    if (e.problems & P_SYNTAX) return FORMULA_SYNTAX;
    return FORMULA_OK;
}

int sheet_recalc(Book *b, Sheet *s)
{
    if (++s->gen == 0) { s->gen = 1; for (size_t i = 0; i < s->count; ++i) s->cells[i].gen = 0; }
    Stack st = {NULL, 0, 0, 0};
    int circular = 0;
    for (size_t i = 0; i < s->count && !st.failed; ++i) {
        SheetCell *c = &s->cells[i];
        if ((c->flags & (SF_FORMULA | SF_CALC)) != (SF_FORMULA | SF_CALC)) continue;
        if (c->gen == s->gen && c->eval == EV_DONE) continue;
        circular |= evaluate(b, s, c, &st);
    }
    int failed = st.failed;
    free(st.v);
    return failed ? 2 : circular ? 1 : 0;
}

/* ---- moving references (shared formulas) ---- */
typedef struct { char *data; size_t len, cap; int failed; } Out;
static void out_add(Out *o, const char *s, size_t n)
{
    if (o->failed) return;
    if (o->len + n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 64;
        while (cap < o->len + n + 1) cap *= 2;
        char *p = realloc(o->data, cap);
        if (!p) { o->failed = 1; return; }
        o->data = p; o->cap = cap;
    }
    memcpy(o->data + o->len, s, n);
    o->len += n;
    o->data[o->len] = 0;
}
static int moved(uint32_t v, long d, int absolute, uint32_t limit, uint32_t *out)
{
    long long r = absolute ? (long long)v : (long long)v + d;
    if (r < 0 || r >= (long long)limit) return 0;
    *out = (uint32_t)r;
    return 1;
}
static int put_cell(Out *o, const char *w, size_t n, long dr, long dc)
{
    uint32_t r, c;
    int ar, ac;
    if (!match_cell(w, n, &r, &c, &ar, &ac)) return 0;
    if (!moved(r, dr, ar, SHEET_MAX_ROWS, &r) || !moved(c, dc, ac, SHEET_MAX_COLS, &c)) return -1;
    char name[8], buf[32];
    sheet_column_name(c, name, sizeof(name));
    snprintf(buf, sizeof(buf), "%s%s%s%u", ac ? "$" : "", name, ar ? "$" : "", r + 1);
    out_add(o, buf, strlen(buf));
    return 1;
}
char *formula_shift(const char *text, long dr, long dc)
{
    Out o = {NULL, 0, 0, 0};
    out_add(&o, "", 0);
    const char *p = text;
    while (*p && !o.failed) {
        if (*p == '"' || *p == '\'') {
            const char *s = p;
            p = skip_quoted(p);
            out_add(&o, s, (size_t)(p - s));
            continue;
        }
        if (*p == '[') {
            const char *s = p;
            int level = 0;
            do {
                if (*p == '[') ++level;
                else if (*p == ']') --level;
                ++p;
            } while (*p && level);
            out_add(&o, s, (size_t)(p - s));
            continue;
        }
        if (!ident_char(*p)) { out_add(&o, p, 1); ++p; continue; }
        size_t n = word(p);
        const char *next = after_spaces(p + n);
        /* Function names and sheet names stay as they are. */
        if ((*next == '(' && !isdigit((unsigned char)*p)) || *next == '!') { out_add(&o, p, n); p += n; continue; }
        int done = put_cell(&o, p, n, dr, dc);
        if (done < 0) { free(o.data); return NULL; }
        if (done) { p += n; continue; }
        if (*next == ':') {
            const char *q = after_spaces(next + 1);
            size_t m = word(q);
            uint32_t a, b;
            int x, y;
            if (match_column(p, n, &a, &x) && match_column(q, m, &b, &y)) {
                char n1[8], n2[8];
                if (!moved(a, dc, x, SHEET_MAX_COLS, &a) || !moved(b, dc, y, SHEET_MAX_COLS, &b)) { free(o.data); return NULL; }
                sheet_column_name(a, n1, sizeof(n1)); sheet_column_name(b, n2, sizeof(n2));
                char buf[32];
                snprintf(buf, sizeof(buf), "%s%s:%s%s", x ? "$" : "", n1, y ? "$" : "", n2);
                out_add(&o, buf, strlen(buf));
                p = q + m;
                continue;
            }
            if (match_row(p, n, &a, &x) && match_row(q, m, &b, &y)) {
                if (!moved(a, dr, x, SHEET_MAX_ROWS, &a) || !moved(b, dr, y, SHEET_MAX_ROWS, &b)) { free(o.data); return NULL; }
                char buf[40];
                snprintf(buf, sizeof(buf), "%s%u:%s%u", x ? "$" : "", a + 1, y ? "$" : "", b + 1);
                out_add(&o, buf, strlen(buf));
                p = q + m;
                continue;
            }
        }
        out_add(&o, p, n);
        p += n;
    }
    if (o.failed) { free(o.data); return NULL; }
    return o.data;
}
