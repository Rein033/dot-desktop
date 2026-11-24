/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file desk.c
 * @brief The desktop: layout, input and painting.
 *
 * Painting is by region: whatever changes adds its rectangle to a short list, and desk_render()
 * repaints each listed rectangle from the bottom up (wallpaper, icons, windows in z-order,
 * taskbar, cursor) with the clip set to it. The outputs then send only those rectangles.
 */
#include "desk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- look: kodeOS's palette (kode_colors.h) and its pills ------------------------ */

#define C_PAGE     RGB(0x00, 0x00, 0x00) /**< Black: the AMOLED page */
#define C_WIN      RGB(0x19, 0x19, 0x19) /**< Soft Black: a window */
#define C_ROW      RGB(0x21, 0x21, 0x21) /**< Dark Grey: a row, a field */
#define C_CTRL     RGB(0x2A, 0x2A, 0x2A) /**< Charcoal: a control at rest */
#define C_TEXT     RGB(0xDE, 0xD7, 0xCD) /**< Light Grey: text */
#define C_DIM      RGB(0xC2, 0xBA, 0xB3) /**< Grey: secondary text */
#define C_MUTE     RGB(0x5C, 0x5A, 0x56) /**< Warm Grey: tracks, what is off */
#define C_YELLOW   RGB(0xFF, 0xAC, 0x00) /**< Bold Yellow: what is on, the focus */
#define C_BLUE     RGB(0x02, 0x8A, 0xC2) /**< Kody Blue */
#define C_INK      RGB(0x19, 0x19, 0x19) /**< text on yellow or light grey */
#define C_GOOD     RGB(0x31, 0xC4, 0x5A)
#define C_CHARGE   RGB(0x67, 0xEA, 0x94)
#define C_BAD      RGB(0xC9, 0x30, 0x3D)

#define BAR_H     46
#define TITLE_H   44
#define ROW_H     28
#define ICON_CELL 96
#define ICON_SZ   60
#define WIN_RAD   16
#define DIRTY_MAX 24
#define FILES_MAX 160

enum {
    W_FILES = 0,
    W_SYSTEM,
    W_NOTES,
    W_DISPLAY,
    W_COUNT
};

typedef struct {
    const char *title;
    gfx_rect_t  r;
    bool        open;
} win_t;

typedef struct {
    char     name[DESK_NAME_MAX];
    uint32_t size;
    bool     dir;
} entry_t;

typedef struct {
    int           w;
    int           h;
    uint16_t     *wall;
    win_t         win[W_COUNT];
    int           z[W_COUNT];      /**< window ids, bottom first */
    int           nz;
    int           cx;
    int           cy;
    bool          down;
    int           drag;            /**< window being dragged, -1 none */
    int           drag_dx;
    int           drag_dy;
    gfx_rect_t    dirty[DIRTY_MAX];
    int           ndirty;
    desk_status_t st;
    int           hh;
    int           mm;
    int           ss;
    char          path[DESK_PATH_MAX];
    entry_t      *files;
    int           nfiles;
    int           files_scroll;
    bool          files_loading;
    bool          files_failed;
    char         *notes;
    size_t        nnotes;
    char          notes_msg[24];
    desk_act_t    act[8];
    int           nact;
} desk_t;

static desk_t d;

/* ---- the dirty list ---------------------------------------------------------- */

static void dirty(gfx_rect_t r)
{
    r = gfx_intersect(r, gfx_rect(0, 0, d.w, d.h));
    if ( gfx_empty(r) )
    {
        return;
    }
    for ( int i = 0; d.ndirty > i; i++ )
    {
        /* Inside one already: nothing to add. Overlapping: grow that one. */
        gfx_rect_t u = gfx_union(d.dirty[i], r);
        if ( !gfx_empty(gfx_intersect(d.dirty[i], r)) )
        {
            d.dirty[i] = u;
            return;
        }
    }
    if ( DIRTY_MAX == d.ndirty )
    {
        for ( int i = 1; d.ndirty > i; i++ )
        {
            d.dirty[0] = gfx_union(d.dirty[0], d.dirty[i]);
        }
        d.ndirty = 1;
        d.dirty[0] = gfx_union(d.dirty[0], r);
        return;
    }
    d.dirty[d.ndirty++] = r;
}

void desk_invalidate(void)
{
    d.ndirty = 0;
    dirty(gfx_rect(0, 0, d.w, d.h));
}

bool desk_dirty(void)
{
    return 0 < d.ndirty;
}

static void act(desk_act_kind_t k, int v)
{
    if ( 8 > d.nact )
    {
        d.act[d.nact].kind = k;
        d.act[d.nact].value = v;
        d.nact++;
    }
}

bool desk_take_action(desk_act_t *a)
{
    if ( 0 == d.nact )
    {
        return false;
    }
    *a = d.act[0];
    memmove(&d.act[0], &d.act[1], sizeof(d.act[0]) * (size_t)(d.nact - 1));
    d.nact--;
    return true;
}

/* ---- layout ----------------------------------------------------------------- */

static gfx_rect_t bar_rect(void)
{
    return gfx_rect(0, d.h - BAR_H, d.w, BAR_H);
}

