/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file gfx.c
 * @brief Clipped RGB565 drawing.
 */
#include "gfx.h"

#include <string.h>

#include "font8x16.h"

gfx_rect_t gfx_rect(int x, int y, int w, int h)
{
    gfx_rect_t r = { x, y, w, h };
    return r;
}

bool gfx_empty(gfx_rect_t r)
{
    return (0 >= r.w) || (0 >= r.h);
}

gfx_rect_t gfx_intersect(gfx_rect_t a, gfx_rect_t b)
{
    int x0 = a.x;
    int y0 = a.y;
    int x1 = a.x + a.w;
    int y1 = a.y + a.h;
    if ( b.x > x0 )
    {
        x0 = b.x;
    }
    if ( b.y > y0 )
    {
        y0 = b.y;
    }
    if ( b.x + b.w < x1 )
    {
        x1 = b.x + b.w;
    }
    if ( b.y + b.h < y1 )
    {
        y1 = b.y + b.h;
    }
    gfx_rect_t r = { x0, y0, x1 - x0, y1 - y0 };
    if ( gfx_empty(r) )
    {
        r.w = 0;
        r.h = 0;
    }
    return r;
}

gfx_rect_t gfx_union(gfx_rect_t a, gfx_rect_t b)
{
    if ( gfx_empty(a) )
    {
        return b;
    }
    if ( gfx_empty(b) )
    {
        return a;
    }
    int x0 = a.x;
    int y0 = a.y;
    int x1 = a.x + a.w;
    int y1 = a.y + a.h;
    if ( b.x < x0 )
    {
        x0 = b.x;
    }
    if ( b.y < y0 )
    {
        y0 = b.y;
    }
    if ( b.x + b.w > x1 )
    {
        x1 = b.x + b.w;
    }
    if ( b.y + b.h > y1 )
    {
        y1 = b.y + b.h;
    }
    gfx_rect_t r = { x0, y0, x1 - x0, y1 - y0 };
    return r;
}

bool gfx_contains(gfx_rect_t r, int x, int y)
{
    return (x >= r.x) && (y >= r.y) && (x < r.x + r.w) && (y < r.y + r.h);
}

void gfx_surf_init(gfx_surf_t *s, uint16_t *px, int w, int h, int stride)
{
    s->px = px;
    s->w = w;
    s->h = h;
    s->stride = stride;
    s->clip = gfx_rect(0, 0, w, h);
}

void gfx_clip(gfx_surf_t *s, gfx_rect_t r)
{
    s->clip = gfx_intersect(r, gfx_rect(0, 0, s->w, s->h));
}

void gfx_fill(gfx_surf_t *s, gfx_rect_t r, uint16_t c)
{
    r = gfx_intersect(r, s->clip);
    if ( gfx_empty(r) )
    {
        return;
    }
    const uint32_t c2 = ((uint32_t)c << 16) | c;
    for ( int y = r.y; r.y + r.h > y; y++ )
    {
        uint16_t *p = s->px + y * s->stride + r.x;
        int n = r.w;
        if ( ((uintptr_t)p & 2u) && (0 < n) )
        {
            *p++ = c;
            n--;
        }
        uint32_t *q = (uint32_t *)p;
        for ( ; 2 <= n; n -= 2 )
        {
            *q++ = c2;
        }
        if ( n )
        {
            *(uint16_t *)q = c;
        }
    }
}

uint16_t gfx_mix(uint16_t fg, uint16_t bg, uint8_t a)
{
    /* Blend in 5/6/5 at once: spread green into the high half of a 32-bit word. */
    uint32_t f = ((uint32_t)fg | ((uint32_t)fg << 16)) & 0x07E0F81Fu;
    uint32_t b = ((uint32_t)bg | ((uint32_t)bg << 16)) & 0x07E0F81Fu;
    uint32_t al = ((uint32_t)a + 4u) >> 3; /* 0..32 */
    uint32_t m = ((((f - b) * al) >> 5) + b) & 0x07E0F81Fu;
    return (uint16_t)((m & 0xFFFFu) | (m >> 16));
}

void gfx_blend(gfx_surf_t *s, gfx_rect_t r, uint16_t c, uint8_t alpha)
{
    r = gfx_intersect(r, s->clip);
    if ( gfx_empty(r) )
    {
        return;
    }
    for ( int y = r.y; r.y + r.h > y; y++ )
    {
        uint16_t *p = s->px + y * s->stride + r.x;
        for ( int x = 0; r.w > x; x++ )
        {
            p[x] = gfx_mix(c, p[x], alpha);
        }
    }
}

