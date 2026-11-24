/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fonts_aa.h
 * @brief Anti-aliased bitmap fonts (Outfit), generated into fonts_aa.c by tools/mkfont_ttf.py.
 */
#ifndef FONTS_AA_H_
#define FONTS_AA_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief One glyph: where its bits start, the pen's advance, its box and its offset from the
 *         pen at the top of the line. */
typedef struct {
    uint32_t off;
    uint8_t  adv;
    uint8_t  w;
    uint8_t  h;
    int8_t   x;
    int8_t   y;
} aa_glyph_t;

/** @brief A font of ASCII 32..126: 4-bit alpha, two pixels a byte, rows padded to a byte. */
typedef struct {
    const uint8_t    *bits;
    const aa_glyph_t *glyph;
    uint8_t           line_h;
    uint8_t           ascent;
} aa_font_t;

extern const aa_font_t font_ui;    /**< Outfit Medium 17 px: text */
extern const aa_font_t font_bold;  /**< Outfit SemiBold 19 px: titles, pills */
extern const aa_font_t font_title; /**< Outfit Bold 26 px: headings */
extern const aa_font_t font_big;   /**< Outfit ExtraBold 56 px: the clock */

#ifdef __cplusplus
}
#endif

#endif /* FONTS_AA_H_ */
