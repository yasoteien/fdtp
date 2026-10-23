// This file is part of FDTP (File & Directory Tool for Pomera).
// SPDX-FileCopyrightText: 2026 yasoteien
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FDTP_SCREEN_H
#define FDTP_SCREEN_H
#include "canvas.h"
#include "cells.h"
#include "keys.h"
typedef struct App App;
typedef struct Screen Screen;
struct Screen {
    void *self;
    int (*key)(Screen *s, App *app, const Key *k);
    void (*draw)(Screen *s, App *app, Canvas *c, Cells *cells);
    int pixel;
    void (*tick)(Screen *s, App *app, double now);
    void (*destroy)(Screen *s);
};
#endif