void gfx_frame(gfx_surf_t *s, gfx_rect_t r, uint16_t c)
{
    gfx_fill(s, gfx_rect(r.x, r.y, r.w, 1), c);
    gfx_fill(s, gfx_rect(r.x, r.y + r.h - 1, r.w, 1), c);
    gfx_fill(s, gfx_rect(r.x, r.y, 1, r.h), c);
    gfx_fill(s, gfx_rect(r.x + r.w - 1, r.y, 1, r.h), c);
}

void gfx_round(gfx_surf_t *s, gfx_rect_t r, int rad, uint16_t c)
{
    if ( 2 * rad > r.h )
    {
        rad = r.h / 2;
    }
    for ( int i = 0; rad > i; i++ )
    {
        /* The inset of row i of a quarter circle of radius rad. */
        const int dy = rad - i;
        int cut = 0;
        while ( ((rad - cut) * (rad - cut) + dy * dy) > (rad * rad) )
        {
            cut++;
        }
        gfx_fill(s, gfx_rect(r.x + cut, r.y + i, r.w - 2 * cut, 1), c);
        gfx_fill(s, gfx_rect(r.x + cut, r.y + r.h - 1 - i, r.w - 2 * cut, 1), c);
    }
    gfx_fill(s, gfx_rect(r.x, r.y + rad, r.w, r.h - 2 * rad), c);
}

void gfx_vgrad(gfx_surf_t *s, gfx_rect_t r, uint16_t top, uint16_t bottom)
{
    gfx_rect_t c = gfx_intersect(r, s->clip);
    if ( gfx_empty(c) || (0 >= r.h) )
    {
        return;
    }
    for ( int y = c.y; c.y + c.h > y; y++ )
    {
        const uint8_t a = (uint8_t)(255 - ((y - r.y) * 255) / r.h);
        gfx_fill(s, gfx_rect(c.x, y, c.w, 1), gfx_mix(top, bottom, a));
    }
}

void gfx_disc(gfx_surf_t *s, int cx, int cy, int rad, uint16_t c, uint8_t alpha)
{
    gfx_rect_t box = gfx_intersect(gfx_rect(cx - rad, cy - rad, 2 * rad + 1, 2 * rad + 1), s->clip);
    if ( gfx_empty(box) )
    {
        return;
    }
    const int r2 = rad * rad;
    for ( int y = box.y; box.y + box.h > y; y++ )
    {
        const int dy = y - cy;
        uint16_t *p = s->px + y * s->stride;
        for ( int x = box.x; box.x + box.w > x; x++ )
        {
            const int dx = x - cx;
            if ( (dx * dx + dy * dy) <= r2 )
            {
                if ( 255 == alpha )
                {
                    p[x] = c;
                }
                else
                {
                    p[x] = gfx_mix(c, p[x], alpha);
                }
            }
        }
    }
}

void gfx_copy(gfx_surf_t *s, const uint16_t *src, int src_stride, gfx_rect_t r)
{
    r = gfx_intersect(r, s->clip);
    if ( gfx_empty(r) )
    {
        return;
    }
    for ( int y = r.y; r.y + r.h > y; y++ )
    {
        memcpy(s->px + y * s->stride + r.x, src + y * src_stride + r.x, (size_t)r.w * 2u);
    }
}

static void glyph(gfx_surf_t *s, int x, int y, unsigned char ch, uint16_t c, int scale)
{
    if ( (FONT8X16_FIRST > ch) || (FONT8X16_LAST < ch) )
    {
        ch = '?';
    }
    gfx_rect_t box = gfx_intersect(gfx_rect(x, y, GFX_FONT_W * scale, GFX_FONT_H * scale), s->clip);
    if ( gfx_empty(box) )
    {
        return;
    }
    const uint8_t *g = font8x16[ch - FONT8X16_FIRST];
    for ( int py = box.y; box.y + box.h > py; py++ )
    {
        const uint8_t bits = g[(py - y) / scale];
        if ( !bits )
        {
            continue;
        }
        uint16_t *p = s->px + py * s->stride;
        for ( int px = box.x; box.x + box.w > px; px++ )
        {
            if ( bits & (0x80u >> ((px - x) / scale)) )
            {
                p[px] = c;
            }
        }
    }
}

int gfx_text(gfx_surf_t *s, int x, int y, const char *t, uint16_t c, int scale, int max_w)
{
    const int x0 = x;
    if ( 1 > scale )
    {
        scale = 1;
    }
    for ( ; *t; t++ )
    {
        if ( (0 < max_w) && ((x - x0 + GFX_FONT_W * scale) > max_w) )
        {
            break;
        }
        if ( ' ' != *t )
        {
            glyph(s, x, y, (unsigned char)*t, c, scale);
        }
        x += GFX_FONT_W * scale;
    }
    return x;
}

int gfx_text_w(const char *t, int scale)
{
    return (int)strlen(t) * GFX_FONT_W * scale;
}

