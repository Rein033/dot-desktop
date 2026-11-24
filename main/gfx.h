/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file gfx.h
 * @brief Clipped RGB565 drawing: rectangles, frames, gradients, text and the cursor.
 *
 * Pure C: no SDK. Every call draws only inside the surface's clip rectangle, which is how the
 * desktop repaints one dirty region at a time.
 */
#ifndef GFX_H_
#define GFX_H_

#include <stdbool.h>
#include <stdint.h>

#include "fonts_aa.h"
#include "icons_aa.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief An RGB565 colour from 8-bit channels. */
#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3)))

/** @brief A rectangle: x, y, width, height. Empty when w or h is not positive. */
typedef struct {
    int x;
    int y;
    int w;
    int h;
} gfx_rect_t;

/** @brief A surface: pixels, size, row stride in pixels and the clip rectangle. */
typedef struct {
    uint16_t  *px;
    int        w;
    int        h;
    int        stride;
    gfx_rect_t clip;
} gfx_surf_t;

#define GFX_FONT_W 8
#define GFX_FONT_H 16

/** @brief A rectangle from four numbers. */
gfx_rect_t gfx_rect(int x, int y, int w, int h);

/** @brief The overlap of @p a and @p b (empty when none). */
gfx_rect_t gfx_intersect(gfx_rect_t a, gfx_rect_t b);

/** @brief The smallest rectangle holding both (an empty one is ignored). */
gfx_rect_t gfx_union(gfx_rect_t a, gfx_rect_t b);

/** @brief Whether a rectangle is empty. */
bool gfx_empty(gfx_rect_t r);

/** @brief Whether point (x, y) is inside @p r. */
bool gfx_contains(gfx_rect_t r, int x, int y);

/** @brief Make a surface over @p px with the clip set to the whole of it. */
void gfx_surf_init(gfx_surf_t *s, uint16_t *px, int w, int h, int stride);

/** @brief Set the clip, kept inside the surface. */
void gfx_clip(gfx_surf_t *s, gfx_rect_t r);

/** @brief Fill a rectangle. */
void gfx_fill(gfx_surf_t *s, gfx_rect_t r, uint16_t c);

/** @brief Blend a rectangle over what is there; @p alpha 0..255. */
void gfx_blend(gfx_surf_t *s, gfx_rect_t r, uint16_t c, uint8_t alpha);

/** @brief A one-pixel frame. */
void gfx_frame(gfx_surf_t *s, gfx_rect_t r, uint16_t c);

/** @brief A filled rectangle with corners cut by @p rad pixels. */
void gfx_round(gfx_surf_t *s, gfx_rect_t r, int rad, uint16_t c);

/** @brief A vertical gradient from @p top to @p bottom. */
void gfx_vgrad(gfx_surf_t *s, gfx_rect_t r, uint16_t top, uint16_t bottom);

/** @brief A filled circle. */
void gfx_disc(gfx_surf_t *s, int cx, int cy, int rad, uint16_t c, uint8_t alpha);

/** @brief Copy the same rectangle from @p src (same size as @p s) inside the clip. */
void gfx_copy(gfx_surf_t *s, const uint16_t *src, int src_stride, gfx_rect_t r);

/**
 * @brief Draw text with its top-left at (x, y); stops at @p max_w pixels when positive.
 * @param scale Pixels per font pixel, 1 or more.
 * @return The x after the last letter drawn.
 */
int gfx_text(gfx_surf_t *s, int x, int y, const char *t, uint16_t c, int scale, int max_w);

/** @brief Width in pixels of @p t at @p scale. */
int gfx_text_w(const char *t, int scale);

/**
 * @brief Draw anti-aliased text with the top of its line at (x, y); stops at @p max_w when positive,
 *        ending in "..." when it had to stop.
 * @return The x after the last letter drawn.
 */
int gfx_atext(gfx_surf_t *s, int x, int y, const aa_font_t *f, const char *t, uint16_t c, int max_w);

/** @brief Width in pixels of @p t in @p f. */
int gfx_atext_w(const aa_font_t *f, const char *t);

/** @brief An icon inked in @p c with its top-left at (x, y). */
void gfx_icon(gfx_surf_t *s, int x, int y, const aa_icon_t *ic, uint16_t c);

/** @brief A pill: a rectangle with fully round ends. */
void gfx_pill(gfx_surf_t *s, gfx_rect_t r, uint16_t c);

/** @brief The arrow cursor, its tip at (x, y). */
void gfx_cursor(gfx_surf_t *s, int x, int y);

/** @brief The cursor's bounding box, tip at (x, y). */
gfx_rect_t gfx_cursor_rect(int x, int y);

/** @brief Mix two colours; @p a is the weight of @p fg, 0..255. */
uint16_t gfx_mix(uint16_t fg, uint16_t bg, uint8_t a);

#ifdef __cplusplus
}
#endif

#endif /* GFX_H_ */
