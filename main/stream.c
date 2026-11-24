/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file stream.c
 * @brief The Wi-Fi output.
 *
 * The stream task waits for the renderer to say something changed, copies the changed
 * rectangles out of the shared frame (holding its lock only for the copy), encodes each as
 * JPEG and sends it to every WebSocket viewer as one binary message: 'T', x, y, w, h (16-bit,
 * little-endian) and the JPEG; an 'F' message ends the frame. The MJPEG viewer gets the whole
 * frame instead. Frames are paced to STREAM_FPS_MAX; TCP's own back-pressure slows the task
 * further for a slow viewer.
 */
#include "stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "kode_log.h"

#include "fbshare.h"
#include "jpeg_enc.h"

#define TAG "dd.stream"

#define MAX_WS         4
#define MAX_RECTS      32
#define STREAM_FPS_MAX 30
#define TASK_STACK     6144
#define BOUNDARY       "ddframe"

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static const char TV_PAGE[] =
    "<!doctype html><html><head><meta name=viewport content='width=device-width'><title>Dot Desktop</title></head>"
    "<body style='margin:0;background:#000'><img src='/mjpeg' "
    "style='width:100vw;height:100vh;object-fit:contain'></body></html>";

typedef struct {
    int  fd;
    bool fresh; /**< needs the size and a whole frame */
} viewer_t;

static httpd_handle_t    s_http;
static TaskHandle_t      s_task;
static int               s_sink = -1;
static SemaphoreHandle_t s_lock; /**< the viewer list */
static viewer_t          s_ws[MAX_WS];
static int               s_nws;
static httpd_req_t      *s_mjpeg;
static bool              s_mjpeg_fresh;
static stream_input_fn   s_input;
static volatile int      s_quality = 70;
static volatile int      s_fps;
static volatile int      s_kbps;
static uint16_t         *s_stage;      /**< the changed rectangles, packed */
static uint16_t         *s_full;       /**< a whole frame, for MJPEG and snapshots */
static SemaphoreHandle_t s_jpg_lock;   /**< one encode (and its send) at a time */

/* ---- viewers ------------------------------------------------------------------ */

static void ws_add(int fd)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for ( int i = 0; s_nws > i; i++ )
    {
        if ( s_ws[i].fd == fd )
        {
            s_ws[i].fresh = true;
            xSemaphoreGive(s_lock);
            return;
        }
    }
    if ( MAX_WS > s_nws )
    {
        s_ws[s_nws].fd = fd;
        s_ws[s_nws].fresh = true;
        s_nws++;
        KLOG_I(TAG, "viewer %d joined (%d)", fd, s_nws);
    }
    xSemaphoreGive(s_lock);
    if ( s_task )
    {
        xTaskNotifyGive(s_task);
    }
}

static void ws_remove(int fd)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for ( int i = 0; s_nws > i; i++ )
    {
        if ( s_ws[i].fd == fd )
        {
            s_ws[i] = s_ws[--s_nws];
            KLOG_I(TAG, "viewer %d left (%d)", fd, s_nws);
            break;
        }
    }
    xSemaphoreGive(s_lock);
}

static void on_close(httpd_handle_t hd, int fd)
{
    (void)hd;
    ws_remove(fd);
    close(fd);
}

int stream_clients(void)
{
    return s_nws + (s_mjpeg ? 1 : 0);
}

void stream_stats(int *fps, int *kbps)
{
    *fps = s_fps;
    *kbps = s_kbps;
}

void stream_set_quality(int q)
{
    if ( 10 > q )
    {
        q = 10;
    }
    if ( 95 < q )
    {
        q = 95;
    }
    s_quality = q;
}

void stream_enable(bool on)
{
    fbs_sink_enable(s_sink, on);
    if ( on && s_task )
    {
        xTaskNotifyGive(s_task);
    }
}

/* ---- HTTP handlers -------------------------------------------------------------- */

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
}

