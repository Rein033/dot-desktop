/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file icons_aa.h
 * @brief The desktop's icons, generated into icons_aa.c by tools/mkicons.py.
 */
#ifndef ICONS_AA_H_
#define ICONS_AA_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief An icon: 4-bit alpha, two pixels a byte, @c w even. */
typedef struct {
    const uint8_t *bits;
    uint8_t        w;
    uint8_t        h;
} aa_icon_t;

extern const aa_icon_t icon_files;   /**< a folder */
extern const aa_icon_t icon_system;  /**< a chip */
extern const aa_icon_t icon_notes;   /**< a notepad and a pencil */
extern const aa_icon_t icon_display; /**< a monitor */

#ifdef __cplusplus
}
#endif

#endif /* ICONS_AA_H_ */