static void aglyph(gfx_surf_t *s, int px, int py, const aa_font_t *f, const aa_glyph_t *g, uint16_t c)
{
    const gfx_rect_t box = gfx_intersect(gfx_rect(px + g->x, py + g->y, g->w, g->h), s->clip);
    if ( gfx_empty(box) )
    {
        return;
    }
    const int pitch = (g->w + 1) / 2;
    const uint8_t *bits = f->bits + g->off;
    for ( int y = box.y; box.y + box.h > y; y++ )
    {
        const uint8_t *row = bits + (y - (py + g->y)) * pitch;
        uint16_t *p = s->px + y * s->stride;
        for ( int x = box.x; box.x + box.w > x; x++ )
        {
            const int gx = x - (px + g->x);
            const uint8_t b = row[gx >> 1];
            const uint8_t a = (gx & 1) ? (b & 0x0F) : (b >> 4);
            if ( 15 == a )
            {
                p[x] = c;
            }
            else if ( a )
            {
                p[x] = gfx_mix(c, p[x], (uint8_t)(a * 17));
            }
        }
    }
}

void gfx_icon(gfx_surf_t *s, int x, int y, const aa_icon_t *ic, uint16_t c)
{
    const aa_glyph_t g = { 0, ic->w, ic->w, ic->h, 0, 0 };
    const aa_font_t f = { ic->bits, &g, ic->h, ic->h };
    aglyph(s, x, y, &f, &g, c);
}

int gfx_atext_w(const aa_font_t *f, const char *t)
{
    int w = 0;
    for ( ; *t; t++ )
    {
        unsigned char ch = (unsigned char)*t;
        if ( (32 > ch) || (126 < ch) )
        {
            ch = '?';
        }
        w += f->glyph[ch - 32].adv;
    }
    return w;
}

int gfx_atext(gfx_surf_t *s, int x, int y, const aa_font_t *f, const char *t, uint16_t c, int max_w)
{
    const int x0 = x;
    const int dots = 3 * f->glyph['.' - 32].adv;
    const bool fits = (0 >= max_w) || (gfx_atext_w(f, t) <= max_w);
    for ( ; *t; t++ )
    {
        unsigned char ch = (unsigned char)*t;
        if ( (32 > ch) || (126 < ch) )
        {
            ch = '?';
        }
        const aa_glyph_t *g = &f->glyph[ch - 32];
        if ( !fits && ((x - x0 + g->adv + dots) > max_w) )
        {
            for ( int i = 0; 3 > i; i++ )
            {
                aglyph(s, x, y, f, &f->glyph['.' - 32], c);
                x += f->glyph['.' - 32].adv;
            }
            break;
        }
        aglyph(s, x, y, f, g, c);
        x += g->adv;
    }
    return x;
}

void gfx_pill(gfx_surf_t *s, gfx_rect_t r, uint16_t c)
{
    const int rad = r.h / 2;
    const int r2 = rad * rad;
    for ( int i = 0; r.h > i; i++ )
    {
        /* Half-pixel centre so both ends are symmetric. */
        const int dy2 = (2 * i + 1 - r.h) * (2 * i + 1 - r.h);
        int cut = 0;
        while ( (cut < rad) && ((((2 * (rad - cut) - 1) * (2 * (rad - cut) - 1)) + dy2) > 4 * r2) )
        {
            cut++;
        }
        gfx_fill(s, gfx_rect(r.x + cut, r.y + i, r.w - 2 * cut, 1), c);
    }
}

/* A 12 x 19 arrow: '#' ink, '.' outline. */
static const char *const CURSOR[] = {
    ".",
    "..",
    ".#.",
    ".##.",
    ".###.",
    ".####.",
    ".#####.",
    ".######.",
    ".#######.",
    ".########.",
    ".#########.",
    ".######.....",
    ".###.##.",
    ".##..##.",
    ".#.  .##.",
    "..   .##.",
    ".     .##.",
    "      .##.",
    "       ..",
};

gfx_rect_t gfx_cursor_rect(int x, int y)
{
    return gfx_rect(x, y, 12, 19);
}

void gfx_cursor(gfx_surf_t *s, int x, int y)
{
    for ( int row = 0; 19 > row; row++ )
    {
        const char *l = CURSOR[row];
        for ( int col = 0; l[col]; col++ )
        {
            const int px = x + col;
            const int py = y + row;
            if ( !gfx_contains(s->clip, px, py) )
            {
                continue;
            }
            if ( '#' == l[col] )
            {
                s->px[py * s->stride + px] = 0xFFFF;
            }
            else if ( '.' == l[col] )
            {
                s->px[py * s->stride + px] = 0x0000;
            }
        }
    }
}
