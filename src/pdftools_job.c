// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
/* Revision 1 item 39: the writing behind the PDF menu (pdftools_job.h),
 * with the bundled MuPDF 1.25. Built on Linux only (mk/pdfview.mk). */
#include "i18n.h"
#include "pdftools_job.h"
#ifdef FDTP_WITH_MUPDF
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

/* MuPDF's resource cache, as the viewer's (the device has 994 MB). */
#define JOB_STORE (32u << 20)
/* A page drawn for JPEG at most this many pixels (150 MB of RGB; an A3 page
 * at 300 dpi is 17 million). */
#define JOB_MAX_PIXELS 50e6
/* The largest page side a PDF allows (points). */
#define JOB_MAX_SIDE 14400.f

enum { STAGE_WORK, STAGE_SAVE, STAGE_CHECK, STAGE_RENAME, STAGE_DONE };

struct PdfJob {
    JobKind kind;
    char **sources, **passwords;
    size_t source_count;
    int *pages;
    size_t page_count;
    size_t *groups;
    size_t group_count;
    int angle, dpi, quality;
    char password[128], quality_text[16];
    char **outputs, **temps;    /* temps[i]: output i's hidden file until renamed */
    size_t output_count;
    int *out_pages, *out_w, *out_h;
    long long *out_bytes;
    fz_context *ctx;
    pdf_document *src, *dst;    /* the source being read, the document being made */
    pdf_graft_map *map;
    int src_pages, merge_pages;
    size_t source, at, out;     /* the next source, page, output */
    int stage, done, total;
    long long source_bytes;
    char what[192], error[256];
};

static void quiet(void *user, const char *message) { (void)user; (void)message; }
static double clock_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}
static const char *base_of(const char *path) { const char *b = strrchr(path, '/'); return b ? b + 1 : path; }
static long long file_bytes(const char *path) { struct stat st; return stat(path, &st) ? -1 : (long long)st.st_size; }
static void wipe(char *s) { if (s) { volatile char *p = s; while (*p) *p++ = 0; } }
static fz_context *new_context(void)
{
    fz_context *ctx = fz_new_context(NULL, NULL, JOB_STORE);
    if (ctx) { fz_set_warning_callback(ctx, quiet, NULL); fz_set_error_callback(ctx, quiet, NULL); }
    return ctx;
}

/* Opens path as a PDF with password (when it needs one); throws if either fails. */
static pdf_document *open_pdf(fz_context *ctx, const char *path, const char *password)
{
    pdf_document *doc = pdf_open_document(ctx, path);
    if (pdf_needs_password(ctx, doc) && !pdf_authenticate_password(ctx, doc, password ? password : "")) {
        pdf_drop_document(ctx, doc);
        fz_throw(ctx, FZ_ERROR_GENERIC, "wrong password");
    }
    return doc;
}

int job_probe(const char *path, const char *password, int *pages, int *encrypted)
{
    fz_context *ctx = new_context();
    if (!ctx) return -1;
    pdf_document *doc = NULL; int result = -1, n = 0, crypt = 0;
    fz_var(doc); fz_var(result); fz_var(n); fz_var(crypt);
    fz_try(ctx) {
        doc = pdf_open_document(ctx, path);
        crypt = doc->crypt != NULL;
        if (pdf_needs_password(ctx, doc) && !pdf_authenticate_password(ctx, doc, password ? password : "")) result = 1;
        else { n = pdf_count_pages(ctx, doc); result = n > 0 ? 0 : -1; }
    }
    fz_always(ctx) pdf_drop_document(ctx, doc);
    fz_catch(ctx) result = -1;
    fz_drop_context(ctx);
    if (pages) *pages = n;
    if (encrypted) *encrypted = crypt;
    return result;
}

/* ---- the hidden files beside the outputs ---------------------------------- */

