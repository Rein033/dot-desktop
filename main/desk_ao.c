/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file desk_ao.c
 * @brief The desktop's Active Object.
 *
 * A tick every TICK_MS: move the cursor for held pad buttons, keep the clock and the status,
 * paint what changed into the shared frame (only when its lock is free: this AO never waits)
 * and wake the outputs; every few ticks, draw the Dot's own glass: the address to open, the
 * state and a small picture of the desktop.
 *
 * Input: the pad moves the cursor, TOP is the mouse button, the glass is a touchpad (a tap is
 * a click). A viewer's mouse and keyboard arrive as INPUT events from the server's task.
 *
 * What blocks - starting the HTTP server, keeping a setting - goes to the helper task.
 */
#include "desk_ao.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "kaos.h"
#include "kdrv_buttons.h"
#include "kdrv_display.h"
#include "kdrv_wifi.h"
#include "kode.h"
#include "kqp_signals.h"

#include "desk.h"
#include "fbshare.h"
#include "jpeg_enc.h"
#include "stream.h"

#define TAG "dd.ao"

#define TICK_MS        33
#define GLASS_EVERY    5          /* the Dot's glass at about 6 per second */
#define SC_W           410
#define SC_H           502
#define NOTES_PATH     "/sdcard/notes.txt"
#define TAP_MS         250
#define TAP_SLOP       8

enum DeskSignals {
    DESK_TICK_SIG = MAX_SIG,
    DESK_INPUT_SIG,
};

typedef struct {
    QEvt  super;
    char  kind;
    int   a;
    int   b;
    char  key[16];
} InputEvt;

typedef struct {
    QActive    super;
    QTimeEvt   tick;
    desk_cfg_t cfg;
    int        want_w;       /**< the size the desktop should have */
    int        want_h;
    bool       resize;       /**< waiting for the frame's lock to resize */
    uint32_t   ticks;
    uint32_t   held;         /**< pad buttons down, as bits of kdrv_btn */
    int        speed;        /**< the pad's cursor speed, growing while held */
    bool       wifi_up;
    bool       wifi_asked;
    char       ip[16];
    char       ssid[33];
    int        rssi;
    bool       batt_known;
    int        batt_pct;
    int        vbat_mv;
    bool       charging;
    bool       touch_down;
    int        tx;
    int        ty;
    int        tx0;
    int        ty0;
    uint32_t   t_down;
    bool       moved;
    bool       glass_up;
    bool       in_flight;
    uint16_t  *thumb;        /**< the desktop scaled for the glass */
    int        thumb_w;
    int        thumb_h;
    bool       thumb_new;
    bool       listing;
    char      *note_buf;     /**< lent to AO_Storage for a read or a write; PSRAM */
    bool       note_busy;
    bool       notes_loaded;
    uint32_t   retry_at;     /**< when to ask the card again; 0 = not waiting */
    int        retries;
} DeskAo;

static QEvtPtr     l_queue[64];
static StackType_t s_stack[KQP_AO_STACK_BYTES / sizeof(StackType_t)];

static DeskAo l_ao;
QActive * const AO_Desk = &l_ao.super;

static QState DeskAo_initial(DeskAo * const me, void const * const par);
static QState DeskAo_running(DeskAo * const me, QEvt const * const e);

/* ---- the helper task: what blocks ------------------------------------------------- */

typedef enum {
    HELP_HTTP = 0,
    HELP_SETTING,
} help_op_t;

typedef struct {
    help_op_t op;
    char      key[16];
    char      value[16];
} help_t;

static QueueHandle_t s_help;

static void helper_task(void *arg)
{
    (void)arg;
    help_t h;
    for ( ;; )
    {
        if ( pdTRUE != xQueueReceive(s_help, &h, portMAX_DELAY) )
        {
            continue;
        }
        if ( HELP_HTTP == h.op )
        {
            (void)stream_start();
        }
        else
        {
            if ( !kode_setting_set(h.key, h.value) )
            {
                KLOG_W(TAG, "setting %s not kept: %s", h.key, kode_error());
            }
        }
    }
}

