/* SPDX-License-Identifier: MPL-2.0 */
/* The USB-A embedding hooks (mr_core.h mr_set_usba_in ...) on the device core alone, its rt thread's calls made by
 * hand: cable-2 input reaches the embedder's callback (not the host), a SysEx sent comes out whole on cable 2, at
 * most the cap a transfer, the LED writes still go every transfer, and a full queue refuses a message whole. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../device/mr_core.h"
#include "../include/moveremote/proto.h"

void mr_plat_frame(const uint8_t px[1024]) { (void)px; }

static uint8_t g_got[4096][4];
static int16_t g_ain[MR_BLOCK * 2];
static int g_ain_calls, g_ain_frames;
static void on_ain(void *ctx, const int16_t *in, int frames)
{
    (void)ctx;
    memcpy(g_ain, in, sizeof g_ain);
    g_ain_calls++;
    g_ain_frames = frames;
}
static int g_ngot, g_fails;

static void on_usba(void *ctx, const uint8_t (*p)[4], int n)
{
    (void)ctx;
    while (n-- > 0 && g_ngot < 4096)
        memcpy(g_got[g_ngot++], *p++, 4);
}

#define CHECK(c, ...)                                                                                               \
    do {                                                                                                            \
        if (c)                                                                                                      \
            printf("  ok    " __VA_ARGS__);                                                                         \
        else {                                                                                                      \
            printf("  FAIL  " __VA_ARGS__);                                                                         \
            g_fails++;                                                                                              \
        }                                                                                                           \
        printf("\n");                                                                                               \
    } while (0)

int main(void)
{
    char dir[] = "/tmp/mr-usba-XXXXXX";
    mr_cfg_t cfg = { NULL, 1, 0 };
    uint8_t out[MR_OUT_MAX][4], in[3 * 8], msg[302], back[400];
    int16_t aout[MR_BLOCK * 2];
    int nout, t, i, k, nback = 0, max_usba = 0, led_transfers = 0, transfers = 0, rc;
    if (!mkdtemp(dir))
        return 1;
    cfg.test_dir = dir;
    mr_set_port(-1);
    mr_set_usba_in(on_usba, NULL);
    mr_set_usba_cap(8);
    if (mr_init(&cfg) != 0)
        return 1;

    /* in: a note on cable 2 and a pad on cable 0, in one transfer */
    memset(in, 0, sizeof in);
    memcpy(in, (const uint8_t[]){ 0x29, 0x90, 60, 100 }, 4);
    memcpy(in + 8, (const uint8_t[]){ 0x09, 0x90, 68, 100 }, 4);
    mr_rt_block(in, 2, NULL, aout, out, &nout);
    k = mr_usba_poll();
    CHECK(k == 1 && g_ngot == 1 && !memcmp(g_got[0], (const uint8_t[]){ 0x29, 0x90, 60, 100 }, 4),
          "cable 2 in reaches the embedder (%d packets)", k);

    /* out: a 302-byte SysEx while every LED is dirty (mr_init turned them all off) */
    msg[0] = 0xF0;
    for (i = 1; i < 301; i++)
        msg[i] = (uint8_t)(i & 0x7F);
    msg[301] = 0xF7;
    rc = mr_usba_send(msg, sizeof msg);
    CHECK(rc == 101, "a 302-byte SysEx queued as %d packets", rc);
    CHECK(mr_usba_send(msg, 5) == 0, "an unterminated SysEx is nothing");
    for (t = 0; t < 200 && (mr_usba_queued() || t < 20); t++) {
        int nu = 0, nl = 0;
        mr_step(0);   /* feeds the LEDs */
        mr_rt_block(NULL, 0, NULL, aout, out, &nout);
        transfers++;
        for (i = 0; i < nout; i++) {
            if (out[i][0] >> 4 == 2) {
                const int cin = out[i][0] & 0x0F, len = cin == 0x4 || cin == 0x7 ? 3 : cin == 0x6 ? 2 : 1;
                nu++;
                for (k = 0; k < len && nback < (int)sizeof back; k++)
                    back[nback++] = out[i][1 + k];
            } else
                nl++;
        }
        if (nu > max_usba)
            max_usba = nu;
        led_transfers += t < 6 && nl >= 10 && nu == 8;   /* (the ~90 LEDs dirty take ~8 transfers) */
    }
    CHECK(nback == (int)sizeof msg && !memcmp(back, msg, sizeof msg), "the SysEx came out whole on cable 2 (%d bytes)",
          nback);
    CHECK(max_usba == 8, "at most the cap a transfer (%d)", max_usba);
    CHECK(led_transfers == 6, "10+ LED writes beside 8 USB-A packets in each of the first 6 transfers (%d of %d)",
          led_transfers, transfers);

    /* a full queue refuses a message whole */
    {
        static uint8_t big[3 * 4000 + 2];
        int n = 0;
        big[0] = 0xF0;
        memset(big + 1, 0x11, sizeof big - 2);
        big[sizeof big - 1] = 0xF7;
        while (mr_usba_send(big, sizeof big) > 0)
            n++;
        CHECK(n == 1 && mr_usba_send(big, sizeof big) == -1 && mr_usba_send((const uint8_t[]){ 0x90, 1, 2 }, 3) == 1,
              "a full queue refuses a big message whole, a small one fits (%d)", n);
    }
    {
        uint32_t a, b, c;
        mr_usba_stats(&a, &b, &c);
        CHECK(a == 1 && b == 0 && c >= 101, "stats: in %u dropped %u out %u", a, b, c);
    }
    {   /* the audio input sink (mr_set_audio_in_sink): each block's input pcm, as it came; silence without one */
        int16_t ain[MR_BLOCK * 2];
        for (i = 0; i < MR_BLOCK * 2; i++)
            ain[i] = (int16_t)(i * 37 - 4000);
        mr_rt_block(in, 0, ain, aout, out, &nout);
        CHECK(g_ain_calls == 0, "no sink: nothing called");
        mr_set_audio_in_sink(on_ain, NULL);
        mr_rt_block(in, 0, ain, aout, out, &nout);
        CHECK(g_ain_calls == 1 && g_ain_frames == MR_BLOCK && !memcmp(g_ain, ain, sizeof ain),
              "the sink gets the block's input pcm (%d calls, %d frames)", g_ain_calls, g_ain_frames);
        mr_rt_block(in, 0, NULL, aout, out, &nout);
        for (i = 0, k = 0; i < MR_BLOCK * 2; i++)
            k |= g_ain[i];
        CHECK(g_ain_calls == 2 && k == 0, "no input pcm: silence");
        mr_set_audio_in_sink(NULL, NULL);
        mr_rt_block(in, 0, ain, aout, out, &nout);
        CHECK(g_ain_calls == 2, "the sink off: not called");
    }
    mr_shutdown();
    rmdir(dir);
    printf(g_fails ? "usba_check: FAILED\n" : "usba_check: passed\n");
    return g_fails ? 1 : 0;
}