/* ".NAME.fdtp-XXXXXX" in the output's directory (the name cut to 180 bytes). */
static char *temp_beside(const char *path)
{
    const char *base = base_of(path);
    size_t dir = (size_t)(base - path), len = strlen(base);
    if (len > 180) { len = 180; while (len && ((unsigned char)base[len] & 0xc0) == 0x80) --len; }
    size_t n = dir + len + 20;
    char *t = malloc(n);
    if (!t) return NULL;
    snprintf(t, n, "%.*s.%.*s.fdtp-XXXXXX", (int)dir, path, (int)len, base);
    int fd = mkstemp(t);
    if (fd < 0) { free(t); return NULL; }
    close(fd);
    return t;
}
static const char *temp_for(PdfJob *j, size_t i)
{
    if (!j->temps[i] && !(j->temps[i] = temp_beside(j->outputs[i]))) {
        snprintf(j->error, sizeof(j->error), T("書き込めません: %.120s（%s）"), base_of(j->outputs[i]), strerror(errno));
        fz_throw(j->ctx, FZ_ERROR_GENERIC, "temporary file");
    }
    return j->temps[i];
}
static void remove_temps(PdfJob *j)
{
    for (size_t i = 0; j->temps && i < j->output_count; ++i)
        if (j->temps[i]) { unlink(j->temps[i]); free(j->temps[i]); j->temps[i] = NULL; }
}
/* Every output into place, with the mode of the file it replaces (or the
 * umask's, as a new file). */
static int rename_all(PdfJob *j)
{
    mode_t mask = umask(0);
    umask(mask);
    for (size_t i = 0; i < j->output_count; ++i) {
        if (!j->temps[i]) continue;
        struct stat st;
        chmod(j->temps[i], !stat(j->outputs[i], &st) ? st.st_mode & 07777 : 0666 & ~mask);
        if (rename(j->temps[i], j->outputs[i]) < 0) {
            snprintf(j->error, sizeof(j->error), T("名前を変えられません: %.120s（%s）"), base_of(j->outputs[i]), strerror(errno));
            return -1;
        }
        free(j->temps[i]); j->temps[i] = NULL;
    }
    return 0;
}

/* ---- the steps ----------------------------------------------------------- */

static void save_doc(PdfJob *j, pdf_document *doc, size_t i)
{
    fz_context *ctx = j->ctx;
    const char *temp = temp_for(j, i);
    pdf_write_options o = pdf_default_write_options;
    o.do_garbage = 1; o.do_compress = 1; o.do_compress_images = 1; o.do_compress_fonts = 1;
    if (j->kind == JOB_COMPRESS) { o.do_garbage = 3; o.do_use_objstms = 1; }
    if (j->kind == JOB_PROTECT) {
        o.do_encrypt = PDF_ENCRYPT_AES_256;
        o.permissions = PDF_PERM_PRINT | PDF_PERM_MODIFY | PDF_PERM_COPY | PDF_PERM_ANNOTATE | PDF_PERM_FORM |
                        PDF_PERM_ACCESSIBILITY | PDF_PERM_ASSEMBLE | PDF_PERM_PRINT_HQ;
        snprintf(o.upwd_utf8, sizeof(o.upwd_utf8), "%s", j->password);
        snprintf(o.opwd_utf8, sizeof(o.opwd_utf8), "%s", j->password);
    }
    if (j->kind == JOB_UNLOCK) o.do_encrypt = PDF_ENCRYPT_NONE;
    snprintf(j->what, sizeof(j->what), T("書き出し中 %s"), base_of(j->outputs[i]));
    fz_try(ctx) pdf_save_document(ctx, doc, temp, &o);
    fz_always(ctx) { wipe(o.upwd_utf8); wipe(o.opwd_utf8); }
    fz_catch(ctx) fz_rethrow(ctx);
}

/* Opens output i's hidden file again: encrypt 1 must have a password, 0
 * must have none, -1 either; it must open with password and have expect
 * pages (-1: any). Fills its pages and bytes. */