static gfx_rect_t icon_rect(int i)
{
    int per_col = (d.h - BAR_H - 12) / ICON_CELL;
    if ( 1 > per_col )
    {
        per_col = 1;
    }
    return gfx_rect(12 + (i / per_col) * ICON_CELL, 12 + (i % per_col) * ICON_CELL, ICON_CELL, ICON_CELL);
}

static gfx_rect_t content(int id)
{
    gfx_rect_t r = d.win[id].r;
    return gfx_rect(r.x + 1, r.y + TITLE_H, r.w - 2, r.h - TITLE_H - 1);
}

static gfx_rect_t close_box(int id)
{
    gfx_rect_t r = d.win[id].r;
    return gfx_rect(r.x + r.w - TITLE_H, r.y, TITLE_H, TITLE_H);
}

static int clampi(int v, int lo, int hi)
{
    if ( v < lo )
    {
        return lo;
    }
    if ( v > hi )
    {
        return hi;
    }
    return v;
}

static void layout(void)
{
    const int area_h = d.h - BAR_H;
    const bool small = (640 > d.w);
    int ww = small ? d.w - 100 : d.w * 36 / 100;
    int wh = small ? area_h - 30 : area_h * 55 / 100;
    ww = clampi(ww, 240, 560);
    wh = clampi(wh, 160, 420);
    if ( ww > d.w - 8 )
    {
        ww = d.w - 8;
    }
    if ( wh > area_h - 8 )
    {
        wh = area_h - 8;
    }
    const int x0 = small ? 90 : 120;
    d.win[W_FILES].title = "Files";
    d.win[W_FILES].r = gfx_rect(x0, 30, ww, wh);
    d.win[W_SYSTEM].title = "System";
    d.win[W_SYSTEM].r = gfx_rect(d.w - ww - 40, small ? 40 : 24 + font_big.line_h + 16, ww, wh);
    d.win[W_NOTES].title = "Notes";
    d.win[W_NOTES].r = gfx_rect(x0 + 60, area_h - wh - 20, ww, wh);
    d.win[W_DISPLAY].title = "Display";
    int dw = (400 > ww) ? 400 : ww;
    int dh = 230;
    if ( dw > d.w - 8 )
    {
        dw = d.w - 8;
    }
    if ( dh > area_h - 8 )
    {
        dh = area_h - 8;
    }
    d.win[W_DISPLAY].r = gfx_rect(d.w - dw - 40, area_h - dh - 10, dw, dh);
    for ( int i = 0; W_COUNT > i; i++ )
    {
        gfx_rect_t *r = &d.win[i].r;
        r->x = clampi(r->x, 0, d.w - r->w);
        r->y = clampi(r->y, 0, area_h - r->h);
    }
    d.cx = clampi(d.cx, 0, d.w - 1);
    d.cy = clampi(d.cy, 0, d.h - 1);
}

static void make_wallpaper(void)
{
    gfx_surf_t s;
    gfx_surf_init(&s, d.wall, d.w, d.h, d.w);
    gfx_fill(&s, gfx_rect(0, 0, d.w, d.h), C_PAGE);
    /* One big Kody-blue shape rising from the bottom right, as kodeOS's pages have. Abstract on
     * purpose: no character. */
    const int r = d.h * 55 / 100;
    gfx_disc(&s, d.w - r * 6 / 10, d.h + r / 5, r, C_BLUE, 255);
}

bool desk_init(int w, int h)
{
    memset(&d, 0, sizeof(d));
    d.files = calloc(FILES_MAX, sizeof(entry_t));
    d.notes = calloc(DESK_NOTES_MAX + 1, 1);
    if ( !d.files || !d.notes )
    {
        return false;
    }
    strcpy(d.path, "/sdcard");
    d.drag = -1;
    d.hh = -1;
    d.nz = 0;
    for ( int i = 0; W_COUNT > i; i++ )
    {
        d.z[d.nz++] = i;
    }
    d.win[W_FILES].open = true;
    d.win[W_SYSTEM].open = true;
    d.win[W_NOTES].open = true;
    d.win[W_DISPLAY].open = false;
    d.cx = w / 2;
    d.cy = h / 2;
    act(DESK_ACT_LIST, 0);
    d.files_loading = true;
    return desk_resize(w, h);
}

bool desk_resize(int w, int h)
{
    if ( (w != d.w) || (h != d.h) || !d.wall )
    {
        free(d.wall);
        d.wall = malloc((size_t)w * h * 2u);
        if ( !d.wall )
        {
            return false;
        }
        d.w = w;
        d.h = h;
        make_wallpaper();
        layout();
    }
    desk_invalidate();
    return true;
}

/* ---- painting -------------------------------------------------------------- */

/** @brief A pill with a centred label: Charcoal at rest, Yellow when on. */
static void button(gfx_surf_t *s, gfx_rect_t r, const char *label, bool on)
{
    gfx_pill(s, r, on ? C_YELLOW : C_CTRL);
    const aa_font_t *f = &font_bold;
    const int tw = gfx_atext_w(f, label);
    int tx = r.x + (r.w - tw) / 2;
    if ( tx < r.x + 8 )
    {
        tx = r.x + 8;
    }
    gfx_atext(s, tx, r.y + (r.h - f->line_h) / 2, f, label, on ? C_INK : C_TEXT, r.w - 16);
}

