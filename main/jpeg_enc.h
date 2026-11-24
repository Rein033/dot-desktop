/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file jpeg_enc.h
 * @brief JPEG for the stream: the P4's encoder engine, or sw_jpeg.c when the engine is absent.
 *
 * Called only from the stream task (and the snapshot handler, under the same guard), never from
 * an Active Object: an encode waits on the engine.
 */
#ifndef JPEG_ENC_H_
#define JPEG_ENC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Allocate the engine (or settle for software) and the buffers. @return false when out of memory. */
bool jpg_init(void);

/** @brief Whether the hardware engine is in use. */
bool jpg_is_hw(void);

/**
 * @brief Encode a rectangle of an RGB565 frame.
 * @param px      Its top-left pixel.
 * @param stride  Pixels from one row to the next.
 * @param w       Width.
 * @param h       Height.
 * @param quality 1..100.
 * @param out     Receives the file; valid until the next call.
 * @param len     Receives its length.
 * @return true on success.
 */
bool jpg_encode(const uint16_t *px, int stride, int w, int h, int quality, const uint8_t **out, size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* JPEG_ENC_H_ */