static void check_output(PdfJob *j, size_t i, const char *password, int encrypt, int expect)
{
    fz_context *ctx = j->ctx; pdf_document *doc = NULL; int n = 0;
    fz_var(doc); fz_var(n);
    snprintf(j->what, sizeof(j->what), T("確かめ中 %s"), base_of(j->outputs[i]));
    fz_try(ctx) {
        doc = pdf_open_document(ctx, j->temps[i]);
        int needs = pdf_needs_password(ctx, doc), crypt = doc->crypt != NULL;
        if (encrypt == 1 && !needs) snprintf(j->error, sizeof(j->error), "%s", T("パスワードが付いていません"));
        else if (encrypt == 0 && crypt) snprintf(j->error, sizeof(j->error), "%s", T("パスワードが残っています"));
        else if (needs && !pdf_authenticate_password(ctx, doc, password ? password : ""))
            snprintf(j->error, sizeof(j->error), "%s", T("書き出した PDF がパスワードで開けません"));
        else if (pdf_was_repaired(ctx, doc)) snprintf(j->error, sizeof(j->error), "%s", T("書き出した PDF が壊れています"));
        else if ((n = pdf_count_pages(ctx, doc)) < 1 || (expect >= 0 && n != expect))
            snprintf(j->error, sizeof(j->error), T("書き出した PDF のページの数が合いません（%d ではなく %d）"), expect, n);
        if (j->error[0]) fz_throw(ctx, FZ_ERROR_GENERIC, "check");
    }
    fz_always(ctx) pdf_drop_document(ctx, doc);
    fz_catch(ctx) {
        if (!j->error[0]) snprintf(j->error, sizeof(j->error), T("書き出した PDF を開けません: %.120s"), base_of(j->outputs[i]));
        fz_rethrow(ctx);
    }
    j->out_pages[i] = n;
    j->out_bytes[i] = file_bytes(j->temps[i]);
}

/* The last page of dst: page `from` of src (with what it inherits, its
 * annotations; MuPDF's pdf_graft_mapped_page). */
static void graft(PdfJob *j, int from)
{
    pdf_graft_mapped_page(j->ctx, j->map, pdf_count_pages(j->ctx, j->dst), j->src, from);
}

static void add_image_page(PdfJob *j, const char *path)
{
    fz_context *ctx = j->ctx;
    fz_image *image = NULL; pdf_obj *ref = NULL, *resources = NULL, *page = NULL; fz_buffer *contents = NULL;
    fz_var(image); fz_var(ref); fz_var(resources); fz_var(page); fz_var(contents);
    fz_try(ctx) {
        image = fz_new_image_from_file(ctx, path);
        int xres = 72, yres = 72;
        fz_image_resolution(image, &xres, &yres);   /* MuPDF picks a sane one when the file has none */
        float w = image->w * 72.f / (xres > 0 ? xres : 72), h = image->h * 72.f / (yres > 0 ? yres : 72);
        float big = fmaxf(w, h);
        if (big > JOB_MAX_SIDE) { w *= JOB_MAX_SIDE / big; h *= JOB_MAX_SIDE / big; }
        w = fmaxf(w, 1.f); h = fmaxf(h, 1.f);
        ref = pdf_add_image(ctx, j->dst, image);
        resources = pdf_new_dict(ctx, j->dst, 1);
        pdf_obj *xobjects = pdf_dict_put_dict(ctx, resources, PDF_NAME(XObject), 1);
        pdf_dict_puts(ctx, xobjects, "Im0", ref);
        contents = fz_new_buffer(ctx, 64);
        fz_append_printf(ctx, contents, "q %g 0 0 %g 0 0 cm /Im0 Do Q\n", w, h);
        page = pdf_add_page(ctx, j->dst, fz_make_rect(0, 0, w, h), 0, resources, contents);
        pdf_insert_page(ctx, j->dst, pdf_count_pages(ctx, j->dst), page);
    }
    fz_always(ctx) {
        pdf_drop_obj(ctx, page); pdf_drop_obj(ctx, resources); pdf_drop_obj(ctx, ref);
        fz_drop_buffer(ctx, contents); fz_drop_image(ctx, image);
    }
    fz_catch(ctx) {
        snprintf(j->error, sizeof(j->error), T("画像を読めません: %.120s"), base_of(path));
        fz_rethrow(ctx);
    }
}

