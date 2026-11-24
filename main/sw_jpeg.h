/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file sw_jpeg.h
 * @brief A small baseline JPEG encoder in software (4:4:4), for when the P4's engine is absent.
 *
 * Pure C: no SDK, so it is tested on the host (test/sw_jpeg_test.c).
 */
#ifndef SW_JPEG_H_
#define SW_JPEG_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Encode a rectangle of RGB565 (native byte order) as a baseline JPEG.
 * @param px      The rectangle's top-left pixel.
 * @param stride  Pixels from one row to the next.
 * @param w       Width, 1..65535.
 * @param h       Height, 1..65535.
 * @param quality 1..100.
 * @param out     Where the file goes.
 * @param cap     Its room in bytes.
 * @return The file's length, or 0 when it did not fit or an argument was wrong.
 */
size_t sw_jpeg_encode(const uint16_t *px, int stride, int w, int h, int quality, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* SW_JPEG_H_ */