static void help(help_op_t op, const char *key, int value)
{
    help_t h = { .op = op };
    if ( key )
    {
        snprintf(h.key, sizeof(h.key), "%s", key);
        snprintf(h.value, sizeof(h.value), "%d", value);
    }
    (void)xQueueSend(s_help, &h, 0);
}

/* ---- input from viewers ------------------------------------------------------------- */

void DeskAo_input(char kind, int a, int b, const char *key)
{
    /* From the server's task: never the last slots of the queue, never QP's assert. */
    InputEvt *ev = Q_NEW_X(InputEvt, 4U, DESK_INPUT_SIG);
    if ( !ev )
    {
        return;
    }
    ev->kind = kind;
    ev->a = a;
    ev->b = b;
    ev->key[0] = 0;
    if ( key )
    {
        snprintf(ev->key, sizeof(ev->key), "%s", key);
    }
    if ( !QACTIVE_POST_X(AO_Desk, &ev->super, 8U, (void *)0) )
    {
        /* Not taken: QP gives the event back to the pool. */
    }
}

/* ---- time and status ---------------------------------------------------------------- */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void clock_tick(void)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    if ( 2024 > tm.tm_year + 1900 )
    {
        desk_clock(-1, 0, 0);
        return;
    }
    desk_clock(tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void status_tick(DeskAo * const me)
{
    desk_status_t st;
    memset(&st, 0, sizeof(st));
    st.wifi_up = me->wifi_up;
    snprintf(st.ip, sizeof(st.ip), "%s", me->ip);
    snprintf(st.ssid, sizeof(st.ssid), "%s", me->ssid);
    st.rssi = me->rssi;
    st.batt_known = me->batt_known;
    st.batt_pct = me->batt_pct;
    st.charging = me->charging;
    st.vbat_mv = me->vbat_mv;
    st.clients = stream_clients();
    stream_stats(&st.fps, &st.kbps);
    st.hw_jpeg = jpg_is_hw();
    st.quality = me->cfg.quality;
    st.res = me->cfg.res;
    st.uptime_s = now_ms() / 1000u;
    st.heap_int_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024u);
    st.heap_psram_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024u);
    desk_status(&st);
}

/* ---- the output ----------------------------------------------------------------- */

static void want_size(DeskAo * const me)
{
    me->want_w = desk_res_w(me->cfg.res);
    me->want_h = desk_res_h(me->cfg.res);
    me->resize = true;
}

static void do_action(DeskAo * const me, const desk_act_t *a)
{
    switch ( a->kind )
    {
    case DESK_ACT_LIST:
        if ( 0 == a->value )
        {
            desk_files_begin(0);
        }
        if ( KODE_OK != kode_sys_file_list(&me->super, desk_files_path(), (uint16_t)a->value) )
        {
            desk_files_end(false, false);
        }
        break;
    case DESK_ACT_SAVE_NOTES: {
        if ( me->note_busy || !me->note_buf )
        {
            desk_notes_saved(false);
            break;
        }
        size_t n = 0;
        const char *t = desk_notes(&n);
        memcpy(me->note_buf, t, n);
        if ( KODE_OK == kode_sys_file_write(&me->super, NOTES_PATH, me->note_buf, (uint32_t)n, false) )
        {
            me->note_busy = true;
        }
        else
        {
            desk_notes_saved(false);
        }
        break;
    }
    case DESK_ACT_RES:
        if ( (0 <= a->value) && (DESK_RES_COUNT > a->value) && (a->value != me->cfg.res) )
        {
            me->cfg.res = a->value;
            help(HELP_SETTING, "ddesk_res", a->value);
            want_size(me);
        }
        break;
    case DESK_ACT_QUALITY:
        me->cfg.quality = a->value;
        stream_set_quality(a->value);
        help(HELP_SETTING, "ddesk_q", a->value);
        break;
    default:
        break;
    }
}

