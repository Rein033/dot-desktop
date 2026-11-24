/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file desk.h
 * @brief The desktop: wallpaper, icons, windows, a taskbar and a cursor, painted region by region.
 *
 * Pure C: no SDK. The Active Object feeds it input and status, asks it to paint what changed
 * into the shared frame, and carries out the actions it asks for (list a folder, save the notes,
 * change the size).
 */
#ifndef DESK_H_
#define DESK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DESK_PATH_MAX  96
#define DESK_NAME_MAX  48
#define DESK_NOTES_MAX 4096

/** @brief The desktop's size, as streamed. */
typedef enum {
    DESK_RES_1280 = 0, /**< 1280 x 720 */
    DESK_RES_1024,     /**< 1024 x 576 */
    DESK_RES_800,      /**< 800 x 480: for a slow network */
    DESK_RES_COUNT
} desk_res_t;

/** @brief The width and height of a desk_res_t. */
int desk_res_w(int res);
int desk_res_h(int res);

/** @brief What the desktop shows about the board; the Active Object fills it in. */
typedef struct {
    bool     wifi_up;
    char     ip[16];
    char     ssid[33];
    int      rssi;
    bool     batt_known;
    int      batt_pct;
    bool     charging;
    int      vbat_mv;
    int      clients;
    int      fps;
    int      kbps;
    bool     hw_jpeg;
    int      quality;
    int      res;          /**< desk_res_t */
    uint32_t uptime_s;
    uint32_t heap_int_kb;
    uint32_t heap_psram_kb;
    bool     sd_ok;
} desk_status_t;

/** @brief What the desktop asks the Active Object to do. */
typedef enum {
    DESK_ACT_NONE = 0,
    DESK_ACT_LIST,       /**< list desk_files_path() from entry @c value */
    DESK_ACT_SAVE_NOTES, /**< write desk_notes() to the card */
    DESK_ACT_RES,        /**< resize the desktop to @c value (desk_res_t) */
    DESK_ACT_QUALITY,    /**< the stream's JPEG quality is @c value */
} desk_act_kind_t;

typedef struct {
    desk_act_kind_t kind;
    int             value;
} desk_act_t;

/** @brief Called for every rectangle desk_render() painted. */
typedef void (*desk_mark_fn)(gfx_rect_t r);

/** @brief Start at @p w x @p h. @return false when the wallpaper would not allocate. */
bool desk_init(int w, int h);

/** @brief Change size: everything is laid out again and repainted. */
bool desk_resize(int w, int h);

/** @brief Paint every region that changed into @p fb (the desktop's size) and report each. */
bool desk_render(uint16_t *fb, desk_mark_fn mark);

/** @brief Whether anything waits to be painted. */
bool desk_dirty(void);

/** @brief Paint everything at the next desk_render(). */
void desk_invalidate(void);

/** @brief New status; what changed is repainted. */
void desk_status(const desk_status_t *st);

/** @brief The time of day; a negative @p hh is "not set". */
void desk_clock(int hh, int mm, int ss);

/** @brief The pointer to (x, y) on the desktop. */
void desk_pointer_abs(int x, int y);

/** @brief The pointer moved by (dx, dy). */
void desk_pointer_rel(int dx, int dy);

/** @brief The pointer's button went down or up. */
void desk_button(bool down);

/** @brief The wheel, in rows (positive is down). */
void desk_wheel(int rows);

/** @brief A key, as a browser names it ("a", "Enter", "Backspace", "ArrowUp", ...). */
void desk_key(const char *key);

/** @brief Where the pointer is. */
void desk_pointer(int *x, int *y);

/** @brief The next action, if any. */
bool desk_take_action(desk_act_t *a);

/** @brief The folder the file window shows. */
const char *desk_files_path(void);

/** @brief A listing starts at entry @p skip (0: a new listing). */
void desk_files_begin(int skip);

/** @brief One entry of the listing. */
void desk_files_entry(const char *name, uint32_t size, bool is_dir);

/** @brief The page ended; @p more asks for the next. @p ok false says the card failed. */
void desk_files_end(bool ok, bool more);

/** @brief The notes' text and length. */
const char *desk_notes(size_t *len);

/** @brief Load the notes (from the card at start). */
void desk_notes_set(const char *text, size_t len);

/** @brief Say how the last save went. */
void desk_notes_saved(bool ok);

#ifdef __cplusplus
}
#endif

#endif /* DESK_H_ */
