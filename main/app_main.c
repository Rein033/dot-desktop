/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file app_main.c
 * @brief Dot Desktop: a large desktop rendered on the Dot and shown in a browser over Wi-Fi.
 *
 * kode_init() brings kodeOS up; app_main (a task of its own, so kode.h may block here) reads the
 * kept settings, starts the stream and the desktop's Active Object (desk_ao.c).
 * Any reset returns to kodeOS (CONFIG_KODEOS_BOOT_ROLE_APP).
 */
#include <stdlib.h>

#include "kode.h"

#include "desk.h"
#include "desk_ao.h"
#include "fbshare.h"
#include "stream.h"

KODE_APP_MANIFEST("Dot Desktop", "tools");

#define TAG "dd.main"

static int setting(const char *key, int def, int lo, int hi)
{
    const char *v = kode_setting_get(key);
    if ( !v || !v[0] )
    {
        return def;
    }
    const int n = atoi(v);
    if ( (n < lo) || (n > hi) )
    {
        return def;
    }
    return n;
}

void app_main(void)
{
    kode_init();
    desk_cfg_t cfg = {
        .res = setting("ddesk_res", DESK_RES_1280, 0, DESK_RES_COUNT - 1),
        .quality = setting("ddesk_q", 70, 10, 95),
    };
    KLOG_I(TAG, "output %dx%d, quality %d", desk_res_w(cfg.res), desk_res_h(cfg.res), cfg.quality);
    if ( !fbs_init() || !stream_init(DeskAo_input) )
    {
        KLOG_E(TAG, "out of memory at start");
    }
    DeskAo_ctor(&cfg);
    DeskAo_start();
}