/** @brief The launcher's look: a disc with two letters, as kodeOS shows an app. */
static void icon_art(gfx_surf_t *s, int id, gfx_rect_t r)
{
    static const uint16_t BG[W_COUNT] = {
        C_BLUE, C_TEXT, C_YELLOW, C_DIM,
    };
    static const char *const MONO[W_COUNT] = { "FI", "SY", "NO", "DI" };
    const int rad = r.w / 2;
    gfx_disc(s, r.x + rad, r.y + rad, rad, BG[id], 255);
    const uint16_t ink = (W_FILES == id) ? C_TEXT : C_INK;
    const int tw = gfx_atext_w(&font_bold, MONO[id]);
    gfx_atext(s, r.x + rad - tw / 2, r.y + rad - font_bold.line_h / 2, &font_bold, MONO[id], ink, 0);
}

static void paint_icons(gfx_surf_t *s)
{
    for ( int i = 0; W_COUNT > i; i++ )
    {
        const gfx_rect_t c = icon_rect(i);
        if ( gfx_empty(gfx_intersect(c, s->clip)) )
        {
            continue;
        }
        icon_art(s, i, gfx_rect(c.x + (ICON_CELL - ICON_SZ) / 2, c.y + 2, ICON_SZ, ICON_SZ));
        const char *t = d.win[i].title;
        gfx_atext(s, c.x + (ICON_CELL - gfx_atext_w(&font_ui, t)) / 2, c.y + ICON_SZ + 6, &font_ui, t, C_TEXT, 0);
    }
}

/** @brief The big clock on the page, as kodeOS's face shows it. */
static gfx_rect_t big_clock_rect(void)
{
    const int w = gfx_atext_w(&font_big, "00:00") + 8;
    return gfx_rect(d.w - w - 40, 24, w, font_big.line_h);
}

static void paint_big_clock(gfx_surf_t *s)
{
    const gfx_rect_t r = big_clock_rect();
    if ( gfx_empty(gfx_intersect(r, s->clip)) || (0 > d.hh) || (640 > d.w) )
    {
        return;
    }
    char t[8];
    snprintf(t, sizeof(t), "%02d:%02d", clampi(d.hh, 0, 23), clampi(d.mm, 0, 59));
    gfx_atext(s, r.x + r.w - 4 - gfx_atext_w(&font_big, t), r.y, &font_big, t, C_TEXT, 0);
}

static void human_size(char *buf, size_t cap, uint32_t n)
{
    if ( 1024u > n )
    {
        snprintf(buf, cap, "%u B", (unsigned)n);
    }
    else if ( 1024u * 1024u > n )
    {
        snprintf(buf, cap, "%u KB", (unsigned)(n / 1024u));
    }
    else
    {
        snprintf(buf, cap, "%u.%u MB", (unsigned)(n / 1048576u), (unsigned)((n % 1048576u) * 10u / 1048576u));
    }
}

static int files_rows(void)
{
    return (content(W_FILES).h - 8) / ROW_H;
}

/** @brief The file list as rows: ".." first below the card's root, then the entries. */
static int files_virtual(void)
{
    return d.nfiles + ((0 == strcmp(d.path, "/sdcard")) ? 0 : 1);
}

/** @brief The entry of virtual row @p v; -1 for "..". */
static int files_entry_of(int v)
{
    if ( 0 == strcmp(d.path, "/sdcard") )
    {
        return v;
    }
    return v - 1;
}

static void paint_files(gfx_surf_t *s, gfx_rect_t c)
{
    int y = c.y + 4;
    const int ty = (ROW_H - font_ui.line_h) / 2;
    if ( d.files_failed )
    {
        gfx_atext(s, c.x + 16, y + ty, &font_ui, "No card, or it will not read.", C_BAD, c.w - 32);
        return;
    }
    const int rows = files_rows();
    int shown = 0;
    for ( int v = d.files_scroll; (files_virtual() > v) && (rows > shown); v++ )
    {
        const int i = files_entry_of(v);
        gfx_pill(s, gfx_rect(c.x + 8, y + 1, c.w - 16, ROW_H - 2), C_ROW);
        if ( 0 > i )
        {
            gfx_atext(s, c.x + 44, y + ty, &font_ui, "..", C_DIM, 0);
        }
        else
        {
            const entry_t *e = &d.files[i];
            gfx_disc(s, c.x + 26, y + ROW_H / 2, 6, e->dir ? C_BLUE : C_MUTE, 255);
            gfx_atext(s, c.x + 44, y + ty, &font_ui, e->name, C_TEXT, c.w - 150);
            if ( !e->dir )
            {
                char sz[16];
                human_size(sz, sizeof(sz), e->size);
                gfx_atext(s, c.x + c.w - 22 - gfx_atext_w(&font_ui, sz), y + ty, &font_ui, sz, C_DIM, 0);
            }
        }
        y += ROW_H;
        shown++;
    }
    if ( rows > shown )
    {
        if ( d.files_loading )
        {
            gfx_atext(s, c.x + 16, y + ty, &font_ui, "Reading...", C_DIM, 0);
        }
        else if ( 0 == d.nfiles )
        {
            gfx_atext(s, c.x + 16, y + ty, &font_ui, "Empty", C_DIM, 0);
        }
    }
}