static void draw_jpeg(PdfJob *j, size_t i)
{
    fz_context *ctx = j->ctx; fz_page *page = NULL; fz_pixmap *pix = NULL;
    int number = j->pages[i];
    fz_var(page); fz_var(pix);
    snprintf(j->what, sizeof(j->what), T("%zu/%zu ページ  %s"), i + 1, j->page_count, base_of(j->outputs[i]));
    fz_try(ctx) {
        page = fz_load_page(ctx, &j->src->super, number);
        fz_rect b = fz_bound_page(ctx, page);
        float s = j->dpi / 72.f;
        if ((double)(b.x1 - b.x0) * s * (double)(b.y1 - b.y0) * s > JOB_MAX_PIXELS) {
            snprintf(j->error, sizeof(j->error), T("%d ページ目が大きすぎます（解像度を下げてください）"), number + 1);
            fz_throw(ctx, FZ_ERROR_GENERIC, "page too large");
        }
        pix = fz_new_pixmap_from_page(ctx, page, fz_scale(s, s), fz_device_rgb(ctx), 0);
        fz_save_pixmap_as_jpeg(ctx, pix, temp_for(j, i), 85);
        j->out_w[i] = fz_pixmap_width(ctx, pix); j->out_h[i] = fz_pixmap_height(ctx, pix);
    }
    fz_always(ctx) { fz_drop_pixmap(ctx, pix); fz_drop_page(ctx, page); }
    fz_catch(ctx) fz_rethrow(ctx);
    j->out_pages[i] = 1;
    if ((j->out_bytes[i] = file_bytes(j->temps[i])) <= 0) {
        snprintf(j->error, sizeof(j->error), T("JPEG を書き出せません: %.120s"), base_of(j->outputs[i]));
        fz_throw(ctx, FZ_ERROR_GENERIC, "empty jpeg");
    }
}

static void compress_images(PdfJob *j)
{
    pdf_image_rewriter_options o;
    memset(&o, 0, sizeof(o));
    snprintf(j->quality_text, sizeof(j->quality_text), "%d", j->quality);
    int threshold = j->dpi * 3 / 2;
    o.color_lossless_image_subsample_method = o.color_lossy_image_subsample_method = FZ_SUBSAMPLE_AVERAGE;
    o.gray_lossless_image_subsample_method = o.gray_lossy_image_subsample_method = FZ_SUBSAMPLE_AVERAGE;
    o.color_lossless_image_subsample_threshold = o.color_lossy_image_subsample_threshold = threshold;
    o.gray_lossless_image_subsample_threshold = o.gray_lossy_image_subsample_threshold = threshold;
    o.color_lossless_image_subsample_to = o.color_lossy_image_subsample_to = j->dpi;
    o.gray_lossless_image_subsample_to = o.gray_lossy_image_subsample_to = j->dpi;
    /* Photos (JPEG) again as JPEG; drawings stay lossless; bitonal (fax) as they are. */
    o.color_lossless_image_recompress_method = o.gray_lossless_image_recompress_method = FZ_RECOMPRESS_LOSSLESS;
    o.color_lossy_image_recompress_method = o.gray_lossy_image_recompress_method = FZ_RECOMPRESS_JPEG;
    o.color_lossy_image_recompress_quality = o.gray_lossy_image_recompress_quality = j->quality_text;
    snprintf(j->what, sizeof(j->what), T("画像を作り直し中 %s"), base_of(j->sources[0]));
    pdf_rewrite_images(j->ctx, j->src, &o);
}

