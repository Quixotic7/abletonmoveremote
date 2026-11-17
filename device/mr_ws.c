/* SPDX-License-Identifier: MPL-2.0 */
/* What the WebSocket handshake needs: SHA-1 (RFC 3174) and base64, for Sec-WebSocket-Accept. */
#include "mr_ws.h"

#include <stdio.h>
#include <string.h>

typedef struct { uint32_t h[5]; uint64_t len; uint8_t blk[64]; int n; } sha1_t;

static uint32_t rol(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }

static void sha1_block(sha1_t *c, const uint8_t *p)
{
    uint32_t w[80], a, b, d, e, f, k, t, cc;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (i = 0; i < 80; i++) {
        if (i < 20)
            f = (b & cc) | (~b & d), k = 0x5A827999;
        else if (i < 40)
            f = b ^ cc ^ d, k = 0x6ED9EBA1;
        else if (i < 60)
            f = (b & cc) | (b & d) | (cc & d), k = 0x8F1BBCDC;
        else
            f = b ^ cc ^ d, k = 0xCA62C1D6;
        t = rol(a, 5) + f + e + k + w[i];
        e = d, d = cc, cc = rol(b, 30), b = a, a = t;
    }
    c->h[0] += a, c->h[1] += b, c->h[2] += cc, c->h[3] += d, c->h[4] += e;
}

static void sha1(const uint8_t *p, size_t n, uint8_t out[20])
{
    sha1_t c = { { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 }, 0, { 0 }, 0 };
    uint8_t pad[72];
    size_t i, padn;
    uint64_t bits = (uint64_t)n * 8;
    for (i = 0; i + 64 <= n; i += 64)
        sha1_block(&c, p + i);
    memcpy(c.blk, p + i, n - i);
    c.n = (int)(n - i);
    padn = (size_t)(c.n < 56 ? 56 - c.n : 120 - c.n);
    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    for (i = 0; i < 8; i++)
        pad[padn + i] = (uint8_t)(bits >> (56 - 8 * i));
    {
        uint8_t tail[128];
        memcpy(tail, c.blk, (size_t)c.n);
        memcpy(tail + c.n, pad, padn + 8);
        sha1_block(&c, tail);
        if (c.n + padn + 8 > 64)
            sha1_block(&c, tail + 64);
    }
    for (i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(c.h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c.h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c.h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)c.h[i];
    }
}

static void base64(const uint8_t *p, size_t n, char *out)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i;
    for (i = 0; i + 2 < n; i += 3) {
        *out++ = T[p[i] >> 2];
        *out++ = T[((p[i] & 3) << 4) | (p[i + 1] >> 4)];
        *out++ = T[((p[i + 1] & 15) << 2) | (p[i + 2] >> 6)];
        *out++ = T[p[i + 2] & 63];
    }
    if (n - i == 1) {
        *out++ = T[p[i] >> 2];
        *out++ = T[(p[i] & 3) << 4];
        *out++ = '=';
        *out++ = '=';
    } else if (n - i == 2) {
        *out++ = T[p[i] >> 2];
        *out++ = T[((p[i] & 3) << 4) | (p[i + 1] >> 4)];
        *out++ = T[(p[i + 1] & 15) << 2];
        *out++ = '=';
    }
    *out = 0;
}

void mr_ws_accept(const char *key, char out[32])
{
    char buf[128];
    uint8_t dig[20];
    const int n = snprintf(buf, sizeof buf, "%.60s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    sha1((const uint8_t *)buf, (size_t)n, dig);
    base64(dig, 20, out);
}