static void line(gfx_surf_t *s, gfx_rect_t c, int *y, const char *k, const char *v, uint16_t vc)
{
    gfx_atext(s, c.x + 16, *y, &font_ui, k, C_DIM, 0);
    gfx_atext(s, c.x + 136, *y, &font_ui, v, vc, c.w - 150);
    *y += 24;
}

static const char *const RES_NAME[DESK_RES_COUNT] = { "1280x720", "1024x576", "800x480" };

int desk_res_w(int res)
{
    static const int W[DESK_RES_COUNT] = { 1280, 1024, 800 };
    return W[clampi(res, 0, DESK_RES_COUNT - 1)];
}

int desk_res_h(int res)
{
    static const int H[DESK_RES_COUNT] = { 720, 576, 480 };
    return H[clampi(res, 0, DESK_RES_COUNT - 1)];
}

static void paint_system(gfx_surf_t *s, gfx_rect_t c)
{
    char b[64];
    int y = c.y + 6;
    line(s, c, &y, "Chip", "ESP32-P4, radio ESP32-C5", C_TEXT);
    snprintf(b, sizeof(b), "%uh %02um %02us", (unsigned)(d.st.uptime_s / 3600u), (unsigned)(d.st.uptime_s / 60u % 60u),
             (unsigned)(d.st.uptime_s % 60u));
    line(s, c, &y, "Uptime", b, C_TEXT);
    snprintf(b, sizeof(b), "%u KB internal, %u KB PSRAM", (unsigned)d.st.heap_int_kb, (unsigned)d.st.heap_psram_kb);
    line(s, c, &y, "Free", b, C_TEXT);
    if ( d.st.wifi_up )
    {
        snprintf(b, sizeof(b), "%s, %d dBm", d.st.ssid, d.st.rssi);
        line(s, c, &y, "Wi-Fi", b, C_GOOD);
        snprintf(b, sizeof(b), "http://%s/", d.st.ip);
        line(s, c, &y, "Stream", b, C_YELLOW);
    }
    else
    {
        line(s, c, &y, "Wi-Fi", "Not connected", C_BAD);
        y += 24;
    }
    if ( d.st.batt_known )
    {
        snprintf(b, sizeof(b), "%d%%, %d mV%s", d.st.batt_pct, d.st.vbat_mv, d.st.charging ? ", charging" : "");
        line(s, c, &y, "Battery", b, d.st.charging ? C_CHARGE : C_TEXT);
    }
    else
    {
        line(s, c, &y, "Battery", "-", C_DIM);
    }
    snprintf(b, sizeof(b), "%d, %d fps, %d kB/s", d.st.clients, d.st.fps, d.st.kbps);
    line(s, c, &y, "Viewers", b, C_TEXT);
    snprintf(b, sizeof(b), "%s JPEG, quality %d", d.st.hw_jpeg ? "Hardware" : "Software", d.st.quality);
    line(s, c, &y, "Encoder", b, C_TEXT);
    snprintf(b, sizeof(b), "%dx%d", d.w, d.h);
    line(s, c, &y, "Desktop", b, C_TEXT);
}

/** @brief The notes' toolbar buttons. */
static gfx_rect_t notes_btn(int i)
{
    const gfx_rect_t c = content(W_NOTES);
    return gfx_rect(c.x + 12 + i * 96, c.y + 4, 88, 30);
}

static void paint_notes(gfx_surf_t *s, gfx_rect_t c)
{
    button(s, notes_btn(0), "Save", false);
    button(s, notes_btn(1), "Clear", false);
    gfx_atext(s, c.x + 212, c.y + 10, &font_ui, d.notes_msg, C_DIM, c.w - 220);
    const gfx_rect_t page = gfx_rect(c.x + 10, c.y + 42, c.w - 20, c.h - 52);
    gfx_round(s, page, 10, C_ROW);
    const int cols = (page.w - 20) / GFX_FONT_W;
    const int rows = (page.h - 12) / (GFX_FONT_H + 2);
    if ( (1 > cols) || (1 > rows) )
    {
        return;
    }
    /* Lay the text out once to find the first row that keeps the end in sight. */
    int total = 1;
    int col = 0;
    for ( size_t i = 0; d.nnotes > i; i++ )
    {
        if ( ('\n' == d.notes[i]) || (cols == col) )
        {
            total++;
            col = 0;
            if ( '\n' == d.notes[i] )
            {
                continue;
            }
        }
        col++;
    }
    const int skip = (total > rows) ? total - rows : 0;
    int row = 0;
    col = 0;
    char ch[2] = { 0, 0 };
    const int x0 = page.x + 10;
    const int y0 = page.y + 6;
    for ( size_t i = 0; d.nnotes > i; i++ )
    {
        if ( ('\n' == d.notes[i]) || (cols == col) )
        {
            row++;
            col = 0;
            if ( '\n' == d.notes[i] )
            {
                continue;
            }
        }
        if ( row >= skip )
        {
            ch[0] = d.notes[i];
            gfx_text(s, x0 + col * GFX_FONT_W, y0 + (row - skip) * (GFX_FONT_H + 2), ch, C_TEXT, 1, 0);
        }
        col++;
    }
    if ( cols == col )
    {
        row++;
        col = 0;
    }
    gfx_fill(s, gfx_rect(x0 + col * GFX_FONT_W, y0 + (row - skip) * (GFX_FONT_H + 2) + 1, 2, GFX_FONT_H), C_YELLOW);
}

