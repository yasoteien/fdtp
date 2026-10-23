// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
/* The -lh5- compressor (see lha_lh5.h).
 *
 * Stream layout, as LHa decoders read it (bits are packed from the most
 * significant end of each byte):
 *   block:  16 bits  number of symbols in the block
 *           temp table   (code lengths of the code-length alphabet, NT=19)
 *           code table   (lengths of literal/length codes, NC=510), coded
 *                        with the temp table
 *           offset table (lengths of the position codes, NP=14)
 *           symbols
 *   A literal/length symbol below 256 is a byte; 256+n is a match of n+3
 *   bytes followed by a position code p and, for p>1, p-1 extra bits:
 *   the value v = distance-1 is 0 (p=0) or 2^(p-1)+extra.
 *   Huffman codes are canonical (shorter codes first, then by symbol) and at
 *   most 16 bits long. A table holding a single symbol is sent as count 0
 *   followed by that symbol; it then takes no bits per use. */
#include "lha_lh5.h"
#include <stdlib.h>
#include <string.h>

enum {
    DICSIZ = 8192,
    MAXDIST = DICSIZ - 1,        /* stay one byte inside the decoders' ring */
    MAXMATCH = 256, THRESHOLD = 3,
    NC = 256 + MAXMATCH - THRESHOLD + 1, /* 510 */
    NP = 14, NT = 19,
    CBIT = 9, TBIT = 5, PBIT = 4,
    MAXBITS = 16,
    BLOCK_SYMBOLS = 16384,       /* the count field holds up to 65535 */
    HBITS = 15, HSIZE = 1 << HBITS,
    BUFSIZE = 4 * DICSIZ,
    OUTSIZE = 65536,
};

struct Lh5Encoder {
    Lh5Output output; void *ctx; int failed;
    int chain, lazy;             /* search effort from the level */
    unsigned char buf[BUFSIZE];
    int64_t base;                /* position of buf[0], counted over all streams */
    int64_t origin;              /* where the current stream began */
    int pos, end;
    int64_t head[HSIZE], prev[DICSIZ];
    int match_available, prev_len; unsigned prev_dist;
    /* The block being collected. */
    uint16_t sym[BLOCK_SYMBOLS], val[BLOCK_SYMBOLS];
    int count;
    uint32_t c_freq[NC], p_freq[NP], t_freq[NT];
    unsigned char c_len[NC], p_len[NP], t_len[NT];
    uint16_t c_code[NC], p_code[NP], t_code[NT];
    /* Bits out. */
    uint32_t bits; int nbits;
    unsigned char out[OUTSIZE]; size_t outn;
    uint64_t written;
};

/* ---- bits ---------------------------------------------------------------- */

static void flush_out(Lh5Encoder *e)
{
    if (!e->outn) return;
    if (!e->failed && e->output(e->ctx, e->out, e->outn) < 0) e->failed = 1;
    e->written += e->outn;
    e->outn = 0;
}
static void putbits(Lh5Encoder *e, int n, unsigned v)
{
    if (n <= 0) return;
    e->bits = (e->bits << n) | (v & ((1u << n) - 1));
    e->nbits += n;
    while (e->nbits >= 8) {
        e->nbits -= 8;
        e->out[e->outn++] = (unsigned char)(e->bits >> e->nbits);
        if (e->outn == OUTSIZE) flush_out(e);
    }
}
static void putcode(Lh5Encoder *e, int len, unsigned code) { putbits(e, len, code); }

/* ---- Huffman code lengths -------------------------------------------------- */

/* Lengths (at most MAXBITS) for the n symbols with nonzero freq, forming a
 * complete prefix code. Returns how many symbols are used; with fewer than
 * two the lengths are all 0 and *single is the used symbol (or 0). */
