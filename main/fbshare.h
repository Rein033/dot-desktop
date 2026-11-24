/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fbshare.h
 * @brief The desktop's framebuffer, shared by the renderer and the outputs ("sinks").
 *
 * One RGB565 frame in PSRAM, one mutex. The renderer (the desktop's Active Object) only ever
 * try-locks it, so it never blocks; the outputs (the Wi-Fi stream task) lock
 * it for as long as a copy of their dirty rectangles takes. Each output has its own mask of
 * dirty 16 x 16 tiles, so a slow output does not hold back a fast one.
 */
#ifndef FBSHARE_H_
#define FBSHARE_H_

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FBS_TILE      16
#define FBS_MAX_W     1280
#define FBS_MAX_H     720
#define FBS_MAX_SINKS 3

/** @brief One rectangle to send, in pixels. */
typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
} fbs_rect_t;

/** @brief Allocate the frame (FBS_MAX_W x FBS_MAX_H) and the mutex. Call once, before any task. */
bool fbs_init(void);

/** @brief Take the lock without waiting (for the Active Object). */
bool fbs_try_lock(void);

/** @brief Take the lock, waiting (for the output tasks only). */
void fbs_lock(void);

/** @brief Give the lock back. */
void fbs_unlock(void);

/** @brief The pixels; the row stride is fbs_w(). Read or written only with the lock held. */
uint16_t *fbs_pixels(void);

/** @brief The current width and height. */
int fbs_w(void);
int fbs_h(void);

/** @brief Changed by every fbs_configure(); an output that sees a new one starts over. */
uint32_t fbs_generation(void);

/**
 * @brief Set a new size (lock held). Every output's mask becomes "all dirty".
 * @return false for a size over FBS_MAX_W x FBS_MAX_H.
 */
bool fbs_configure(int w, int h);

/** @brief Register an output task, woken by fbs_signal(). @return Its id, or -1. */
int fbs_sink_add(TaskHandle_t task);

/** @brief Turn an output on or off; off, it is neither marked nor woken. */
void fbs_sink_enable(int sink, bool on);

/** @brief Mark a rectangle dirty for every enabled output (lock held). */
void fbs_mark(gfx_rect_t r);

/** @brief Mark the whole frame dirty for one output (lock held). */
void fbs_mark_all(int sink);

/** @brief Wake every enabled output that has something dirty (lock not needed). */
void fbs_signal(void);

/**
 * @brief Turn an output's dirty tiles into rectangles and clear them (lock held).
 *
 * Runs of tiles on a row become one rectangle, and identical runs on the rows below join it.
 * When there would be more than @p max, the remainder is folded into one band to the bottom.
 *
 * @return How many rectangles were written to @p out.
 */
int fbs_take(int sink, fbs_rect_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* FBSHARE_H_ */