/** @brief The display window's buttons: 0..2 size, 3 quality down, 4 up. */
static gfx_rect_t disp_btn(int i)
{
    const gfx_rect_t c = content(W_DISPLAY);
    if ( 3 > i )
    {
        const int bw = (c.w - 32 - 16) / 3;
        return gfx_rect(c.x + 16 + i * (bw + 8), c.y + 32, bw, 32);
    }
    return gfx_rect(c.x + 16 + (i - 3) * 120, c.y + 104, 48, 32);
}

static void paint_display(gfx_surf_t *s, gfx_rect_t c)
{
    gfx_atext(s, c.x + 16, c.y + 6, &font_ui, "Desktop size", C_DIM, 0);
    for ( int i = 0; DESK_RES_COUNT > i; i++ )
    {
        button(s, disp_btn(i), RES_NAME[i], d.st.res == i);
    }
    gfx_atext(s, c.x + 16, c.y + 78, &font_ui, "Stream quality", C_DIM, 0);
    button(s, disp_btn(3), "-", false);
    button(s, disp_btn(4), "+", false);
    char b[8];
    snprintf(b, sizeof(b), "%d", d.st.quality);
    gfx_atext(s, c.x + 16 + 84 - gfx_atext_w(&font_bold, b) / 2, c.y + 110, &font_bold, b, C_TEXT, 0);
    gfx_atext(s, c.x + 16, c.y + 156, &font_ui, "Smaller and lower: faster on a slow network.", C_DIM, c.w - 32);
}

static void paint_window(gfx_surf_t *s, int id, bool top)
{
    const win_t *w = &d.win[id];
    if ( gfx_empty(gfx_intersect(gfx_rect(w->r.x, w->r.y, w->r.w + 2, w->r.h + 2), s->clip)) )
    {
        return;
    }
    /* The focused window has a yellow edge; the others a charcoal one. */
    gfx_round(s, w->r, WIN_RAD, top ? C_YELLOW : C_CTRL);
    gfx_round(s, gfx_rect(w->r.x + 2, w->r.y + 2, w->r.w - 4, w->r.h - 4), WIN_RAD - 2, C_WIN);
    char title[DESK_PATH_MAX + 16];
    if ( W_FILES == id )
    {
        snprintf(title, sizeof(title), "Files  %s", d.path);
    }
    else
    {
        snprintf(title, sizeof(title), "%s", w->title);
    }
    /* The title as kodeOS writes a page's: bold, Light Grey; in a yellow pill when focused. */
    const aa_font_t *f = &font_bold;
    const int ty = w->r.y + (TITLE_H - f->line_h) / 2 + 2;
    if ( top )
    {
        int tw = gfx_atext_w(f, title) + 28;
        if ( tw > w->r.w - 70 )
        {
            tw = w->r.w - 70;
        }
        gfx_pill(s, gfx_rect(w->r.x + 10, w->r.y + 8, tw, TITLE_H - 12), C_YELLOW);
        gfx_atext(s, w->r.x + 24, ty, f, title, C_INK, tw - 28);
    }
    else
    {
        gfx_atext(s, w->r.x + 24, ty, f, title, C_TEXT, w->r.w - 80);
    }
    const gfx_rect_t cb = close_box(id);
    gfx_disc(s, cb.x + cb.w / 2, cb.y + cb.h / 2 + 2, 12, C_CTRL, 255);
    gfx_atext(s, cb.x + cb.w / 2 - gfx_atext_w(&font_ui, "x") / 2, cb.y + cb.h / 2 + 2 - font_ui.line_h / 2 - 1,
              &font_ui, "x", C_TEXT, 0);
    const gfx_rect_t c = content(id);
    const gfx_rect_t keep = s->clip;
    gfx_clip(s, gfx_intersect(keep, c));
    if ( !gfx_empty(s->clip) )
    {
        switch ( id )
        {
        case W_FILES:
            paint_files(s, c);
            break;
        case W_SYSTEM:
            paint_system(s, c);
            break;
        case W_NOTES:
            paint_notes(s, c);
            break;
        default:
            paint_display(s, c);
            break;
        }
    }
    s->clip = keep;
}

static int top_open(void)
{
    for ( int i = d.nz - 1; 0 <= i; i-- )
    {
        if ( d.win[d.z[i]].open )
        {
            return d.z[i];
        }
    }
    return -1;
}

static bool show_ip(void)
{
    return 1000 <= d.w;
}

static gfx_rect_t clock_rect(void)
{
    return gfx_rect(d.w - 76, d.h - BAR_H, 76, BAR_H);
}

/** @brief Wi-Fi bars, the address when there is room, and the battery. */
static gfx_rect_t status_rect(void)
{
    const int w = 28 + (show_ip() ? 140 : 0) + 84;
    return gfx_rect(d.w - 76 - w, d.h - BAR_H, w, BAR_H);
}