static int make_lengths(int n, const uint32_t *freq, unsigned char *len, int *single)
{
    uint32_t weight[2 * NC];
    int parent[2 * NC], heap[2 * NC], depth[2 * NC], leaf[NC];
    int used = 0;
    memset(len, 0, (size_t)n);
    *single = 0;
    for (int i = 0; i < n; ++i) if (freq[i]) { leaf[used++] = i; *single = i; }
    if (used < 2) return used;
    for (int i = 0; i < used; ++i) weight[i] = freq[leaf[i]];
    for (;;) {
        /* A binary heap of node indices ordered by weight, then index. */
        int hn = 0, nodes = used;
#define LESS(a, b) (weight[a] < weight[b] || (weight[a] == weight[b] && (a) < (b)))
        for (int i = 0; i < used; ++i) {
            int k = hn++;
            heap[k] = i;
            while (k && LESS(heap[k], heap[(k - 1) / 2])) { int t = heap[k]; heap[k] = heap[(k - 1) / 2]; heap[(k - 1) / 2] = t; k = (k - 1) / 2; }
        }
        while (hn > 1) {
            int two[2];
            for (int j = 0; j < 2; ++j) {
                two[j] = heap[0];
                heap[0] = heap[--hn];
                for (int k = 0;;) {
                    int l = 2 * k + 1, r = l + 1, m = k;
                    if (l < hn && LESS(heap[l], heap[m])) m = l;
                    if (r < hn && LESS(heap[r], heap[m])) m = r;
                    if (m == k) break;
                    int t = heap[k]; heap[k] = heap[m]; heap[m] = t; k = m;
                }
            }
            int node = nodes++;
            weight[node] = weight[two[0]] + weight[two[1]];
            parent[two[0]] = parent[two[1]] = node;
            int k = hn++;
            heap[k] = node;
            while (k && LESS(heap[k], heap[(k - 1) / 2])) { int t = heap[k]; heap[k] = heap[(k - 1) / 2]; heap[(k - 1) / 2] = t; k = (k - 1) / 2; }
        }
#undef LESS
        int root = nodes - 1, deepest = 0;
        depth[root] = 0;
        for (int i = root - 1; i >= 0; --i) depth[i] = depth[parent[i]] + 1; /* parents come later */
        for (int i = 0; i < used; ++i) if (depth[i] > deepest) deepest = depth[i];
        if (deepest <= MAXBITS) {
            for (int i = 0; i < used; ++i) len[leaf[i]] = (unsigned char)depth[i];
            return used;
        }
        /* Too deep: flatten the weights and build again. */
        for (int i = 0; i < used; ++i) weight[i] = (weight[i] + 1) / 2;
    }
}
static void make_codes(int n, const unsigned char *len, uint16_t *code)
{
    unsigned count[MAXBITS + 2] = {0}, next[MAXBITS + 2];
    for (int i = 0; i < n; ++i) ++count[len[i]];
    count[0] = 0;
    next[1] = 0;
    for (int l = 1; l <= MAXBITS; ++l) next[l + 1] = (next[l] + count[l]) << 1;
    for (int i = 0; i < n; ++i) code[i] = len[i] ? (uint16_t)next[len[i]]++ : 0;
}

/* ---- tables -------------------------------------------------------------- */

static int bit_length(unsigned v) { int n = 0; while (v) { ++n; v >>= 1; } return n; }

/* The temp or offset table: count, then each length in 3 bits (7 and above
 * as 7 plus a run of 1 bits ended by 0). After the third length of the temp
 * table, 2 bits give a run of zero lengths (0-3). */
static void write_pt_len(Lh5Encoder *e, int total, int nbit, int special, const unsigned char *len)
{
    int n = total;
    while (n > 0 && !len[n - 1]) --n;
    putbits(e, nbit, (unsigned)n);
    for (int i = 0; i < n;) {
        int k = len[i++];
        if (k <= 6) putbits(e, 3, (unsigned)k);
        else putbits(e, k - 3, (1u << (k - 3)) - 2);
        if (i == special) {
            while (i < 6 && !len[i]) ++i;
            putbits(e, 2, (unsigned)((i - 3) & 3));
        }
    }
}
static int c_table_end(const Lh5Encoder *e) { int n = NC; while (n > 0 && !e->c_len[n - 1]) --n; return n; }
/* Code lengths as temp symbols: 0 one zero, 1 a run of 3-18 zeros (4 bits),
 * 2 a run of 20-531 zeros (9 bits), k+2 the length k. 19 zeros: 0 then 1. */
static void count_t_freq(Lh5Encoder *e)
{
    memset(e->t_freq, 0, sizeof(e->t_freq));
    int n = c_table_end(e);
    for (int i = 0; i < n;) {
        int k = e->c_len[i++];
        if (k) { ++e->t_freq[k + 2]; continue; }
        int run = 1;
        while (i < n && !e->c_len[i]) { ++i; ++run; }
        if (run <= 2) e->t_freq[0] += (uint32_t)run;
        else if (run <= 18) ++e->t_freq[1];
        else if (run == 19) { ++e->t_freq[0]; ++e->t_freq[1]; }
        else ++e->t_freq[2];
    }
}
static void write_c_len(Lh5Encoder *e)
{
    int n = c_table_end(e);
    putbits(e, CBIT, (unsigned)n);
    for (int i = 0; i < n;) {
        int k = e->c_len[i++];
        if (k) { putcode(e, e->t_len[k + 2], e->t_code[k + 2]); continue; }
        int run = 1;
        while (i < n && !e->c_len[i]) { ++i; ++run; }
        if (run <= 2) for (int j = 0; j < run; ++j) putcode(e, e->t_len[0], e->t_code[0]);
        else if (run <= 18) { putcode(e, e->t_len[1], e->t_code[1]); putbits(e, 4, (unsigned)(run - 3)); }
        else if (run == 19) { putcode(e, e->t_len[0], e->t_code[0]); putcode(e, e->t_len[1], e->t_code[1]); putbits(e, 4, 15); }
        else { putcode(e, e->t_len[2], e->t_code[2]); putbits(e, CBIT, (unsigned)(run - 20)); }
    }
}