static esp_err_t h_tv(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, TV_PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_snap(httpd_req_t *req)
{
    if ( !s_full )
    {
        s_full = heap_caps_malloc((size_t)FBS_MAX_W * FBS_MAX_H * 2u, MALLOC_CAP_SPIRAM);
        if ( !s_full )
        {
            return httpd_resp_send_500(req);
        }
    }
    xSemaphoreTake(s_jpg_lock, portMAX_DELAY);
    fbs_lock();
    const int w = fbs_w();
    const int h = fbs_h();
    memcpy(s_full, fbs_pixels(), (size_t)w * h * 2u);
    fbs_unlock();
    const uint8_t *jpg = NULL;
    size_t len = 0;
    esp_err_t err;
    if ( jpg_encode(s_full, w, w, h, 90, &jpg, &len) )
    {
        httpd_resp_set_type(req, "image/jpeg");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        err = httpd_resp_send(req, (const char *)jpg, (ssize_t)len);
    }
    else
    {
        err = httpd_resp_send_500(req);
    }
    xSemaphoreGive(s_jpg_lock);
    return err;
}

static esp_err_t h_mjpeg(httpd_req_t *req)
{
    if ( s_mjpeg )
    {
        httpd_resp_set_status(req, "503 Busy");
        return httpd_resp_send(req, "one MJPEG viewer at a time", HTTPD_RESP_USE_STRLEN);
    }
    httpd_req_t *copy = NULL;
    if ( ESP_OK != httpd_req_async_handler_begin(req, &copy) )
    {
        return ESP_FAIL;
    }
    httpd_resp_set_type(copy, "multipart/x-mixed-replace; boundary=" BOUNDARY);
    httpd_resp_set_hdr(copy, "Cache-Control", "no-store");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_mjpeg = copy;
    s_mjpeg_fresh = true;
    xSemaphoreGive(s_lock);
    KLOG_I(TAG, "MJPEG viewer joined");
    if ( s_task )
    {
        xTaskNotifyGive(s_task);
    }
    return ESP_OK;
}

static int clampi(int v, int lo, int hi)
{
    if ( v < lo )
    {
        return lo;
    }
    if ( v > hi )
    {
        return hi;
    }
    return v;
}

/** @brief One line from a viewer: "m x y b", "w rows" or "k key". */
static void viewer_line(char *msg)
{
    if ( !s_input || !msg[0] || (' ' != msg[1]) )
    {
        return;
    }
    char *arg = msg + 2;
    switch ( msg[0] )
    {
    case 'm': {
        int x = 0;
        int y = 0;
        int b = 0;
        if ( 3 == sscanf(arg, "%d %d %d", &x, &y, &b) )
        {
            s_input('m', clampi(x, 0, 4095), clampi(y, 0, 4095) | (b ? 0x10000 : 0), NULL);
        }
        break;
    }
    case 'w':
        s_input('w', clampi(atoi(arg), -20, 20), 0, NULL);
        break;
    case 'k':
        if ( 15 >= strlen(arg) )
        {
            s_input('k', 0, 0, arg);
        }
        break;
    default:
        break;
    }
}

/** @brief A viewer's handshake is done (ESP-IDF v6 no longer calls the handler for it). */
static esp_err_t ws_joined(httpd_req_t *req)
{
    ws_add(httpd_req_to_sockfd(req));
    return ESP_OK;
}

static esp_err_t h_ws(httpd_req_t *req)
{
    if ( HTTP_GET == req->method )
    {
        ws_add(httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    httpd_ws_frame_t f = { 0 };
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
    if ( ESP_OK != err )
    {
        return err;
    }
    if ( HTTPD_WS_TYPE_CLOSE == f.type )
    {
        ws_remove(httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    char buf[64];
    if ( (HTTPD_WS_TYPE_TEXT != f.type) || (0 == f.len) || (sizeof(buf) <= f.len) )
    {
        /* Drain what does not interest us. */
        uint8_t junk[64];
        while ( 0 < f.len )
        {
            size_t n = (sizeof(junk) < f.len) ? sizeof(junk) : f.len;
            f.payload = junk;
            httpd_ws_frame_t g = f;
            g.len = n;
            if ( ESP_OK != httpd_ws_recv_frame(req, &g, n) )
            {
                break;
            }
            f.len -= n;
        }
        return ESP_OK;
    }
    f.payload = (uint8_t *)buf;
    err = httpd_ws_recv_frame(req, &f, f.len);
    if ( ESP_OK != err )
    {
        return err;
    }
    buf[f.len] = 0;
    viewer_line(buf);
    return ESP_OK;
}

/* ---- sending -------------------------------------------------------------------- */

static bool ws_send(int fd, httpd_ws_type_t type, const void *data, size_t len, bool frag, bool final)
{
    httpd_ws_frame_t f = {
        .final = final,
        .fragmented = frag,
        .type = type,
        .payload = (uint8_t *)data,
        .len = len,
    };
    return ESP_OK == httpd_ws_send_frame_async(s_http, fd, &f);
}

/** @brief Send one rectangle to every viewer; a viewer whose send fails is dropped. */
static size_t send_rect(const int *fds, int n, bool *dead, const fbs_rect_t *r, const uint8_t *jpg, size_t len)
{
    uint8_t hdr[9] = { 'T',
                       (uint8_t)r->x, (uint8_t)(r->x >> 8), (uint8_t)r->y, (uint8_t)(r->y >> 8),
                       (uint8_t)r->w, (uint8_t)(r->w >> 8), (uint8_t)r->h, (uint8_t)(r->h >> 8) };
    size_t sent = 0;
    for ( int i = 0; n > i; i++ )
    {
        if ( dead[i] )
        {
            continue;
        }
        if ( !ws_send(fds[i], HTTPD_WS_TYPE_BINARY, hdr, sizeof(hdr), true, false)
             || !ws_send(fds[i], HTTPD_WS_TYPE_CONTINUE, jpg, len, true, true) )
        {
            dead[i] = true;
            continue;
        }
        sent += len + sizeof(hdr);
    }
    return sent;
}

static bool mjpeg_send(httpd_req_t *req, const uint8_t *jpg, size_t len)
{
    char hdr[96];
    const int n = snprintf(hdr, sizeof(hdr), "\r\n--" BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                           (unsigned)len);
    if ( ESP_OK != httpd_resp_send_chunk(req, hdr, n) )
    {
        return false;
    }
    return ESP_OK == httpd_resp_send_chunk(req, (const char *)jpg, (ssize_t)len);
}

static void stream_task(void *arg)
{
    (void)arg;
    static fbs_rect_t rects[MAX_RECTS];
    static size_t offs[MAX_RECTS];
    uint32_t gen = 0;
    int64_t last_frame = 0;
    int64_t window = esp_timer_get_time();
    int frames = 0;
    size_t bytes = 0;
    for ( ;; )
    {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        const int64_t now = esp_timer_get_time();
        if ( 1000000 <= now - window )
        {
            s_fps = frames;
            s_kbps = (int)(bytes / 1024u);
            frames = 0;
            bytes = 0;
            window = now;
        }
        if ( (0 == s_nws) && !s_mjpeg )
        {
            continue;
        }
        const int64_t gap = 1000000 / STREAM_FPS_MAX - (now - last_frame);
        if ( 0 < gap )
        {
            vTaskDelay(pdMS_TO_TICKS(gap / 1000 + 1));
        }
        last_frame = esp_timer_get_time();

        /* Who is watching, and who is new. */
        int fds[MAX_WS];
        bool fresh[MAX_WS];
        bool dead[MAX_WS];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const int n = s_nws;
        bool any_fresh = false;
        for ( int i = 0; n > i; i++ )
        {
            fds[i] = s_ws[i].fd;
            fresh[i] = s_ws[i].fresh;
            dead[i] = false;
            any_fresh = any_fresh || fresh[i];
            s_ws[i].fresh = false;
        }
        httpd_req_t *mj = s_mjpeg;
        bool mj_fresh = s_mjpeg_fresh;
        s_mjpeg_fresh = false;
        xSemaphoreGive(s_lock);

        /* Copy what changed out of the shared frame. */
        fbs_lock();
        const int w = fbs_w();
        const int h = fbs_h();
        bool resized = false;
        if ( fbs_generation() != gen )
        {
            gen = fbs_generation();
            resized = true;
        }
        if ( resized || any_fresh || mj_fresh )
        {
            fbs_mark_all(s_sink);
        }
        const int nr = fbs_take(s_sink, rects, MAX_RECTS);
        const uint16_t *fb = fbs_pixels();
        size_t off = 0;
        for ( int i = 0; nr > i; i++ )
        {
            offs[i] = off;
            for ( int y = 0; rects[i].h > y; y++ )
            {
                memcpy(s_stage + off, fb + (size_t)(rects[i].y + y) * w + rects[i].x, (size_t)rects[i].w * 2u);
                off += rects[i].w;
            }
        }
        if ( mj && (0 < nr) )
        {
            if ( !s_full )
            {
                s_full = heap_caps_malloc((size_t)FBS_MAX_W * FBS_MAX_H * 2u, MALLOC_CAP_SPIRAM);
            }
            if ( s_full )
            {
                memcpy(s_full, fb, (size_t)w * h * 2u);
            }
        }
        fbs_unlock();
        if ( 0 == nr )
        {
            continue;
        }

        xSemaphoreTake(s_jpg_lock, portMAX_DELAY);
        if ( resized || any_fresh )
        {
            char size[40];
            const int sl = snprintf(size, sizeof(size), "{\"size\":[%d,%d]}", w, h);
            for ( int i = 0; n > i; i++ )
            {
                if ( (resized || fresh[i]) && !ws_send(fds[i], HTTPD_WS_TYPE_TEXT, size, (size_t)sl, false, true) )
                {
                    dead[i] = true;
                }
            }
        }
        const int q = s_quality;
        for ( int i = 0; (nr > i) && (0 < n); i++ )
        {
            const uint8_t *jpg = NULL;
            size_t len = 0;
            if ( jpg_encode(s_stage + offs[i], rects[i].w, rects[i].w, rects[i].h, q, &jpg, &len) )
            {
                bytes += send_rect(fds, n, dead, &rects[i], jpg, len);
            }
            else
            {
                KLOG_W(TAG, "encode %ux%u failed", rects[i].w, rects[i].h);
            }
        }
        for ( int i = 0; n > i; i++ )
        {
            if ( !dead[i] && !ws_send(fds[i], HTTPD_WS_TYPE_BINARY, "F", 1, false, true) )
            {
                dead[i] = true;
            }
        }
        if ( mj && s_full )
        {
            const uint8_t *jpg = NULL;
            size_t len = 0;
            bool ok = jpg_encode(s_full, w, w, h, q, &jpg, &len) && mjpeg_send(mj, jpg, len);
            if ( ok )
            {
                bytes += len;
            }
            else
            {
                KLOG_I(TAG, "MJPEG viewer left");
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_mjpeg = NULL;
                xSemaphoreGive(s_lock);
                httpd_req_async_handler_complete(mj);
            }
        }
        xSemaphoreGive(s_jpg_lock);
        for ( int i = 0; n > i; i++ )
        {
            if ( dead[i] )
            {
                ws_remove(fds[i]);
                httpd_sess_trigger_close(s_http, fds[i]);
            }
        }
        frames++;
    }
}

/* ---- start ---------------------------------------------------------------------- */

bool stream_init(stream_input_fn input)
{
    s_input = input;
    s_lock = xSemaphoreCreateMutex();
    s_jpg_lock = xSemaphoreCreateMutex();
    s_stage = heap_caps_malloc((size_t)FBS_MAX_W * FBS_MAX_H * 2u, MALLOC_CAP_SPIRAM);
    if ( !s_lock || !s_jpg_lock || !s_stage || !jpg_init() )
    {
        KLOG_E(TAG, "out of memory");
        return false;
    }
    if ( pdPASS != xTaskCreatePinnedToCore(stream_task, "dd_stream", TASK_STACK, NULL, 4, &s_task, 1) )
    {
        return false;
    }
    s_sink = fbs_sink_add(s_task);
    return 0 <= s_sink;
}

bool stream_running(void)
{
    return NULL != s_http;
}

bool stream_start(void)
{
    if ( s_http )
    {
        return true;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_open_sockets = 7;
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;
    cfg.close_fn = on_close;
    cfg.max_uri_handlers = 8;
    if ( ESP_OK != httpd_start(&s_http, &cfg) )
    {
        KLOG_E(TAG, "the HTTP server would not start");
        s_http = NULL;
        return false;
    }
    static const httpd_uri_t URIS[] = {
        { .uri = "/", .method = HTTP_GET, .handler = h_index },
        { .uri = "/tv", .method = HTTP_GET, .handler = h_tv },
        { .uri = "/snap.jpg", .method = HTTP_GET, .handler = h_snap },
        { .uri = "/mjpeg", .method = HTTP_GET, .handler = h_mjpeg },
        { .uri = "/ws", .method = HTTP_GET, .handler = h_ws, .is_websocket = true,
          .ws_post_handshake_cb = ws_joined },
    };
    for ( size_t i = 0; sizeof(URIS) / sizeof(URIS[0]) > i; i++ )
    {
        httpd_register_uri_handler(s_http, &URIS[i]);
    }
    KLOG_I(TAG, "serving on port 80 (%s JPEG)", jpg_is_hw() ? "hardware" : "software");
    return true;
}