/* One unit of STAGE_WORK. */
static void work(PdfJob *j)
{
    fz_context *ctx = j->ctx;
    switch (j->kind) {
    case JOB_MERGE:
        if (!j->dst) j->dst = pdf_create_document(ctx);
        if (!j->src) {
            j->src = open_pdf(ctx, j->sources[j->source], j->passwords[j->source]);
            j->src_pages = pdf_count_pages(ctx, j->src);
            j->map = pdf_new_graft_map(ctx, j->dst);
            j->at = 0;
        }
        snprintf(j->what, sizeof(j->what), T("%s  %zu/%d ページ"), base_of(j->sources[j->source]), j->at + 1, j->src_pages);
        graft(j, (int)j->at++); ++j->done;
        if ((int)j->at >= j->src_pages) {
            pdf_drop_graft_map(ctx, j->map); j->map = NULL;
            pdf_drop_document(ctx, j->src); j->src = NULL;
            if (++j->source == j->source_count) j->stage = STAGE_SAVE;
        }
        return;
    case JOB_SPLIT: {
        size_t g = j->out, first = j->groups[g], last = j->groups[g + 1];
        if (!j->dst) { j->dst = pdf_create_document(ctx); j->map = pdf_new_graft_map(ctx, j->dst); j->at = first; }
        if (j->at < last) {
            snprintf(j->what, sizeof(j->what), T("%s  %zu/%zu ページ"), base_of(j->outputs[g]), j->at - first + 1, last - first);
            graft(j, j->pages[j->at++]); ++j->done;
            return;
        }
        save_doc(j, j->dst, g);
        pdf_drop_graft_map(ctx, j->map); j->map = NULL;
        pdf_drop_document(ctx, j->dst); j->dst = NULL;
        check_output(j, g, NULL, 0, (int)(last - first)); ++j->done;
        if (++j->out == j->group_count) j->stage = STAGE_RENAME;
        return;
    }
    case JOB_PAGES:
        snprintf(j->what, sizeof(j->what), T("ページを並べ中 %s"), base_of(j->sources[0]));
        pdf_rearrange_pages(ctx, j->src, (int)j->page_count, j->pages);
        ++j->done; j->stage = STAGE_SAVE;
        return;
    case JOB_ROTATE:
        if (j->at < j->page_count) {
            int number = j->pages[j->at++];
            snprintf(j->what, sizeof(j->what), T("回転中 %zu/%zu ページ"), j->at, j->page_count);
            pdf_obj *page = pdf_lookup_page_obj(ctx, j->src, number);
            int r = pdf_dict_get_inheritable_int(ctx, page, PDF_NAME(Rotate)) + j->angle;
            r = ((r % 360) + 360) % 360 / 90 * 90;
            pdf_dict_put_int(ctx, page, PDF_NAME(Rotate), r);
            ++j->done;
        }
        if (j->at >= j->page_count) j->stage = STAGE_SAVE;
        return;
    case JOB_COMPRESS:
        compress_images(j);
        ++j->done; j->stage = STAGE_SAVE;
        return;
    case JOB_JPEG:
        draw_jpeg(j, j->out); ++j->done;
        if (++j->out == j->output_count) j->stage = STAGE_RENAME;
        return;
    case JOB_IMAGES:
        if (!j->dst) j->dst = pdf_create_document(ctx);
        snprintf(j->what, sizeof(j->what), T("%zu/%zu 枚  %s"), j->source + 1, j->source_count, base_of(j->sources[j->source]));
        add_image_page(j, j->sources[j->source]); ++j->done;
        if (++j->source == j->source_count) j->stage = STAGE_SAVE;
        return;
    default:   /* PROTECT, UNLOCK, REPAIR: straight to the saving */
        j->stage = STAGE_SAVE;
        return;
    }
}

static void check(PdfJob *j)
{
    switch (j->kind) {
    case JOB_MERGE: check_output(j, 0, NULL, 0, j->merge_pages); break;
    case JOB_IMAGES: check_output(j, 0, NULL, 0, (int)j->source_count); break;
    case JOB_PAGES: check_output(j, 0, j->passwords[0], -1, (int)j->page_count); break;
    case JOB_PROTECT: check_output(j, 0, j->password, 1, j->src_pages); break;
    case JOB_UNLOCK: check_output(j, 0, NULL, 0, j->src_pages); break;
    default: check_output(j, 0, j->passwords[0], -1, j->src_pages); break;
    }
    if (j->kind == JOB_COMPRESS && j->source_bytes >= 0 && j->out_bytes[0] >= j->source_bytes) {
        snprintf(j->error, sizeof(j->error), "%s", T("小さくなりませんでした"));
        fz_throw(j->ctx, FZ_ERROR_GENERIC, "not smaller");
    }
}

