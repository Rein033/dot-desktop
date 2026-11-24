/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file jpeg_enc.c
 * @brief The P4's JPEG engine, after kode-sdk's kdrv_jpeg.c, with a software fallback.
 *
 * kdrv_jpeg.c is compiled only with CONFIG_KODEOS_MIRROR, which an app leaves off (it costs
 * 530 KB of PSRAM from boot), and its output buffer (128 KB) is too small for a 720p frame. So
 * this file opens the engine itself, the same way: ESP-IDF's driver shares the one codec among
 * its handles. A frame here is a rectangle of a larger frame, so it is first copied into a
 * contiguous buffer the engine can read.
 */
#include "jpeg_enc.h"

#include <string.h>

#include "driver/jpeg_encode.h"
#include "esp_heap_caps.h"
#include "kode_log.h"

#include "fbshare.h"
#include "sw_jpeg.h"

#define TAG "dd.jpeg"

#define OUT_CAP    (512u * 1024u)
#define TIMEOUT_MS 500

static jpeg_encoder_handle_t s_enc;
static uint8_t              *s_in;
static size_t                s_in_cap;
static uint8_t              *s_out;
static size_t                s_out_cap;
static bool                  s_hw;

bool jpg_init(void)
{
    if ( s_out )
    {
        return true;
    }
    jpeg_encode_engine_cfg_t const cfg = { .timeout_ms = TIMEOUT_MS };
    if ( ESP_OK == jpeg_new_encoder_engine(&cfg, &s_enc) )
    {
        jpeg_encode_memory_alloc_cfg_t const in_cfg = { .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER };
        jpeg_encode_memory_alloc_cfg_t const out_cfg = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
        s_in = jpeg_alloc_encoder_mem((size_t)FBS_MAX_W * FBS_MAX_H * 2u, &in_cfg, &s_in_cap);
        s_out = jpeg_alloc_encoder_mem(OUT_CAP, &out_cfg, &s_out_cap);
        if ( s_in && s_out )
        {
            s_hw = true;
            KLOG_I(TAG, "hardware encoder, %u KB out", (unsigned)(s_out_cap / 1024u));
            return true;
        }
        KLOG_W(TAG, "no room for the engine's buffers; software instead");
        (void)jpeg_del_encoder_engine(s_enc);
        s_enc = NULL;
        if ( s_in )
        {
            heap_caps_free(s_in);
            s_in = NULL;
        }
        if ( s_out )
        {
            heap_caps_free(s_out);
            s_out = NULL;
        }
    }
    else
    {
        KLOG_W(TAG, "no JPEG engine; software encoder instead");
    }
    s_out = heap_caps_malloc(OUT_CAP, MALLOC_CAP_SPIRAM);
    s_out_cap = OUT_CAP;
    return NULL != s_out;
}

bool jpg_is_hw(void)
{
    return s_hw;
}

static bool encode_hw(const uint16_t *px, int stride, int w, int h, int quality, size_t *len)
{
    const size_t row = (size_t)w * 2u;
    if ( (size_t)h * row > s_in_cap )
    {
        return false;
    }
    for ( int y = 0; h > y; y++ )
    {
        memcpy(s_in + (size_t)y * row, px + (size_t)y * stride, row);
    }
    jpeg_encode_cfg_t const cfg = {
        .width = (uint32_t)w,
        .height = (uint32_t)h,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = (uint32_t)quality,
    };
    uint32_t n = 0;
    const esp_err_t err = jpeg_encoder_process(s_enc, &cfg, s_in, (uint32_t)((size_t)h * row), s_out,
                                               (uint32_t)s_out_cap, &n);
    if ( ESP_OK != err )
    {
        KLOG_W(TAG, "engine refused %dx%d: %s", w, h, esp_err_to_name(err));
        return false;
    }
    *len = n;
    return true;
}

bool jpg_encode(const uint16_t *px, int stride, int w, int h, int quality, const uint8_t **out, size_t *len)
{
    if ( !s_out || !px || (1 > w) || (1 > h) )
    {
        return false;
    }
    if ( 1 > quality )
    {
        quality = 1;
    }
    if ( 100 < quality )
    {
        quality = 100;
    }
    if ( s_hw )
    {
        if ( encode_hw(px, stride, w, h, quality, len) )
        {
            *out = s_out;
            return true;
        }
        if ( (16 <= w) && (16 <= h) )
        {
            /* An engine that fails a plain rectangle is not one to keep asking. */
            KLOG_W(TAG, "switching to the software encoder");
            s_hw = false;
        }
    }
    const size_t n = sw_jpeg_encode(px, stride, w, h, quality, s_out, s_out_cap);
    if ( !n )
    {
        return false;
    }
    *out = s_out;
    *len = n;
    return true;
}
