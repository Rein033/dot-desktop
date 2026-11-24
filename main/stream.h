/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file stream.h
 * @brief The Wi-Fi output: an HTTP server on port 80 with the viewer page, a WebSocket that
 *        carries dirty rectangles as JPEG one way and the mouse and keyboard the other, an
 *        MJPEG stream for simple viewers and a snapshot.
 *
 * The server and the stream run on tasks of their own; nothing here runs on an Active Object.
 */
#ifndef STREAM_H_
#define STREAM_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Input from a viewer, handed to the desktop's Active Object. */
typedef void (*stream_input_fn)(char kind, int a, int b, const char *key);

/** @brief Create the stream task and register it as an output (once, at start). */
bool stream_init(stream_input_fn input);

/** @brief Start the HTTP server (when the network is up). Idempotent. */
bool stream_start(void);

/** @brief Whether the server runs. */
bool stream_running(void);

/** @brief Turn the stream on or off as an output of the shared frame. */
void stream_enable(bool on);

/** @brief The JPEG quality, 10..95. */
void stream_set_quality(int q);

/** @brief Viewers connected (WebSocket and MJPEG). */
int stream_clients(void);

/** @brief Frames and kilobytes sent in the last second. */
void stream_stats(int *fps, int *kbps);

#ifdef __cplusplus
}
#endif

#endif /* STREAM_H_ */