static gfx_rect_t task_btn(int id)
{
    const int x0 = 158;
    int bw = (status_rect().x - x0 - 8) / W_COUNT - 8;
    if ( 112 < bw )
    {
        bw = 112;
    }
    return gfx_rect(x0 + id * (bw + 8), d.h - BAR_H + 8, bw, BAR_H - 16);
}

static bool show_tasks(void)
{
    return 64 <= task_btn(0).w;
}

static void paint_bar(gfx_surf_t *s)
{
    const gfx_rect_t bar = bar_rect();
    if ( gfx_empty(gfx_intersect(bar, s->clip)) )
    {
        return;
    }
    gfx_fill(s, bar, C_PAGE);
    const int ty = bar.y + (BAR_H - font_bold.line_h) / 2;
    gfx_disc(s, 22, bar.y + BAR_H / 2, 8, C_YELLOW, 255);
    gfx_atext(s, 38, ty, &font_bold, "Dot Desktop", C_TEXT, 0);
    const int top = top_open();
    for ( int i = 0; (W_COUNT > i) && show_tasks(); i++ )
    {
        if ( d.win[i].open )
        {
            button(s, task_btn(i), d.win[i].title, top == i);
        }
    }
    /* Wi-Fi: four bars and the address. */
    int x = status_rect().x;
    int bars = 0;
    if ( d.st.wifi_up )
    {
        bars = (-55 <= d.st.rssi) ? 4 : (-65 <= d.st.rssi) ? 3 : (-75 <= d.st.rssi) ? 2 : 1;
    }
    for ( int i = 0; 4 > i; i++ )
    {
        gfx_round(s, gfx_rect(x + i * 6, bar.y + 30 - i * 4 - 6, 4, i * 4 + 6), 1, (i < bars) ? C_TEXT : C_MUTE);
    }
    x += 28;
    const int tyu = bar.y + (BAR_H - font_ui.line_h) / 2;
    if ( show_ip() )
    {
        gfx_atext(s, x, tyu, &font_ui, d.st.wifi_up ? d.st.ip : "No Wi-Fi", d.st.wifi_up ? C_TEXT : C_DIM, 134);
        x += 140;
    }
    /* The battery, as a small pill. */
    const gfx_rect_t cell = gfx_rect(x, bar.y + 15, 30, 16);
    gfx_pill(s, cell, C_MUTE);
    if ( d.st.batt_known )
    {
        const int pct = clampi(d.st.batt_pct, 0, 100);
        const int fill = 16 + (14 * pct) / 100;
        uint16_t c = C_TEXT;
        if ( d.st.charging )
        {
            c = C_CHARGE;
        }
        else if ( 20 >= pct )
        {
            c = C_BAD;
        }
        gfx_pill(s, gfx_rect(cell.x, cell.y, fill, 16), c);
        char b[8];
        snprintf(b, sizeof(b), "%d%%", pct);
        gfx_atext(s, x + 36, tyu, &font_ui, b, C_TEXT, 0);
    }
    /* The clock. */
    char t[8];
    if ( 0 <= d.hh )
    {
        snprintf(t, sizeof(t), "%02d:%02d", clampi(d.hh, 0, 23), clampi(d.mm, 0, 59));
    }
    else
    {
        snprintf(t, sizeof(t), "--:--");
    }
    const gfx_rect_t cr = clock_rect();
    gfx_atext(s, cr.x + cr.w - 16 - gfx_atext_w(&font_bold, t), ty, &font_bold, t, C_TEXT, 0);
}

static void paint(gfx_surf_t *s)
{
    gfx_copy(s, d.wall, d.w, s->clip);
    paint_big_clock(s);
    paint_icons(s);
    const int top = top_open();
    for ( int i = 0; d.nz > i; i++ )
    {
        const int id = d.z[i];
        if ( d.win[id].open )
        {
            paint_window(s, id, id == top);
        }
    }
    paint_bar(s);
    gfx_cursor(s, d.cx, d.cy);
}

bool desk_render(uint16_t *fb, desk_mark_fn mark)
{
    if ( (0 == d.ndirty) || !fb )
    {
        return false;
    }
    gfx_surf_t s;
    gfx_surf_init(&s, fb, d.w, d.h, d.w);
    for ( int i = 0; d.ndirty > i; i++ )
    {
        gfx_clip(&s, d.dirty[i]);
        paint(&s);
        if ( mark )
        {
            mark(d.dirty[i]);
        }
    }
    d.ndirty = 0;
    return true;
}


/* ---- status ----------------------------------------------------------------- */

static void dirty_content(int id)
{
    if ( d.win[id].open )
    {
        dirty(content(id));
    }
}

void desk_status(const desk_status_t *st)
{
    const desk_status_t old = d.st;
    d.st = *st;
    if ( (old.wifi_up != st->wifi_up) || strcmp(old.ip, st->ip) || (old.rssi / 5 != st->rssi / 5)
         || (old.batt_pct != st->batt_pct) || (old.charging != st->charging) || (old.batt_known != st->batt_known) )
    {
        dirty(status_rect());
    }
    if ( memcmp(&old, st, sizeof(old)) )
    {
        dirty_content(W_SYSTEM);
    }
    if ( (old.res != st->res) || (old.quality != st->quality) )
    {
        dirty_content(W_DISPLAY);
    }
}