static int step(PdfJob *j)
{
    fz_context *ctx = j->ctx; int failed = 0;
    fz_var(failed);
    fz_try(ctx) {
        switch (j->stage) {
        case STAGE_WORK: work(j); break;
        case STAGE_SAVE:
            save_doc(j, j->kind == JOB_MERGE || j->kind == JOB_IMAGES ? j->dst : j->src, 0);
            ++j->done; j->stage = STAGE_CHECK;
            break;
        case STAGE_CHECK: check(j); ++j->done; j->stage = STAGE_RENAME; break;
        case STAGE_RENAME:
            snprintf(j->what, sizeof(j->what), "%s", T("仕上げ中"));
            if (rename_all(j) < 0) fz_throw(ctx, FZ_ERROR_GENERIC, "rename");
            j->stage = STAGE_DONE;
            break;
        }
    }
    fz_catch(ctx) {
        if (!j->error[0]) snprintf(j->error, sizeof(j->error), T("書き出せませんでした（%.100s）"), fz_caught_message(ctx));
        failed = 1;
    }
    return failed ? -1 : 0;
}

int job_step(PdfJob *j, double seconds)
{
    if (!j || j->error[0]) return -1;
    double end = clock_now() + seconds;
    while (j->stage != STAGE_DONE) {
        if (step(j) < 0) { remove_temps(j); return -1; }
        if (clock_now() >= end) break;
    }
    return j->stage == STAGE_DONE;
}

/* ---- start and end -------------------------------------------------------- */

static char **copy_list(const char *const *from, size_t n, int *failed)
{
    char **to = calloc(n ? n : 1, sizeof(*to));
    if (!to) { *failed = 1; return NULL; }
    for (size_t i = 0; i < n; ++i) if (from && from[i] && !(to[i] = strdup(from[i]))) *failed = 1;
    return to;
}
static void free_list(char **list, size_t n, int secret)
{
    for (size_t i = 0; list && i < n; ++i) { if (secret) wipe(list[i]); free(list[i]); }
    free(list);
}

/* Opens the source (or counts the pages of all, for MERGE); the path that
 * failed, or NULL. wrong: a page outside the source. */
static const char *open_sources(PdfJob *j, int *wrong)
{
    fz_context *ctx = j->ctx; const char *volatile bad = NULL; size_t i = 0;
    fz_var(i);
    fz_try(ctx) {
        fz_register_document_handlers(ctx);
        if (j->kind == JOB_MERGE) {
            for (i = 0; i < j->source_count; ++i) {
                bad = j->sources[i];
                pdf_document *doc = open_pdf(ctx, j->sources[i], j->passwords[i]);
                fz_try(ctx) j->merge_pages += pdf_count_pages(ctx, doc);
                fz_always(ctx) pdf_drop_document(ctx, doc);
                fz_catch(ctx) fz_rethrow(ctx);
            }
        } else if (j->kind != JOB_IMAGES) {
            bad = j->sources[0];
            j->src = open_pdf(ctx, j->sources[0], j->passwords[0]);
            j->src_pages = pdf_count_pages(ctx, j->src);
            for (i = 0; i < j->page_count; ++i) if (j->pages[i] < 0 || j->pages[i] >= j->src_pages) *wrong = 1;
        }
    }
    fz_catch(ctx) return bad ? bad : "";
    return NULL;
}

