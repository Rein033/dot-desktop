/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file sw_jpeg.c
 * @brief Baseline JPEG, 4:4:4, the standard tables of ITU-T T.81 Annex K and the AAN DCT.
 */
#include "sw_jpeg.h"

#include <string.h>

static const uint8_t ZIGZAG[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

static const uint8_t Q_LUM[64] = {
    16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
    14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
    18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99,
};

static const uint8_t Q_CHR[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
};

static const uint8_t DC_LUM_BITS[16] = { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t DC_CHR_BITS[16] = { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const uint8_t DC_VALS[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };

static const uint8_t AC_LUM_BITS[16] = { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const uint8_t AC_LUM_VALS[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};

static const uint8_t AC_CHR_BITS[16] = { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
static const uint8_t AC_CHR_VALS[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};

/** @brief One Huffman table as codes and lengths, indexed by symbol. */
typedef struct {
    uint16_t code[256];
    uint8_t  len[256];
} huff_t;

typedef struct {
    uint8_t *out;
    size_t   cap;
    size_t   n;
    uint32_t acc;
    int      bits;
    int      over;
} bw_t;

static void put8(bw_t *b, uint8_t v)
{
    if ( b->n < b->cap )
    {
        b->out[b->n++] = v;
    }
    else
    {
        b->over = 1;
    }
}

static void put16(bw_t *b, uint16_t v)
{
    put8(b, (uint8_t)(v >> 8));
    put8(b, (uint8_t)v);
}

static void putbits(bw_t *b, uint32_t code, int len)
{
    b->acc = (b->acc << len) | (code & ((1u << len) - 1u));
    b->bits += len;
    while ( 8 <= b->bits )
    {
        const uint8_t c = (uint8_t)(b->acc >> (b->bits - 8));
        put8(b, c);
        if ( 0xFF == c )
        {
            put8(b, 0);
        }
        b->bits -= 8;
    }
}

static void build(huff_t *h, const uint8_t *bits, const uint8_t *vals)
{
    uint16_t code = 0;
    int k = 0;
    memset(h, 0, sizeof(*h));
    for ( int l = 1; 16 >= l; l++ )
    {
        for ( int i = 0; bits[l - 1] > i; i++ )
        {
            h->code[vals[k]] = code;
            h->len[vals[k]] = (uint8_t)l;
            k++;
            code++;
        }
        code <<= 1;
    }
}

static void dct8(float *d, int s)
{
    float t0 = d[0] + d[7 * s];
    float t7 = d[0] - d[7 * s];
    float t1 = d[s] + d[6 * s];
    float t6 = d[s] - d[6 * s];
    float t2 = d[2 * s] + d[5 * s];
    float t5 = d[2 * s] - d[5 * s];
    float t3 = d[3 * s] + d[4 * s];
    float t4 = d[3 * s] - d[4 * s];

    float t10 = t0 + t3;
    float t13 = t0 - t3;
    float t11 = t1 + t2;
    float t12 = t1 - t2;
    d[0] = t10 + t11;
    d[4 * s] = t10 - t11;
    float z1 = (t12 + t13) * 0.707106781f;
    d[2 * s] = t13 + z1;
    d[6 * s] = t13 - z1;

    t10 = t4 + t5;
    t11 = t5 + t6;
    t12 = t6 + t7;
    float z5 = (t10 - t12) * 0.382683433f;
    float z2 = t10 * 0.541196100f + z5;
    float z4 = t12 * 1.306562965f + z5;
    float z3 = t11 * 0.707106781f;
    float z11 = t7 + z3;
    float z13 = t7 - z3;
    d[5 * s] = z13 + z2;
    d[3 * s] = z13 - z2;
    d[s] = z11 + z4;
    d[7 * s] = z11 - z4;
}

static int bitlen(int v)
{
    if ( 0 > v )
    {
        v = -v;
    }
    int n = 0;
    while ( v )
    {
        n++;
        v >>= 1;
    }
    return n;
}

static int block(bw_t *b, float *blk, const float *fq, int dc_prev, const huff_t *dc, const huff_t *ac)
{
    for ( int r = 0; 8 > r; r++ )
    {
        dct8(blk + r * 8, 1);
    }
    for ( int c = 0; 8 > c; c++ )
    {
        dct8(blk + c, 8);
    }
    int q[64];
    for ( int i = 0; 64 > i; i++ )
    {
        const float v = blk[ZIGZAG[i]] * fq[i];
        q[i] = (int)(v < 0 ? v - 0.5f : v + 0.5f);
    }
    const int diff = q[0] - dc_prev;
    int n = bitlen(diff);
    putbits(b, dc->code[n], dc->len[n]);
    if ( n )
    {
        putbits(b, (uint32_t)(0 > diff ? diff - 1 : diff), n);
    }
    int last = 63;
    while ( (0 < last) && (0 == q[last]) )
    {
        last--;
    }
    int run = 0;
    for ( int i = 1; last >= i; i++ )
    {
        if ( 0 == q[i] )
        {
            run++;
            continue;
        }
        while ( 16 <= run )
        {
            putbits(b, ac->code[0xF0], ac->len[0xF0]);
            run -= 16;
        }
        n = bitlen(q[i]);
        const int sym = (run << 4) | n;
        putbits(b, ac->code[sym], ac->len[sym]);
        putbits(b, (uint32_t)(0 > q[i] ? q[i] - 1 : q[i]), n);
        run = 0;
    }
    if ( 63 != last )
    {
        putbits(b, ac->code[0], ac->len[0]);
    }
    return q[0];
}

static void dqt(bw_t *b, uint8_t id, const uint8_t *base, int scale, float *fq)
{
    static const float AASF[8] = {
        1.0f, 1.387039845f, 1.306562965f, 1.175875602f, 1.0f, 0.785694958f, 0.541196100f, 0.275899379f,
    };
    uint8_t t[64];
    for ( int i = 0; 64 > i; i++ )
    {
        int v = (base[i] * scale + 50) / 100;
        if ( 1 > v )
        {
            v = 1;
        }
        if ( 255 < v )
        {
            v = 255;
        }
        t[i] = (uint8_t)v;
    }
    put16(b, 0xFFDB);
    put16(b, 67);
    put8(b, id);
    for ( int i = 0; 64 > i; i++ )
    {
        put8(b, t[ZIGZAG[i]]);
    }
    for ( int i = 0; 64 > i; i++ )
    {
        const int k = ZIGZAG[i];
        fq[i] = 1.0f / ((float)t[k] * AASF[k >> 3] * AASF[k & 7] * 8.0f);
    }
}

static void dht(bw_t *b, uint8_t cls_id, const uint8_t *bits, const uint8_t *vals, int nvals)
{
    put16(b, 0xFFC4);
    put16(b, (uint16_t)(3 + 16 + nvals));
    put8(b, cls_id);
    for ( int i = 0; 16 > i; i++ )
    {
        put8(b, bits[i]);
    }
    for ( int i = 0; nvals > i; i++ )
    {
        put8(b, vals[i]);
    }
}

size_t sw_jpeg_encode(const uint16_t *px, int stride, int w, int h, int quality, uint8_t *out, size_t cap)
{
    static huff_t s_dcl, s_dcc, s_acl, s_acc;
    static int s_ready;
    if ( !px || !out || (1 > w) || (1 > h) || (65535 < w) || (65535 < h) || (1 > quality) || (100 < quality) )
    {
        return 0;
    }
    if ( !s_ready )
    {
        build(&s_dcl, DC_LUM_BITS, DC_VALS);
        build(&s_dcc, DC_CHR_BITS, DC_VALS);
        build(&s_acl, AC_LUM_BITS, AC_LUM_VALS);
        build(&s_acc, AC_CHR_BITS, AC_CHR_VALS);
        s_ready = 1;
    }
    int scale = (50 > quality) ? (5000 / quality) : (200 - quality * 2);
    bw_t b = { out, cap, 0, 0, 0, 0 };
    float fql[64];
    float fqc[64];

    put16(&b, 0xFFD8);
    dqt(&b, 0, Q_LUM, scale, fql);
    dqt(&b, 1, Q_CHR, scale, fqc);
    put16(&b, 0xFFC0); /* SOF0, 3 components, 4:4:4 */
    put16(&b, 17);
    put8(&b, 8);
    put16(&b, (uint16_t)h);
    put16(&b, (uint16_t)w);
    put8(&b, 3);
    put8(&b, 1);
    put8(&b, 0x11);
    put8(&b, 0);
    put8(&b, 2);
    put8(&b, 0x11);
    put8(&b, 1);
    put8(&b, 3);
    put8(&b, 0x11);
    put8(&b, 1);
    dht(&b, 0x00, DC_LUM_BITS, DC_VALS, 12);
    dht(&b, 0x10, AC_LUM_BITS, AC_LUM_VALS, 162);
    dht(&b, 0x01, DC_CHR_BITS, DC_VALS, 12);
    dht(&b, 0x11, AC_CHR_BITS, AC_CHR_VALS, 162);
    put16(&b, 0xFFDA); /* SOS */
    put16(&b, 12);
    put8(&b, 3);
    put8(&b, 1);
    put8(&b, 0x00);
    put8(&b, 2);
    put8(&b, 0x11);
    put8(&b, 3);
    put8(&b, 0x11);
    put8(&b, 0);
    put8(&b, 63);
    put8(&b, 0);

    int py = 0;
    int pu = 0;
    int pv = 0;
    float Y[64];
    float U[64];
    float V[64];
    for ( int by = 0; h > by; by += 8 )
    {
        for ( int bx = 0; w > bx; bx += 8 )
        {
            for ( int r = 0; 8 > r; r++ )
            {
                int yy = by + r;
                if ( yy >= h )
                {
                    yy = h - 1;
                }
                const uint16_t *row = px + (size_t)yy * stride;
                for ( int c = 0; 8 > c; c++ )
                {
                    int xx = bx + c;
                    if ( xx >= w )
                    {
                        xx = w - 1;
                    }
                    const uint16_t p = row[xx];
                    const float R = (float)(((((p >> 11) & 0x1F) * 527) + 23) >> 6);
                    const float G = (float)(((((p >> 5) & 0x3F) * 259) + 33) >> 6);
                    const float B = (float)((((p & 0x1F) * 527) + 23) >> 6);
                    const int k = r * 8 + c;
                    Y[k] = 0.299f * R + 0.587f * G + 0.114f * B - 128.0f;
                    U[k] = -0.168736f * R - 0.331264f * G + 0.5f * B;
                    V[k] = 0.5f * R - 0.418688f * G - 0.081312f * B;
                }
            }
            py = block(&b, Y, fql, py, &s_dcl, &s_acl);
            pu = block(&b, U, fqc, pu, &s_dcc, &s_acc);
            pv = block(&b, V, fqc, pv, &s_dcc, &s_acc);
            if ( b.over )
            {
                return 0;
            }
        }
    }
    if ( b.bits )
    {
        putbits(&b, 0x7F, 8 - b.bits); /* pad the last byte with ones */
    }
    put16(&b, 0xFFD9);
    if ( b.over )
    {
        return 0;
    }
    return b.n;
}
