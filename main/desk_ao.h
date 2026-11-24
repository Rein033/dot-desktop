/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file desk_ao.h
 * @brief The desktop's Active Object: input, status, the frame loop and the Dot's own glass.
 */
#ifndef DESK_AO_H_
#define DESK_AO_H_

#include "kode_sys.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What was kept from the last run: the desktop's size and the stream's quality. */
typedef struct {
    int res;      /**< desk_res_t */
    int quality;
} desk_cfg_t;

/** @brief The desktop's Active Object. */
extern QActive * const AO_Desk;

/** @brief Construct it with the kept settings. */
void DeskAo_ctor(const desk_cfg_t *cfg);

/** @brief Start it. */
void DeskAo_start(void);

/** @brief Input from a viewer (any task): 'm' (a = x, b = y | 0x10000 when pressed), 'w' (a = rows), 'k' (key). */
void DeskAo_input(char kind, int a, int b, const char *key);

#ifdef __cplusplus
}
#endif

#endif /* DESK_AO_H_ */