PdfJob *job_start(const JobSpec *s, char *error, size_t size)
{
    PdfJob *j = calloc(1, sizeof(*j)); int failed = !j;
    if (failed) { snprintf(error, size, "%s", T("メモリが足りません")); return NULL; }
    j->kind = s->kind; j->angle = s->angle; j->dpi = s->dpi > 0 ? s->dpi : 150; j->quality = s->quality > 0 ? s->quality : 75;
    if (s->password) snprintf(j->password, sizeof(j->password), "%s", s->password);
    j->source_count = s->source_count; j->output_count = s->output_count;
    j->page_count = s->page_count; j->group_count = s->group_count;
    j->sources = copy_list(s->sources, s->source_count, &failed);
    j->passwords = copy_list(s->passwords, s->source_count, &failed);
    j->outputs = copy_list(s->outputs, s->output_count, &failed);
    j->temps = calloc(s->output_count ? s->output_count : 1, sizeof(*j->temps));
    j->out_pages = calloc(s->output_count ? s->output_count : 1, sizeof(int));
    j->out_w = calloc(s->output_count ? s->output_count : 1, sizeof(int));
    j->out_h = calloc(s->output_count ? s->output_count : 1, sizeof(int));
    j->out_bytes = calloc(s->output_count ? s->output_count : 1, sizeof(long long));
    j->pages = calloc(s->page_count ? s->page_count : 1, sizeof(int));
    j->groups = calloc(s->group_count + 1, sizeof(size_t));
    if (!j->temps || !j->out_pages || !j->out_w || !j->out_h || !j->out_bytes || !j->pages || !j->groups) failed = 1;
    if (!failed) {
        if (s->page_count) memcpy(j->pages, s->pages, s->page_count * sizeof(int));
        if (s->groups) memcpy(j->groups, s->groups, (s->group_count + 1) * sizeof(size_t));
    }
    if (failed || !s->source_count || !s->output_count || !(j->ctx = new_context())) {
        snprintf(error, size, "%s", T("メモリが足りません")); job_free(j); return NULL;
    }
    int wrong = 0;
    const char *bad = open_sources(j, &wrong);
    if (bad) {
        snprintf(error, size, T("PDF を開けません: %.120s"), base_of(bad));
        job_free(j); return NULL;
    }
    if (wrong || (j->kind == JOB_SPLIT && j->groups[j->group_count] != j->page_count) ||
        ((j->kind == JOB_SPLIT || j->kind == JOB_JPEG) && j->output_count != (j->kind == JOB_SPLIT ? j->group_count : j->page_count))) {
        snprintf(error, size, "%s", T("ページの範囲が違います")); job_free(j); return NULL;
    }
    j->source_bytes = file_bytes(j->sources[0]);
    switch (j->kind) {
    case JOB_MERGE: j->total = j->merge_pages + 2; break;
    case JOB_SPLIT: j->total = (int)(j->page_count + j->group_count); break;
    case JOB_PAGES: case JOB_COMPRESS: j->total = 3; break;
    case JOB_ROTATE: j->total = (int)j->page_count + 2; break;
    case JOB_JPEG: j->total = (int)j->page_count; break;
    case JOB_IMAGES: j->total = (int)j->source_count + 2; break;
    default: j->total = 2; j->stage = STAGE_SAVE; break;
    }
    snprintf(j->what, sizeof(j->what), "%s", base_of(j->sources[0]));
    return j;
}

const char *job_error(const PdfJob *j) { return j && j->error[0] ? j->error : T("書き出せませんでした"); }
void job_progress(const PdfJob *j, int *done, int *total, const char **what)
{
    *done = j ? j->done : 0; *total = j ? j->total : 0; *what = j ? j->what : "";
}
void job_result(const PdfJob *j, size_t i, int *pages, long long *bytes, int *width, int *height)
{
    int ok = j && i < j->output_count;
    *pages = ok ? j->out_pages[i] : 0; *bytes = ok ? j->out_bytes[i] : 0;
    *width = ok ? j->out_w[i] : 0; *height = ok ? j->out_h[i] : 0;
}
long long job_source_bytes(const PdfJob *j) { return j ? j->source_bytes : -1; }
void job_free(PdfJob *j)
{
    if (!j) return;
    if (j->ctx) {
        pdf_drop_graft_map(j->ctx, j->map);
        pdf_drop_document(j->ctx, j->src);
        pdf_drop_document(j->ctx, j->dst);
        fz_drop_context(j->ctx);
    }
    remove_temps(j);
    wipe(j->password);
    free_list(j->sources, j->source_count, 0); free_list(j->passwords, j->source_count, 1);
    free_list(j->outputs, j->output_count, 0); free(j->temps);
    free(j->out_pages); free(j->out_w); free(j->out_h); free(j->out_bytes);
    free(j->pages); free(j->groups); free(j);
}
#else
/* Without MuPDF (the Mac) pdftools.c only says the menu cannot be used. */
typedef int pdftools_job_unused;
#endif