void desk_clock(int hh, int mm, int ss)
{
    d.ss = ss;
    if ( (hh != d.hh) || (mm != d.mm) )
    {
        d.hh = hh;
        d.mm = mm;
        dirty(clock_rect());
        dirty(big_clock_rect());
    }
}

/* ---- input ------------------------------------------------------------------ */

static void move_cursor(int x, int y)
{
    x = clampi(x, 0, d.w - 1);
    y = clampi(y, 0, d.h - 1);
    if ( (x == d.cx) && (y == d.cy) )
    {
        return;
    }
    dirty(gfx_cursor_rect(d.cx, d.cy));
    d.cx = x;
    d.cy = y;
    dirty(gfx_cursor_rect(d.cx, d.cy));
    if ( 0 <= d.drag )
    {
        win_t *w = &d.win[d.drag];
        const gfx_rect_t was = w->r;
        w->r.x = clampi(x - d.drag_dx, -w->r.w + 60, d.w - 60);
        w->r.y = clampi(y - d.drag_dy, 0, d.h - BAR_H - TITLE_H);
        dirty(gfx_rect(was.x, was.y, was.w + 4, was.h + 4));
        dirty(gfx_rect(w->r.x, w->r.y, w->r.w + 4, w->r.h + 4));
    }
}

void desk_pointer_abs(int x, int y)
{
    move_cursor(x, y);
}

void desk_pointer_rel(int dx, int dy)
{
    move_cursor(d.cx + dx, d.cy + dy);
}

void desk_pointer(int *x, int *y)
{
    *x = d.cx;
    *y = d.cy;
}

static void raise(int id)
{
    int at = -1;
    for ( int i = 0; d.nz > i; i++ )
    {
        if ( d.z[i] == id )
        {
            at = i;
        }
    }
    const int old_top = top_open();
    if ( 0 <= at )
    {
        memmove(&d.z[at], &d.z[at + 1], sizeof(int) * (size_t)(d.nz - at - 1));
        d.z[d.nz - 1] = id;
    }
    d.win[id].open = true;
    if ( old_top != id )
    {
        if ( 0 <= old_top )
        {
            dirty(gfx_rect(d.win[old_top].r.x, d.win[old_top].r.y, d.win[old_top].r.w + 4, d.win[old_top].r.h + 4));
        }
        dirty(gfx_rect(d.win[id].r.x, d.win[id].r.y, d.win[id].r.w + 4, d.win[id].r.h + 4));
        dirty(bar_rect());
    }
}

static void close_win(int id)
{
    d.win[id].open = false;
    dirty(gfx_rect(d.win[id].r.x, d.win[id].r.y, d.win[id].r.w + 4, d.win[id].r.h + 4));
    dirty(bar_rect());
    const int top = top_open();
    if ( 0 <= top )
    {
        dirty(d.win[top].r);
    }
}

static void list_path(const char *path)
{
    snprintf(d.path, sizeof(d.path), "%s", path);
    d.nfiles = 0;
    d.files_scroll = 0;
    d.files_loading = true;
    d.files_failed = false;
    act(DESK_ACT_LIST, 0);
    dirty(d.win[W_FILES].r);
}

static void click_files(int lx, int ly)
{
    (void)lx;
    const int row = (ly - 4) / ROW_H;
    if ( (0 > ly - 4) || (files_rows() <= row) )
    {
        return;
    }
    const int v = d.files_scroll + row;
    if ( files_virtual() <= v )
    {
        return;
    }
    const int idx = files_entry_of(v);
    if ( 0 > idx )
    {
        char up[DESK_PATH_MAX];
        snprintf(up, sizeof(up), "%s", d.path);
        char *slash = strrchr(up, '/');
        if ( slash && (slash != up) )
        {
            *slash = 0;
        }
        list_path(up);
        return;
    }
    if ( !d.files[idx].dir )
    {
        return;
    }
    char next[DESK_PATH_MAX + DESK_NAME_MAX + 2];
    if ( (strlen(d.path) + 1 + strlen(d.files[idx].name)) >= DESK_PATH_MAX )
    {
        return;
    }
    snprintf(next, sizeof(next), "%s/%s", d.path, d.files[idx].name);
    list_path(next);
}

static void click_notes(int x, int y)
{
    if ( gfx_contains(notes_btn(0), x, y) )
    {
        snprintf(d.notes_msg, sizeof(d.notes_msg), "saving...");
        act(DESK_ACT_SAVE_NOTES, 0);
        dirty_content(W_NOTES);
    }
    else if ( gfx_contains(notes_btn(1), x, y) )
    {
        d.nnotes = 0;
        d.notes[0] = 0;
        d.notes_msg[0] = 0;
        dirty_content(W_NOTES);
    }
}

static void click_display(int x, int y)
{
    for ( int i = 0; 5 > i; i++ )
    {
        if ( !gfx_contains(disp_btn(i), x, y) )
        {
            continue;
        }
        if ( 3 > i )
        {
            act(DESK_ACT_RES, i);
        }
        else
        {
            const int q = d.st.quality + ((3 == i) ? -10 : 10);
            act(DESK_ACT_QUALITY, clampi(q, 10, 95));
        }
        return;
    }
}