/* ---- the frame -------------------------------------------------------------------- */

static void mark(gfx_rect_t r)
{
    fbs_mark(r);
}

/** @brief Scale the desktop into the thumbnail (lock held). */
static void make_thumb(DeskAo * const me)
{
    const int w = fbs_w();
    const int h = fbs_h();
    me->thumb_w = 390;
    me->thumb_h = 390 * h / w;
    const uint16_t *fb = fbs_pixels();
    for ( int y = 0; me->thumb_h > y; y++ )
    {
        const uint16_t *row = fb + (size_t)(y * h / me->thumb_h) * w;
        uint16_t *out = me->thumb + y * me->thumb_w;
        for ( int x = 0; me->thumb_w > x; x++ )
        {
            out[x] = row[x * w / me->thumb_w];
        }
    }
    me->thumb_new = true;
}

static void render(DeskAo * const me)
{
    if ( !me->resize && !desk_dirty() && (0 != me->ticks % GLASS_EVERY) )
    {
        return;
    }
    if ( !fbs_try_lock() )
    {
        return; /* an output is copying; try again next tick */
    }
    if ( me->resize )
    {
        if ( fbs_configure(me->want_w, me->want_h) && desk_resize(me->want_w, me->want_h) )
        {
            KLOG_I(TAG, "desktop %dx%d", me->want_w, me->want_h);
        }
        me->resize = false;
    }
    const bool painted = desk_render(fbs_pixels(), mark);
    if ( me->thumb && (0 == me->ticks % GLASS_EVERY) && (painted || !me->thumb_new) )
    {
        make_thumb(me);
    }
    fbs_unlock();
    if ( painted )
    {
        fbs_signal();
    }
}

/* ---- the Dot's own glass ------------------------------------------------------------- */

static void glass(DeskAo * const me)
{
    if ( !me->glass_up || (0 != me->ticks % GLASS_EVERY) )
    {
        return;
    }
    if ( me->in_flight )
    {
        if ( !kdrv_display_flush_wait(0) )
        {
            return;
        }
        me->in_flight = false;
    }
    uint16_t * const back = kdrv_display_back_buffer();
    if ( !back )
    {
        return;
    }
    gfx_surf_t s;
    gfx_surf_init(&s, back, SC_W, SC_H, SC_W);
    const uint16_t ink = RGB(0xDE, 0xD7, 0xCD);
    const uint16_t dim = RGB(0xC2, 0xBA, 0xB3);
    const uint16_t yellow = RGB(0xFF, 0xAC, 0x00);
    gfx_fill(&s, gfx_rect(0, 0, SC_W, SC_H), 0);
    gfx_atext(&s, (SC_W - gfx_atext_w(&font_title, "Dot Desktop")) / 2, 14, &font_title, "Dot Desktop", ink, 0);
    char b[48];
    if ( me->wifi_up )
    {
        snprintf(b, sizeof(b), "http://%s", me->ip);
        const int tw = gfx_atext_w(&font_bold, b) + 36;
        gfx_pill(&s, gfx_rect((SC_W - tw) / 2, 56, tw, 40), yellow);
        gfx_atext(&s, (SC_W - tw) / 2 + 18, 56 + (40 - font_bold.line_h) / 2, &font_bold, b, RGB(0x19, 0x19, 0x19), 0);
    }
    else
    {
        const char *t = me->wifi_asked ? "Joining Wi-Fi..." : "No Wi-Fi: join one in Settings";
        gfx_atext(&s, (SC_W - gfx_atext_w(&font_ui, t)) / 2, 66, &font_ui, t, dim, SC_W - 20);
    }
    if ( me->thumb && me->thumb_new )
    {
        const int x0 = (SC_W - me->thumb_w) / 2;
        const int y0 = 112;
        for ( int y = 0; me->thumb_h > y; y++ )
        {
            memcpy(back + (y0 + y) * SC_W + x0, me->thumb + y * me->thumb_w, (size_t)me->thumb_w * 2u);
        }
    }
    int fps = 0;
    int kbps = 0;
    stream_stats(&fps, &kbps);
    snprintf(b, sizeof(b), "%d viewer%s, %d fps, %d kB/s", stream_clients(), (1 == stream_clients()) ? "" : "s", fps,
             kbps);
    gfx_atext(&s, (SC_W - gfx_atext_w(&font_ui, b)) / 2, 370, &font_ui, b, dim, 0);
    static const char *const HINT[] = {
        "Glass: touchpad, tap to click",
        "Pad: move     TOP: click, hold to drag",
        "Hold BOT to go home",
    };
    for ( int i = 0; 3 > i; i++ )
    {
        gfx_atext(&s, (SC_W - gfx_atext_w(&font_ui, HINT[i])) / 2, 412 + i * 26, &font_ui, HINT[i],
                  RGB(0x8A, 0x86, 0x80), 0);
    }
    FlushEvt * const ev = Q_NEW_X(FlushEvt, 0U, DISPLAY_FLUSH_SIG);
    if ( !ev )
    {
        return;
    }
    ev->x1 = 0;
    ev->y1 = 0;
    ev->x2 = SC_W;
    ev->y2 = SC_H;
    ev->pixels = (uint8_t *)back;
    ev->last = true;
    QACTIVE_POST(AO_Display, &ev->super, &me->super);
    me->in_flight = true;
}

