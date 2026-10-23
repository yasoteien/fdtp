// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_IMG_DECODE_H
#define FDTP_IMG_DECODE_H
#include <stddef.h>
/* Decoders return 0 on success, -1 on failure; caller owns RGB pixels.
 * maxw/maxh are a bounding box, never enlarge. Original dimensions include
 * EXIF orientation and legacy pixel aspect. No caller retains file handles. */
typedef struct Img {
    int width,height,original_width,original_height;
    unsigned colors;
    /* GIF: number of frames and the time (seconds since the start) at which
     * the decoded frame ends; INFINITY when it never changes. */
    unsigned frames;
    double frame_end;
    const char *format;
    unsigned char *pixels;
} Img;
void img_size(int w,int h,int maxw,int maxh,int *ow,int *oh);
int img_alloc(Img *img,int w,int h);
void img_free(Img *img);
int img_load(const char *,int,int,Img *,char *,size_t);
int img_modern_load(const char *,int,int,Img *,char *,size_t);
int img_gif_load(const char *,int,int,double,Img *,char *,size_t);
int img_bmp_load(const char *,int,int,Img *,char *,size_t);
int img_legacy_load(const char *,int,int,Img *,char *,size_t);
#endif