static void send_block(Lh5Encoder *e)
{
    if (!e->count) return;
    int single;
    putbits(e, 16, (unsigned)e->count);
    if (make_lengths(NC, e->c_freq, e->c_len, &single) >= 2) {
        make_codes(NC, e->c_len, e->c_code);
        count_t_freq(e);
        int t_single;
        if (make_lengths(NT, e->t_freq, e->t_len, &t_single) >= 2) {
            make_codes(NT, e->t_len, e->t_code);
            write_pt_len(e, NT, TBIT, 3, e->t_len);
        } else {
            memset(e->t_code, 0, sizeof(e->t_code));
            putbits(e, TBIT, 0); putbits(e, TBIT, (unsigned)t_single);
        }
        write_c_len(e);
    } else {
        memset(e->c_code, 0, sizeof(e->c_code));
        putbits(e, TBIT, 0); putbits(e, TBIT, 0);
        putbits(e, CBIT, 0); putbits(e, CBIT, (unsigned)single);
    }
    if (make_lengths(NP, e->p_freq, e->p_len, &single) >= 2) {
        make_codes(NP, e->p_len, e->p_code);
        write_pt_len(e, NP, PBIT, -1, e->p_len);
    } else {
        memset(e->p_code, 0, sizeof(e->p_code));
        putbits(e, PBIT, 0); putbits(e, PBIT, (unsigned)single);
    }
    for (int i = 0; i < e->count; ++i) {
        unsigned s = e->sym[i];
        putcode(e, e->c_len[s], e->c_code[s]);
        if (s < 256) continue;
        unsigned v = e->val[i];
        int p = bit_length(v);
        putcode(e, e->p_len[p], e->p_code[p]);
        if (p > 1) putbits(e, p - 1, v - (1u << (p - 1)));
    }
    e->count = 0;
    memset(e->c_freq, 0, sizeof(e->c_freq));
    memset(e->p_freq, 0, sizeof(e->p_freq));
}

static void emit_literal(Lh5Encoder *e, unsigned char c)
{
    e->sym[e->count] = c; e->val[e->count] = 0; ++e->c_freq[c];
    if (++e->count == BLOCK_SYMBOLS) send_block(e);
}
static void emit_match(Lh5Encoder *e, int len, unsigned dist)
{
    unsigned s = (unsigned)(len - THRESHOLD + 256), v = dist - 1;
    e->sym[e->count] = (uint16_t)s; e->val[e->count] = (uint16_t)v;
    ++e->c_freq[s]; ++e->p_freq[bit_length(v)];
    if (++e->count == BLOCK_SYMBOLS) send_block(e);
}

/* ---- matching ------------------------------------------------------------ */

static unsigned hash3(const unsigned char *p) { return (((unsigned)p[0] << 10) ^ ((unsigned)p[1] << 5) ^ p[2]) & (HSIZE - 1); }
/* Enter position p (three bytes available) into its hash chain. */
static void insert(Lh5Encoder *e, int p)
{
    int64_t a = e->base + p;
    unsigned h = hash3(e->buf + p);
    e->prev[a & (DICSIZ - 1)] = e->head[h];
    e->head[h] = a;
}
/* The longest earlier match for the bytes at p (already inserted): length
 * (0 when shorter than THRESHOLD) and distance. */
static int longest(Lh5Encoder *e, int p, unsigned *dist)
{
    int limit = e->end - p < MAXMATCH ? e->end - p : MAXMATCH, best = THRESHOLD - 1;
    if (limit < THRESHOLD) return 0;
    int64_t a = e->base + p;
    const unsigned char *s = e->buf + p;
    int chain = e->chain;
    for (int64_t c = e->prev[a & (DICSIZ - 1)]; c >= 0 && chain-- > 0;) {
        int64_t d = a - c;
        if (d > MAXDIST || d <= 0 || c < e->origin) break;
        const unsigned char *q = e->buf + (c - e->base);
        if (q[best] == s[best] && q[0] == s[0] && q[1] == s[1]) {
            int n = 2;
            while (n < limit && q[n] == s[n]) ++n;
            if (n > best) { best = n; *dist = (unsigned)d; if (n == limit) break; }
        }
        int64_t next = e->prev[c & (DICSIZ - 1)];
        if (next >= c) break;
        c = next;
    }
    return best >= THRESHOLD ? best : 0;
}

