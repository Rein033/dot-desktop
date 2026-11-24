/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fbshare.c
 * @brief The shared desktop frame and its per-output dirty tiles.
 */
#include "fbshare.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/semphr.h"

#define TW_MAX ((FBS_MAX_W + FBS_TILE - 1) / FBS_TILE)
#define TH_MAX ((FBS_MAX_H + FBS_TILE - 1) / FBS_TILE)

typedef struct {
    TaskHandle_t task;
    bool         on;
    bool         any;     /**< something is marked */
    uint8_t     *mask;    /**< TW_MAX x TH_MAX, one byte per tile */
} sink_t;

static uint16_t         *s_px;
static int               s_w = FBS_MAX_W;
static int               s_h = FBS_MAX_H;
static uint32_t          s_gen = 1;
static SemaphoreHandle_t s_mutex;
static sink_t            s_sink[FBS_MAX_SINKS];
static int               s_nsink;

bool fbs_init(void)
{
    if ( s_px )
    {
        return true;
    }
    s_px = heap_caps_calloc((size_t)FBS_MAX_W * FBS_MAX_H, 2, MALLOC_CAP_SPIRAM);
    s_mutex = xSemaphoreCreateMutex();
    return (NULL != s_px) && (NULL != s_mutex);
}

bool fbs_try_lock(void)
{
    return pdTRUE == xSemaphoreTake(s_mutex, 0);
}

void fbs_lock(void)
{
    (void)xSemaphoreTake(s_mutex, portMAX_DELAY);
}

void fbs_unlock(void)
{
    (void)xSemaphoreGive(s_mutex);
}

uint16_t *fbs_pixels(void)
{
    return s_px;
}

int fbs_w(void)
{
    return s_w;
}

int fbs_h(void)
{
    return s_h;
}

uint32_t fbs_generation(void)
{
    return s_gen;
}

bool fbs_configure(int w, int h)
{
    if ( (FBS_MAX_W < w) || (FBS_MAX_H < h) || (16 > w) || (16 > h) )
    {
        return false;
    }
    s_w = w;
    s_h = h;
    s_gen++;
    for ( int i = 0; s_nsink > i; i++ )
    {
        fbs_mark_all(i);
    }
    return true;
}

int fbs_sink_add(TaskHandle_t task)
{
    if ( FBS_MAX_SINKS <= s_nsink )
    {
        return -1;
    }
    sink_t *k = &s_sink[s_nsink];
    k->mask = heap_caps_calloc(TW_MAX * TH_MAX, 1, MALLOC_CAP_SPIRAM);
    if ( !k->mask )
    {
        return -1;
    }
    k->task = task;
    k->on = false;
    return s_nsink++;
}

void fbs_sink_enable(int sink, bool on)
{
    if ( (0 <= sink) && (s_nsink > sink) )
    {
        s_sink[sink].on = on;
    }
}

static void mark_one(sink_t *k, gfx_rect_t r)
{
    r = gfx_intersect(r, gfx_rect(0, 0, s_w, s_h));
    if ( gfx_empty(r) )
    {
        return;
    }
    const int tx0 = r.x / FBS_TILE;
    const int ty0 = r.y / FBS_TILE;
    const int tx1 = (r.x + r.w - 1) / FBS_TILE;
    const int ty1 = (r.y + r.h - 1) / FBS_TILE;
    for ( int ty = ty0; ty1 >= ty; ty++ )
    {
        memset(k->mask + ty * TW_MAX + tx0, 1, (size_t)(tx1 - tx0 + 1));
    }
    k->any = true;
}

void fbs_mark(gfx_rect_t r)
{
    for ( int i = 0; s_nsink > i; i++ )
    {
        if ( s_sink[i].on )
        {
            mark_one(&s_sink[i], r);
        }
    }
}

void fbs_mark_all(int sink)
{
    if ( (0 <= sink) && (s_nsink > sink) )
    {
        mark_one(&s_sink[sink], gfx_rect(0, 0, s_w, s_h));
    }
}

void fbs_signal(void)
{
    for ( int i = 0; s_nsink > i; i++ )
    {
        if ( s_sink[i].on && s_sink[i].any && s_sink[i].task )
        {
            xTaskNotifyGive(s_sink[i].task);
        }
    }
}

static fbs_rect_t to_px(int tx, int ty, int tw, int th)
{
    fbs_rect_t r;
    int x = tx * FBS_TILE;
    int y = ty * FBS_TILE;
    int w = tw * FBS_TILE;
    int h = th * FBS_TILE;
    if ( x + w > s_w )
    {
        w = s_w - x;
    }
    if ( y + h > s_h )
    {
        h = s_h - y;
    }
    r.x = (uint16_t)x;
    r.y = (uint16_t)y;
    r.w = (uint16_t)w;
    r.h = (uint16_t)h;
    return r;
}

int fbs_take(int sink, fbs_rect_t *out, int max)
{
    if ( (0 > sink) || (s_nsink <= sink) || (1 > max) )
    {
        return 0;
    }
    sink_t *k = &s_sink[sink];
    if ( !k->any )
    {
        return 0;
    }
    const int tw = (s_w + FBS_TILE - 1) / FBS_TILE;
    const int th = (s_h + FBS_TILE - 1) / FBS_TILE;
    uint8_t *m = k->mask;
    int n = 0;
    for ( int ty = 0; th > ty; ty++ )
    {
        for ( int tx = 0; tw > tx; tx++ )
        {
            if ( !m[ty * TW_MAX + tx] )
            {
                continue;
            }
            if ( n == max - 1 )
            {
                /* Out of room: one band from here to the bottom holds the rest. */
                int x0 = tw;
                int x1 = -1;
                int y1 = ty;
                for ( int y = ty; th > y; y++ )
                {
                    for ( int x = 0; tw > x; x++ )
                    {
                        if ( m[y * TW_MAX + x] )
                        {
                            if ( x < x0 )
                            {
                                x0 = x;
                            }
                            if ( x > x1 )
                            {
                                x1 = x;
                            }
                            y1 = y;
                            m[y * TW_MAX + x] = 0;
                        }
                    }
                }
                out[n++] = to_px(x0, ty, x1 - x0 + 1, y1 - ty + 1);
                k->any = false;
                return n;
            }
            int e = tx;
            while ( (tw > e) && m[ty * TW_MAX + e] )
            {
                e++;
            }
            int bottom = ty + 1;
            while ( th > bottom )
            {
                bool full = true;
                for ( int x = tx; e > x; x++ )
                {
                    if ( !m[bottom * TW_MAX + x] )
                    {
                        full = false;
                        break;
                    }
                }
                if ( !full )
                {
                    break;
                }
                bottom++;
            }
            for ( int y = ty; bottom > y; y++ )
            {
                memset(m + y * TW_MAX + tx, 0, (size_t)(e - tx));
            }
            out[n++] = to_px(tx, ty, e - tx, bottom - ty);
            tx = e;
        }
    }
    k->any = false;
    return n;
}