/* ---- the pad ---------------------------------------------------------------------- */

static void pad_tick(DeskAo * const me)
{
    const uint32_t dirs = me->held & ((1u << KDRV_BTN_UP) | (1u << KDRV_BTN_DOWN) | (1u << KDRV_BTN_LEFT)
                                      | (1u << KDRV_BTN_RIGHT));
    if ( !dirs )
    {
        me->speed = 2;
        return;
    }
    int dx = 0;
    int dy = 0;
    if ( dirs & (1u << KDRV_BTN_LEFT) )
    {
        dx -= me->speed;
    }
    if ( dirs & (1u << KDRV_BTN_RIGHT) )
    {
        dx += me->speed;
    }
    if ( dirs & (1u << KDRV_BTN_UP) )
    {
        dy -= me->speed;
    }
    if ( dirs & (1u << KDRV_BTN_DOWN) )
    {
        dy += me->speed;
    }
    desk_pointer_rel(dx, dy);
    if ( 24 > me->speed )
    {
        me->speed++;
    }
}

static void on_touch(DeskAo * const me, TouchEvt const *t)
{
    const uint32_t now = now_ms();
    if ( t->pressed && !me->touch_down )
    {
        me->touch_down = true;
        me->tx = me->tx0 = t->x;
        me->ty = me->ty0 = t->y;
        me->t_down = now;
        me->moved = false;
        return;
    }
    if ( t->pressed )
    {
        /* A touchpad: the cursor follows the finger, twice as far on the desktop. */
        const int gain = (900 <= fbs_w()) ? 3 : 2;
        desk_pointer_rel((t->x - me->tx) * gain, (t->y - me->ty) * gain);
        me->tx = t->x;
        me->ty = t->y;
        if ( (TAP_SLOP < abs(t->x - me->tx0)) || (TAP_SLOP < abs(t->y - me->ty0)) )
        {
            me->moved = true;
        }
        return;
    }
    if ( me->touch_down )
    {
        me->touch_down = false;
        if ( !me->moved && !t->stale && (TAP_MS >= now - me->t_down) )
        {
            desk_button(true);
            desk_button(false);
        }
    }
}

/* ---- the AO ----------------------------------------------------------------------- */

void DeskAo_ctor(const desk_cfg_t *cfg)
{
    DeskAo * const me = &l_ao;
    QActive_ctor(&me->super, Q_STATE_CAST(&DeskAo_initial));
    QTimeEvt_ctorX(&me->tick, &me->super, DESK_TICK_SIG, 0U);
    me->cfg = *cfg;
}