/* Turn buffered bytes into symbols; without final, keep MAXMATCH bytes of
 * lookahead so that every search sees as far as a match can reach. */
static void process(Lh5Encoder *e, int final)
{
    if (!e->lazy) {
        while (e->pos < e->end && (final || e->end - e->pos >= MAXMATCH)) {
            int len = 0; unsigned dist = 0;
            if (e->pos + THRESHOLD <= e->end) { insert(e, e->pos); len = longest(e, e->pos, &dist); }
            if (len) {
                emit_match(e, len, dist);
                int stop = e->pos + len;
                for (++e->pos; e->pos < stop; ++e->pos) if (e->pos + THRESHOLD <= e->end) insert(e, e->pos);
            } else emit_literal(e, e->buf[e->pos++]);
        }
        return;
    }
    while (e->pos < e->end && (final || e->end - e->pos >= MAXMATCH)) {
        int len = 0; unsigned dist = 0;
        if (e->pos + THRESHOLD <= e->end) {
            insert(e, e->pos);
            if (e->prev_len < e->lazy) len = longest(e, e->pos, &dist);
        }
        if (e->prev_len >= THRESHOLD && len <= e->prev_len) {
            /* The match found one byte back is at least as good. */
            emit_match(e, e->prev_len, e->prev_dist);
            int stop = e->pos - 1 + e->prev_len;
            for (++e->pos; e->pos < stop; ++e->pos) if (e->pos + THRESHOLD <= e->end) insert(e, e->pos);
            e->match_available = 0; e->prev_len = 0;
            continue;
        }
        if (e->match_available) emit_literal(e, e->buf[e->pos - 1]);
        e->match_available = 1; e->prev_len = len; e->prev_dist = dist;
        ++e->pos;
    }
    if (final && e->match_available) { emit_literal(e, e->buf[e->pos - 1]); e->match_available = 0; e->prev_len = 0; }
}

/* Drop all but DICSIZ bytes of history before pos. */
static void slide(Lh5Encoder *e)
{
    int shift = e->pos - DICSIZ;
    if (shift <= 0) return;
    memmove(e->buf, e->buf + shift, (size_t)(e->end - shift));
    e->base += shift; e->pos -= shift; e->end -= shift;
}

/* ---- public -------------------------------------------------------------- */

Lh5Encoder *lh5_create(int level, Lh5Output output, void *ctx)
{
    static const int chains[10] = {0, 4, 8, 16, 16, 32, 128, 256, 1024, 4096};
    static const int lazies[10] = {0, 0, 0, 0, 16, 32, 128, MAXMATCH, MAXMATCH, MAXMATCH};
    if (level < 1) level = 1;
    if (level > 9) level = 9;
    Lh5Encoder *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->output = output; e->ctx = ctx;
    e->chain = chains[level]; e->lazy = lazies[level];
    for (int i = 0; i < HSIZE; ++i) e->head[i] = -1;
    for (int i = 0; i < DICSIZ; ++i) e->prev[i] = -1;
    return e;
}
int lh5_write(Lh5Encoder *e, const void *data, size_t size)
{
    const unsigned char *p = data;
    while (size && !e->failed) {
        if (e->end == BUFSIZE) slide(e);
        size_t room = (size_t)(BUFSIZE - e->end), n = size < room ? size : room;
        memcpy(e->buf + e->end, p, n);
        e->end += (int)n; p += n; size -= n;
        process(e, 0);
    }
    return e->failed ? -1 : 0;
}
int lh5_finish(Lh5Encoder *e)
{
    process(e, 1);
    send_block(e);
    if (e->nbits) { e->out[e->outn++] = (unsigned char)(e->bits << (8 - e->nbits)); e->nbits = 0; }
    flush_out(e);
    return e->failed ? -1 : 0;
}
void lh5_reset(Lh5Encoder *e)
{
    e->origin = e->base = e->base + e->end;
    e->pos = e->end = 0;
    e->match_available = 0; e->prev_len = 0; e->prev_dist = 0;
    e->count = 0;
    memset(e->c_freq, 0, sizeof(e->c_freq));
    memset(e->p_freq, 0, sizeof(e->p_freq));
    e->bits = 0; e->nbits = 0; e->outn = 0; e->written = 0; e->failed = 0;
}
uint64_t lh5_output_size(const Lh5Encoder *e) { return e->written + e->outn; }
void lh5_free(Lh5Encoder *e) { free(e); }