void desk_button(bool down)
{
    if ( !down )
    {
        d.down = false;
        d.drag = -1;
        return;
    }
    if ( d.down )
    {
        return;
    }
    d.down = true;
    const int x = d.cx;
    const int y = d.cy;
    if ( gfx_contains(bar_rect(), x, y) )
    {
        for ( int i = 0; W_COUNT > i; i++ )
        {
            if ( d.win[i].open && show_tasks() && gfx_contains(task_btn(i), x, y) )
            {
                raise(i);
            }
        }
        return;
    }
    for ( int i = d.nz - 1; 0 <= i; i-- )
    {
        const int id = d.z[i];
        win_t *w = &d.win[id];
        if ( !w->open || !gfx_contains(w->r, x, y) )
        {
            continue;
        }
        raise(id);
        if ( gfx_contains(close_box(id), x, y) )
        {
            close_win(id);
            return;
        }
        if ( y < w->r.y + TITLE_H )
        {
            d.drag = id;
            d.drag_dx = x - w->r.x;
            d.drag_dy = y - w->r.y;
            return;
        }
        const gfx_rect_t c = content(id);
        switch ( id )
        {
        case W_FILES:
            click_files(x - c.x, y - c.y);
            break;
        case W_NOTES:
            click_notes(x, y);
            break;
        case W_DISPLAY:
            click_display(x, y);
            break;
        default:
            break;
        }
        return;
    }
    for ( int i = 0; W_COUNT > i; i++ )
    {
        if ( gfx_contains(icon_rect(i), x, y) )
        {
            raise(i);
            return;
        }
    }
}

void desk_wheel(int rows)
{
    const int top = top_open();
    if ( W_FILES != top )
    {
        return;
    }
    const int max = files_virtual() - files_rows();
    d.files_scroll = clampi(d.files_scroll + rows, 0, (0 < max) ? max : 0);
    dirty_content(W_FILES);
}

void desk_key(const char *key)
{
    const int top = top_open();
    if ( W_FILES == top )
    {
        if ( 0 == strcmp(key, "ArrowDown") )
        {
            desk_wheel(1);
        }
        else if ( 0 == strcmp(key, "ArrowUp") )
        {
            desk_wheel(-1);
        }
        else if ( 0 == strcmp(key, "PageDown") )
        {
            desk_wheel(files_rows());
        }
        else if ( 0 == strcmp(key, "PageUp") )
        {
            desk_wheel(-files_rows());
        }
        return;
    }
    if ( W_NOTES != top )
    {
        return;
    }
    if ( 0 == strcmp(key, "Backspace") )
    {
        if ( d.nnotes )
        {
            d.notes[--d.nnotes] = 0;
        }
    }
    else if ( 0 == strcmp(key, "Enter") )
    {
        if ( DESK_NOTES_MAX > d.nnotes )
        {
            d.notes[d.nnotes++] = '\n';
            d.notes[d.nnotes] = 0;
        }
    }
    else if ( (1 == strlen(key)) && (' ' <= key[0]) && ('~' >= key[0]) )
    {
        if ( DESK_NOTES_MAX > d.nnotes )
        {
            d.notes[d.nnotes++] = key[0];
            d.notes[d.nnotes] = 0;
        }
    }
    else
    {
        return;
    }
    if ( d.notes_msg[0] )
    {
        d.notes_msg[0] = 0;
    }
    dirty_content(W_NOTES);
}

/* ---- files and notes ----------------------------------------------------------- */

const char *desk_files_path(void)
{
    return d.path;
}

void desk_files_begin(int skip)
{
    if ( 0 == skip )
    {
        d.nfiles = 0;
    }
    d.files_loading = true;
    d.files_failed = false;
}

void desk_files_entry(const char *name, uint32_t size, bool is_dir)
{
    if ( FILES_MAX <= d.nfiles )
    {
        return;
    }
    entry_t *e = &d.files[d.nfiles++];
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->size = size;
    e->dir = is_dir;
    dirty_content(W_FILES);
}

void desk_files_end(bool ok, bool more)
{
    if ( !ok )
    {
        d.files_failed = true;
        d.files_loading = false;
    }
    else if ( more && (FILES_MAX > d.nfiles) )
    {
        act(DESK_ACT_LIST, d.nfiles);
    }
    else
    {
        d.files_loading = false;
    }
    dirty_content(W_FILES);
}

const char *desk_notes(size_t *len)
{
    *len = d.nnotes;
    return d.notes;
}

void desk_notes_set(const char *text, size_t len)
{
    if ( DESK_NOTES_MAX < len )
    {
        len = DESK_NOTES_MAX;
    }
    size_t n = 0;
    for ( size_t i = 0; len > i; i++ )
    {
        const char c = text[i];
        if ( ('\n' == c) || ((' ' <= c) && ('~' >= c)) )
        {
            d.notes[n++] = c;
        }
    }
    d.notes[n] = 0;
    d.nnotes = n;
    dirty_content(W_NOTES);
}

void desk_notes_saved(bool ok)
{
    snprintf(d.notes_msg, sizeof(d.notes_msg), "%s", ok ? "saved to the card" : "save failed");
    dirty_content(W_NOTES);
}