void DeskAo_start(void)
{
    s_help = xQueueCreate(8, sizeof(help_t));
    xTaskCreatePinnedToCore(helper_task, "dd_help", 4096, NULL, 2, NULL, 1);
    QActive_setAttr(AO_Desk, TASK_NAME_ATTR, "desk_ao");
    QACTIVE_START(AO_Desk, KODE_PRIO_USER_GUI, l_queue, Q_DIM(l_queue), s_stack, sizeof(s_stack), (void *)0);
}

static QState DeskAo_initial(DeskAo * const me, void const * const par)
{
    (void)par;
    me->speed = 2;
    me->thumb = heap_caps_malloc(390u * 390u * 2u, MALLOC_CAP_SPIRAM);
    me->note_buf = heap_caps_calloc(DESK_NOTES_MAX + 1u, 1, MALLOC_CAP_SPIRAM);
    want_size(me);
    if ( !desk_init(me->want_w, me->want_h) )
    {
        KLOG_E(TAG, "the desktop would not allocate");
    }
    stream_set_quality(me->cfg.quality);
    QActive_subscribe(&me->super, BUTTON_EVT_SIG);
    QActive_subscribe(&me->super, TOUCH_EVT_SIG);
    QActive_subscribe(&me->super, DISPLAY_STATE_SIG);
    QActive_subscribe(&me->super, WIFI_STATE_SIG);
    QActive_subscribe(&me->super, POWER_EVT_SIG);
    display_state_request();
    wifi_state_request();
    power_peek_request();
    stream_enable(true);
    /* The notes from the last visit. */
    if ( me->note_buf
         && (KODE_OK == kode_sys_file_read(&me->super, NOTES_PATH, 0, me->note_buf, DESK_NOTES_MAX)) )
    {
        me->note_busy = true;
    }
    QTimeEvt_armX(&me->tick, KQP_TICKS_PER_MS(TICK_MS), KQP_TICKS_PER_MS(TICK_MS));
    return Q_TRAN(&DeskAo_running);
}

