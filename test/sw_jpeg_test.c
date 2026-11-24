/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host test: write a test pattern through sw_jpeg_encode() to the file in argv[1]. */
#include <stdio.h>
#include <stdlib.h>
#include "sw_jpeg.h"
int main(int argc, char **argv)
{
    const int W = 203, H = 117, S = 256;
    uint16_t *px = calloc((size_t)S * H, 2);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int r = x * 255 / W, g = y * 255 / H, b = ((x / 20 + y / 20) & 1) ? 255 : 0;
            px[y * S + x] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        }
    static uint8_t out[1 << 20];
    size_t n = sw_jpeg_encode(px, S, W, H, 80, out, sizeof out);
    FILE *f = fopen(argv[1], "wb"); fwrite(out, 1, n, f); fclose(f);
    printf("%zu bytes\n", n);
    FILE *r = fopen(argv[2], "wb"); /* raw RGB888 reference */
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint16_t p = px[y * S + x];
        unsigned char c[3] = { (unsigned char)((((p >> 11) & 31) * 527 + 23) >> 6), (unsigned char)((((p >> 5) & 63) * 259 + 33) >> 6), (unsigned char)(((p & 31) * 527 + 23) >> 6) };
        fwrite(c, 1, 3, r);
    }
    fclose(r);
    return n ? 0 : 1;
}