static void on_wifi(DeskAo * const me, WifiStateEvt const *w)
{
    const bool up = (KDRV_WIFI_GOT_IP == w->state) && (0u != w->ip4);
    if ( up )
    {
        const uint8_t *ip = (const uint8_t *)&w->ip4;
        snprintf(me->ip, sizeof(me->ip), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
        snprintf(me->ssid, sizeof(me->ssid), "%s", w->ssid);
        me->rssi = w->rssi;
        if ( !me->wifi_up )
        {
            KLOG_I(TAG, "Wi-Fi up: open http://%s/", me->ip);
            help(HELP_HTTP, NULL, 0);
        }
    }
    me->wifi_up = up;
    if ( !up && !me->wifi_asked && !w->joining && !w->reconnecting )
    {
        /* Wi-Fi off, or on and idle: switch it on and join a remembered network. */
        me->wifi_asked = true;
        if ( !w->enabled )
        {
            wifi_start_request();
        }
        (void)kode_sys_wifi_connect(NULL, NULL);
    }
}

static QState DeskAo_running(DeskAo * const me, QEvt const * const e)
{
    switch ( e->sig )
    {
    case DESK_TICK_SIG: {
        me->ticks++;
        pad_tick(me);
        if ( 0 == me->ticks % 15 )
        {
            clock_tick();
            status_tick(me);
        }
        desk_act_t a;
        while ( desk_take_action(&a) )
        {
            do_action(me, &a);
        }
        if ( me->retry_at && ((int32_t)(now_ms() - me->retry_at) >= 0) )
        {
            me->retry_at = 0;
            const desk_act_t ls = { DESK_ACT_LIST, 0 };
            do_action(me, &ls);
            if ( !me->notes_loaded && !me->note_busy && me->note_buf )
            {
                size_t n = 0;
                (void)desk_notes(&n);
                if ( (0 == n)
                     && (KODE_OK == kode_sys_file_read(&me->super, NOTES_PATH, 0, me->note_buf, DESK_NOTES_MAX)) )
                {
                    me->note_busy = true;
                    me->notes_loaded = true; /* one try more; a card without the file is normal */
                }
            }
        }
        render(me);
        glass(me);
        return Q_HANDLED();
    }
    case DESK_INPUT_SIG: {
        InputEvt const *in = (InputEvt const *)e;
        if ( 'm' == in->kind )
        {
            desk_pointer_abs(in->a, in->b & 0xFFFF);
            desk_button(0 != (in->b & 0x10000));
        }
        else if ( 'w' == in->kind )
        {
            desk_wheel(in->a);
        }
        else if ( 'k' == in->kind )
        {
            desk_key(in->key);
        }
        return Q_HANDLED();
    }
    case BUTTON_EVT_SIG: {
        ButtonEvt const *be = (ButtonEvt const *)e;
        if ( KDRV_BTN_COUNT <= be->id )
        {
            return Q_HANDLED();
        }
        if ( be->pressed )
        {
            me->held |= 1u << be->id;
        }
        else
        {
            me->held &= ~(1u << be->id);
        }
        if ( KDRV_BTN_TOP == be->id )
        {
            desk_button(be->pressed);
        }
        return Q_HANDLED();
    }
    case TOUCH_EVT_SIG:
        on_touch(me, (TouchEvt const *)e);
        return Q_HANDLED();
    case DISPLAY_STATE_SIG:
        me->glass_up = ((DisplayStateEvt const *)e)->up;
        return Q_HANDLED();
    case WIFI_STATE_SIG:
        on_wifi(me, (WifiStateEvt const *)e);
        return Q_HANDLED();
    case POWER_EVT_SIG: {
        PowerEvt const *p = (PowerEvt const *)e;
        me->batt_known = (0 < p->vbat_mv);
        me->batt_pct = p->soc_pct;
        me->vbat_mv = p->vbat_mv;
        me->charging = p->vbus && (0 != p->charging);
        return Q_HANDLED();
    }
    case STORAGE_ENTRY_SIG: {
        StorageEntryEvt const *s = (StorageEntryEvt const *)e;
        desk_files_entry(s->name, s->size, s->is_dir);
        return Q_HANDLED();
    }
    case FILE_LIST_END_SIG: {
        FileRspEvt const *r = (FileRspEvt const *)e;
        desk_files_end(KODE_OK == r->rc, r->more);
        if ( KODE_OK == r->rc )
        {
            me->retries = 0;
        }
        if ( KODE_OK != r->rc )
        {
            /* The card is unmounted while the radio's link comes up, and kodeOS says nothing
             * when it is back: ask again in a while. */
            KLOG_I(TAG, "listing %s: rc %u", desk_files_path(), (unsigned)r->rc);
            if ( 20 > me->retries )
            {
                me->retries++;
                me->retry_at = now_ms() + 3000u;
            }
        }
        return Q_HANDLED();
    }
    case FILE_READ_RSP_SIG: {
        FileRspEvt const *r = (FileRspEvt const *)e;
        me->note_busy = false;
        if ( (KODE_OK == r->rc) && r->got )
        {
            me->notes_loaded = true;
            desk_notes_set(me->note_buf, r->got);
        }
        return Q_HANDLED();
    }
    case FILE_WRITE_RSP_SIG: {
        FileRspEvt const *r = (FileRspEvt const *)e;
        me->note_busy = false;
        desk_notes_saved(KODE_OK == r->rc);
        if ( (KODE_OK == r->rc) && (0 == strcmp(desk_files_path(), "/sdcard")) )
        {
            const desk_act_t ls = { DESK_ACT_LIST, 0 };
            do_action(me, &ls);
        }
        return Q_HANDLED();
    }
    default:
        return Q_SUPER(&QHsm_top);
    }
}
