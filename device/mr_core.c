/* SPDX-License-Identifier: MPL-2.0 */
/* Move Remote's device core: everything but the hardware link. See mr_core.h for the threads and
 * docs/PROTOCOL.md for the wire.
 *
 *   rt thread -> main    r_in (surface + USB-A events), r_ain (input pcm), r_batch (audio clock ticks)
 *   main -> rt thread    r_led (LED writes, grouped so a SysEx never splits), r_mout (USB-A out), r_aout (output pcm),
 *                        r_inject (the test hook's pretend presses)
 *
 * The exit gesture (Shift + volume knob touch + jog click) and the power button's acknowledgement are decided on the
 * rt thread, from the raw events, whatever the host is doing. */
#define _GNU_SOURCE
#include "mr_core.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../common/font5x7.h"
#include "../common/ring.h"
#include "../include/moveremote/proto.h"
#include "mr_ws.h"
#include "web_page.h"   /* build/gen: web/index.html as mr_web_page[] / mr_web_page_len */

/* ------------------------------------------------------------------ shared state --- */
typedef struct { uint32_t frame; uint8_t pkt[4]; } in_rec_t;
typedef struct { uint8_t n, pad[3]; uint8_t pkt[6][4]; } led_rec_t;
typedef struct { uint32_t frame; uint16_t nframes, fill, pcm; uint8_t flags, pad; } batch_rec_t;

static mr_ring_t r_in, r_ain, r_batch, r_led, r_mout, r_aout, r_inject;

static int g_wake[2] = { -1, -1 };
static volatile int g_wake_pending;
static volatile sig_atomic_t g_stop_req;

/* main -> rt */
static volatile int g_connected, g_cfg_clock = 1, g_cfg_ain, g_cfg_batch = 2, g_out_gain = 256, g_flush_aout;
static volatile int g_test_vol = 100;
/* rt -> main */
static volatile uint32_t g_frame, g_underruns, g_late, g_in_dropped;
static volatile int g_quit_req, g_peak, g_power_bits, g_battery, g_jacks, g_rt;
static volatile uint32_t g_power_since;   /* the frame the power button went down (0: up) */
/* main only */
static uint32_t g_overruns, g_dropped;

static mr_cfg_t g_cfg;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static void wake_main(void)
{
    if (!__atomic_exchange_n(&g_wake_pending, 1, __ATOMIC_ACQ_REL)) {
        const char b = 1;
        if (write(g_wake[1], &b, 1) < 0) { /* full: the main thread is awake anyway */ }
    }
}

/* ------------------------------------------------------------------ the screen (seqlock) --- */
static struct { uint32_t seq; uint8_t px[MR_SCREEN_BYTES]; } g_frame_buf;

static void frame_publish(const uint8_t *px)
{
    __atomic_add_fetch(&g_frame_buf.seq, 1, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    memcpy(g_frame_buf.px, px, MR_SCREEN_BYTES);
    __atomic_add_fetch(&g_frame_buf.seq, 1, __ATOMIC_RELEASE);
    mr_plat_frame(px);
}

void mr_rt_display(uint8_t *serving)
{
    uint8_t tmp[MR_SCREEN_BYTES];
    const uint32_t s = __atomic_load_n(&g_frame_buf.seq, __ATOMIC_ACQUIRE);
    if (s & 1u)
        return;   /* being written: keep serving the last one */
    memcpy(tmp, g_frame_buf.px, MR_SCREEN_BYTES);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&g_frame_buf.seq, __ATOMIC_RELAXED) == s)
        memcpy(serving, tmp, MR_SCREEN_BYTES);
}

/* ------------------------------------------------------------------ the rt thread --- */
static struct {
    int shift, vol_touch;
    int pw_stage;
    uint8_t pw_cmd[3];
    uint8_t ack_pending;
    int fade;
    int playing, idle_blocks;
    int batch_blocks, batch_pcm, batch_flags;
    uint32_t batch_frame;
} R;

#define FADE_BLOCKS 172   /* half a second from silence: the link may still carry the last owner's block */

/* The power button is a SysEx status report, F0 00 21 1D 01 01 3A <bits> <battery> 00 F7, bit 3 of <bits> the
 * button held. Every report must be answered (F0 00 21 1D 01 01 39 02 F7) or the surface goes quiet after one.
 * After movy-host's matcher (schwung-movy host/surface_keys.c), as evilgeniusmove's egm.c has it. */
static void power_feed(const uint8_t p[4])
{
    static const int16_t WANT[4][4] = { { 0x04, 0xF0, 0x00, 0x21 }, { 0x04, 0x1D, 0x01, 0x01 },
                                        { 0x04, 0x3A, -1, -1 }, { 0x06, 0x00, 0xF7, -1 } };
    int i;
    for (i = 0; i < 4; i++) {
        const int got = i == 0 ? p[0] & 0x0F : p[i];
        if (WANT[R.pw_stage][i] >= 0 && got != WANT[R.pw_stage][i]) {
            R.pw_stage = 0;
            return;
        }
    }
    if (R.pw_stage == 2)
        memcpy(R.pw_cmd, p + 1, 3);
    if (++R.pw_stage < 4)
        return;
    R.pw_stage = 0;
    R.ack_pending = 1;
    if ((R.pw_cmd[1] & 0x08) && !(g_power_bits & 0x08))
        g_power_since = g_frame ? g_frame : 1;
    else if (!(R.pw_cmd[1] & 0x08))
        g_power_since = 0;
    g_power_bits = R.pw_cmd[1];
    g_battery = R.pw_cmd[2];
}

static void rt_event(const uint8_t p[4])
{
    const int cable = p[0] >> 4, cin = p[0] & 0x0F, st = p[1] & 0xF0;
    if (cable == 0 && (cin == 0x04 || cin == 0x06 || R.pw_stage))
        power_feed(p);
    if ((cable != 0 && cable != 2) || cin < 0x04 || cin > 0x0E)
        return;   /* the XMOS heartbeat, cable 14's power companion, the SPI protocol's own */
    if (cable == 0) {
        if (st == 0xB0 && p[2] == MR_CC_SHIFT)
            R.shift = p[3] > 0;
        else if ((st == 0x90 || st == 0x80) && p[2] == MR_NOTE_VOL_TOUCH)
            R.vol_touch = st == 0x90 && p[3] > 0;
        else if (st == 0xB0 && p[2] == MR_CC_JOG_CLICK && p[3] > 0 && R.shift && R.vol_touch)
            g_quit_req = 1;
        else if (st == 0xB0 && p[2] == MR_CC_LINE_IN)
            g_jacks = (g_jacks & ~1) | (p[3] >= 64);
        else if (st == 0xB0 && p[2] == MR_CC_LINE_OUT)
            g_jacks = (g_jacks & ~2) | ((p[3] >= 64) << 1);
    }
    {
        in_rec_t rec;
        rec.frame = g_frame;
        memcpy(rec.pkt, p, 4);
        if (!mr_ring_push(&r_in, &rec, 1))
            g_in_dropped++;
    }
}

void mr_rt_late(void) { g_late++; }

void mr_rt_block(const uint8_t *in, int nin, const int16_t *ain, int16_t *aout, uint8_t out[][4], int *nout)
{
    int i, n = 0, woke = 0, take, underrun = 0;
    uint32_t c0 = mr_ring_count(&r_in);
    /* ---- input: the transfer's events, then the test hook's */
    for (i = 0; i < nin; i++) {
        const uint8_t *e = in + i * 8;
        if (e[0] | e[1] | e[2] | e[3])
            rt_event(e);
    }
    {
        uint8_t p[4];
        while (mr_ring_pop(&r_inject, p, 1))
            rt_event(p);
    }
    if (mr_ring_count(&r_in) != c0 || g_quit_req)
        woke = 1;

    /* ---- out to the surface: the power ACK first, USB-A next (timing matters), then LED groups whole */
    if (R.ack_pending) {
        static const uint8_t ACK[3][4] = { { 0x04, 0xF0, 0x00, 0x21 }, { 0x04, 0x1D, 0x01, 0x01 },
                                           { 0x07, 0x39, 0x02, 0xF7 } };
        memcpy(out[n], ACK, sizeof ACK);
        n += 3;
        R.ack_pending = 0;
    }
    while (n < MR_OUT_MAX - 6 && mr_ring_pop(&r_mout, out[n], 1))
        n++;
    for (;;) {
        const led_rec_t *g = mr_ring_peek(&r_led);
        if (!g || n + g->n > MR_OUT_MAX)
            break;
        memcpy(out[n], g->pkt, (size_t)g->n * 4);
        n += g->n;
        mr_ring_pop(&r_led, NULL, 1);
    }
    *nout = n;

    /* ---- audio out: the host's queue, scaled, faded in from the start */
    if (g_flush_aout) {
        mr_ring_pop(&r_aout, NULL, r_aout.cap);
        g_flush_aout = 0;
        R.playing = 0;
    }
    take = (int)mr_ring_pop(&r_aout, aout, MR_BLOCK);
    if (take < MR_BLOCK) {
        memset(aout + take * 2, 0, (size_t)(MR_BLOCK - take) * 4);
        if (R.playing) {
            underrun = 1;
            g_underruns++;
        }
    }
    if (take > 0)
        R.playing = 1, R.idle_blocks = 0;
    else if (R.playing && ++R.idle_blocks > 344)   /* a second of nothing: the host stopped, not a dropout */
        R.playing = 0;
    {
        const int gain = g_out_gain, vol = g_test_vol;
        int pk = g_peak;
        for (i = 0; i < MR_BLOCK * 2; i++) {
            int v = aout[i] * gain / 256;
            const int a = v < 0 ? -v : v;
            if (a > pk)
                pk = a;
            if (vol < 100)
                v = v * vol / 100;
            if (R.fade < FADE_BLOCKS)
                v = v * R.fade / FADE_BLOCKS;
            aout[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        g_peak = pk;
        if (R.fade < FADE_BLOCKS)
            R.fade++;
    }

    /* ---- the audio clock (and the input pcm) for the host */
    if (g_connected && g_cfg_clock) {
        if (R.batch_blocks == 0)
            R.batch_frame = g_frame, R.batch_pcm = 0, R.batch_flags = 0;
        if (g_cfg_ain) {
            static const int16_t silence[MR_BLOCK * 2];
            R.batch_pcm += (int)mr_ring_push(&r_ain, ain ? ain : silence, MR_BLOCK);
        }
        if (underrun)
            R.batch_flags |= MR_AUDIO_UNDERRUN;
        if (++R.batch_blocks >= g_cfg_batch) {
            batch_rec_t b;
            b.frame = R.batch_frame;
            b.nframes = (uint16_t)(R.batch_blocks * MR_BLOCK);
            b.fill = (uint16_t)(mr_ring_count(&r_aout) > 65535 ? 65535 : mr_ring_count(&r_aout));
            b.pcm = (uint16_t)R.batch_pcm;
            b.flags = (uint8_t)R.batch_flags;
            b.pad = 0;
            mr_ring_push(&r_batch, &b, 1);
            R.batch_blocks = 0;
            woke = 1;
        }
    } else
        R.batch_blocks = 0;

    g_frame += MR_BLOCK;
    if (woke)
        wake_main();
}

/* ------------------------------------------------------------------ LEDs (main thread) --- */
typedef struct { uint8_t mode, st, val, r, g, b; } led_t;   /* mode 1 palette/brightness, 2 RGB */
static led_t g_want[2][128];                               /* [0] notes, [1] CCs */
static uint8_t g_dirty[2][128];
static int g_ndirty, g_led_cursor;

static int led_target(int cc, int num)
{
    if (!cc)
        return (num >= 16 && num <= 31) || (num >= 68 && num <= 99);
    return (num >= 16 && num <= 31) || (num >= 40 && num <= 43) || (num >= 49 && num <= 52) ||
           (num >= 54 && num <= 56) || num == 58 || num == 60 || num == 62 || num == 63 || (num >= 71 && num <= 78) ||
           num == 85 || num == 86 || num == 88 || num == 118 || num == 119;
}

static void led_set(int cc, int num, const led_t *v)
{
    if (!led_target(cc, num))
        return;
    g_want[cc][num] = *v;
    if (!g_dirty[cc][num])
        g_dirty[cc][num] = 1, g_ndirty++;
}

static void leds_off(void)
{
    int cc, num;
    for (cc = 0; cc < 2; cc++)
        for (num = 0; num < 128; num++) {
            const led_t off = { 1, (uint8_t)(cc ? 0xB0 : 0x90), 0, 0, 0, 0 };
            led_set(cc, num, &off);
        }
}

static void led_feed(void)   /* keeps the rt thread's queue shallow, so the newest value is what goes out */
{
    while (g_ndirty > 0 && mr_ring_count(&r_led) < 12) {
        int k, cc, num;
        led_rec_t g;
        const led_t *w;
        for (k = 0; k < 256; k++) {
            const int at = (g_led_cursor + k) & 255;
            if (g_dirty[at >> 7][at & 127])
                break;
        }
        if (k == 256) {
            g_ndirty = 0;
            return;
        }
        k = (g_led_cursor + k) & 255;
        g_led_cursor = (k + 1) & 255;
        cc = k >> 7, num = k & 127;
        w = &g_want[cc][num];
        memset(&g, 0, sizeof g);
        if (w->mode == 2) {   /* F0 00 21 1D 01 01 3B <0x00 note | 0x10 CC> <idx> rl rh gl gh bl bh F7 */
            const uint8_t s[16] = { 0xF0, 0x00, 0x21, 0x1D, 0x01, 0x01, 0x3B, (uint8_t)(cc ? 0x10 : 0x00),
                                    (uint8_t)num, (uint8_t)(w->r & 0x7F), (uint8_t)(w->r >> 7),
                                    (uint8_t)(w->g & 0x7F), (uint8_t)(w->g >> 7), (uint8_t)(w->b & 0x7F),
                                    (uint8_t)(w->b >> 7), 0xF7 };
            int i;
            for (i = 0; i < 5; i++) {
                g.pkt[i][0] = 0x04;
                memcpy(&g.pkt[i][1], s + i * 3, 3);
            }
            g.pkt[5][0] = 0x05, g.pkt[5][1] = 0xF7;
            g.n = 6;
        } else {
            g.pkt[0][0] = (uint8_t)(w->st >> 4), g.pkt[0][1] = w->st, g.pkt[0][2] = (uint8_t)num, g.pkt[0][3] = w->val;
            g.n = 1;
        }
        if (!mr_ring_push(&r_led, &g, 1))
            return;
        g_dirty[cc][num] = 0;
        g_ndirty--;
    }
}

/* ------------------------------------------------------------------ connections --- */
enum { C_NEW, C_TCP, C_WS, C_HTTP };
#define MAX_CONNS 6
#define IN_CAP (256 * 1024)
#define OUT_CAP (1024 * 1024)

typedef struct {
    int fd, kind, via, hello;
    char name[33];
    uint8_t *in, *raw;   /* in: the message stream; raw: WebSocket frames not yet unwrapped (and the HTTP request) */
    size_t in_n, raw_n;
    uint8_t *out;
    size_t out_n, out_off;
    int closing;         /* flush, then close */
    uint64_t born;
} conn_t;

static int g_listen = -1;
static conn_t g_conns[MAX_CONNS];
static conn_t *g_active;   /* the host in control: the last to say HELLO */
static int g_host_drew;
static uint8_t g_host_px[MR_SCREEN_BYTES];
static int g_quit_why;     /* host sent QUIT */

static void conn_free(conn_t *c)
{
    if (c->fd >= 0)
        close(c->fd);
    free(c->in), free(c->raw), free(c->out);
    memset(c, 0, sizeof *c);
    c->fd = -1;
}

static void host_reset(void)   /* a host arrived or left: back to defaults, dark */
{
    g_cfg_clock = 1, g_cfg_ain = 0, g_cfg_batch = 2, g_out_gain = 256;
    g_flush_aout = 1;
    g_host_drew = 0;
    g_overruns = g_dropped = 0;
    __atomic_store_n(&g_underruns, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_late, 0, __ATOMIC_RELAXED);
    leds_off();
}

static void conn_close(conn_t *c)
{
    if (c == g_active) {
        logf_("host: %s left", c->name);
        g_active = NULL;
        g_connected = 0;
        host_reset();
    }
    conn_free(c);
}

static int out_put(conn_t *c, const void *p, size_t n)
{
    if (c->out_off && c->out_n + n > OUT_CAP) {   /* compact */
        memmove(c->out, c->out + c->out_off, c->out_n - c->out_off);
        c->out_n -= c->out_off;
        c->out_off = 0;
    }
    if (c->out_n + n > OUT_CAP)
        return -1;
    memcpy(c->out + c->out_n, p, n);
    c->out_n += n;
    return 0;
}

static size_t out_free(const conn_t *c) { return OUT_CAP - (c->out_n - c->out_off); }

/* one message; droppable ones (audio) give way when the link is backed up, the rest close a host that is not reading */
static int send_msg(conn_t *c, uint8_t type, const void *payload, size_t n, int droppable)
{
    uint8_t hdr[3 + 10];
    size_t h = 0;
    const size_t len = n + 1;
    if (!c || c->fd < 0 || len > MR_MAX_MSG)
        return -1;
    if (c->kind == C_WS) {
        const size_t wl = len + 2;
        hdr[h++] = 0x82;
        if (wl <= 125)
            hdr[h++] = (uint8_t)wl;
        else if (wl <= 65535)
            hdr[h++] = 126, hdr[h++] = (uint8_t)(wl >> 8), hdr[h++] = (uint8_t)wl;
        else {
            int i;
            hdr[h++] = 127;
            for (i = 7; i >= 0; i--)
                hdr[h++] = (uint8_t)((uint64_t)wl >> (8 * i));
        }
    }
    hdr[h++] = (uint8_t)len, hdr[h++] = (uint8_t)(len >> 8), hdr[h++] = type;
    if (out_free(c) < h + n + (droppable ? 256 * 1024 : 0)) {   /* audio leaves room for everything else */
        if (droppable)
            return -1;
        logf_("host: %s is not reading; closing it", c->name);
        c->closing = 2;   /* close now */
        return -1;
    }
    out_put(c, hdr, h);
    out_put(c, payload, n);
    return 0;
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v), put16(p + 2, v >> 16); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }

static void send_hello(conn_t *c)
{
    uint8_t p[48];
    uint16_t caps = MR_CAP_AUDIO_OUT | MR_CAP_AUDIO_IN | MR_CAP_RGB;
    memset(p, 0, sizeof p);
    memcpy(p, "MOVR", 4);
    if (g_cfg.simulator)
        caps |= MR_CAP_SIMULATOR;
    else
        caps |= MR_CAP_USB_A_MIDI;
    put16(p + 4, MR_PROTO);
    put16(p + 6, caps);
    put32(p + 8, MR_SAMPLE_RATE);
    put16(p + 12, MR_BLOCK);
    p[14] = MR_SCREEN_W, p[15] = MR_SCREEN_H, p[16] = (uint8_t)c->via, p[17] = 0;
    put32(p + 18, r_aout.cap);
    snprintf((char *)p + 22, 24, "moveremote %s", MR_VERSION);
    send_msg(c, MR_D_HELLO, p, 46, 0);
}

static void send_bye(conn_t *c, int why)
{
    const uint8_t r = (uint8_t)why;
    send_msg(c, MR_D_BYE, &r, 1, 0);
    if (c->closing < 1)
        c->closing = 1;
}

static void send_status(conn_t *c)
{
    uint8_t p[MR_STATUS_LEN];
    const uint32_t fill = mr_ring_count(&r_aout);
    memset(p, 0, sizeof p);
    put32(p, g_frame);
    p[4] = (uint8_t)g_battery, p[5] = (uint8_t)g_power_bits, p[6] = (uint8_t)g_jacks;
    p[7] = (uint8_t)((g_rt ? 1 : 0) | (g_test_vol < 100 ? 2 : 0));
    put32(p + 8, g_underruns);
    put32(p + 12, g_overruns);
    put32(p + 16, g_late);
    put32(p + 20, g_dropped);
    put16(p + 24, (uint32_t)(g_ndirty + (int)mr_ring_count(&r_led)));
    put16(p + 26, fill > 65535 ? 65535 : fill);
    p[28] = (uint8_t)g_test_vol;
    send_msg(c, MR_D_STATUS, p, sizeof p, 0);
}

/* USB-A out: complete MIDI messages to USB-MIDI packets on cable 2. SysEx goes 3 bytes a packet (CIN 4), ending with
 * CIN 5/6/7; an unterminated SysEx, a stray data byte or a short message is dropped */
static void midi_out(const uint8_t *p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        const uint8_t b = p[i];
        uint8_t pkt[4] = { 0, 0, 0, 0 };
        if (b == 0xF0) {
            size_t j = i + 1;
            while (j < n && p[j] != 0xF7 && p[j] < 0x80)
                j++;
            if (j >= n || p[j] != 0xF7) {   /* unterminated (or interrupted): drop it */
                i = j;
                continue;
            }
            {
                size_t k = i, left = j - i + 1;
                while (left > 0) {
                    const size_t take = left > 3 ? 3 : left;
                    memset(pkt, 0, 4);
                    memcpy(pkt + 1, p + k, take);
                    pkt[0] = (uint8_t)(0x20 | (left > 3 ? 0x4 : (uint8_t)(0x4 + take)));
                    if (!mr_ring_push(&r_mout, pkt, 1))
                        return;
                    k += take, left -= take;
                }
            }
            i = j + 1;
            continue;
        }
        if (b >= 0xF8) {   /* real time: clock, start, stop, continue ... */
            pkt[0] = 0x2F, pkt[1] = b;
            mr_ring_push(&r_mout, pkt, 1);
            i++;
            continue;
        }
        if (b < 0x80 || b == 0xF7 || b == 0xF4 || b == 0xF5) {
            i++;
            continue;
        }
        {
            int len, cin;
            if (b < 0xF0)
                len = ((b & 0xF0) == 0xC0 || (b & 0xF0) == 0xD0) ? 2 : 3, cin = b >> 4;
            else if (b == 0xF1 || b == 0xF3)
                len = 2, cin = 0x2;
            else if (b == 0xF2)
                len = 3, cin = 0x3;
            else   /* 0xF6 */
                len = 1, cin = 0x5;
            if (i + (size_t)len > n) {
                i = n;
                continue;
            }
            {
                int k, ok = 1;
                for (k = 1; k < len; k++)
                    ok &= p[i + (size_t)k] < 0x80;
                if (ok) {
                    pkt[0] = (uint8_t)(0x20 | cin);
                    memcpy(pkt + 1, p + i, (size_t)len);
                    mr_ring_push(&r_mout, pkt, 1);
                }
            }
            i += (size_t)len;
        }
    }
}

static void become_active(conn_t *c)
{
    if (g_active && g_active != c) {
        logf_("host: %s replaced by %s", g_active->name, c->name);
        send_bye(g_active, MR_BYE_REPLACED);
        g_active = NULL;
    }
    g_active = c;
    host_reset();
    g_connected = 1;
    logf_("host: %s connected via %s", c->name, c->via == MR_VIA_USB ? "USB" : c->via == MR_VIA_LOCAL ? "local" : "Wi-Fi");
}

static void on_msg(conn_t *c, uint8_t type, const uint8_t *p, size_t n)
{
    size_t i;
    if (type == MR_H_HELLO) {
        if (n < 2 || get16(p) != MR_PROTO) {
            send_hello(c);   /* tells it which version we speak */
            send_bye(c, MR_BYE_PROTOCOL);
            return;
        }
        memset(c->name, 0, sizeof c->name);
        if (n > 2)
            memcpy(c->name, p + 2, n - 2 > 32 ? 32 : n - 2);
        for (i = 0; c->name[i]; i++)
            if ((unsigned char)c->name[i] < 32 || (unsigned char)c->name[i] > 126)
                c->name[i] = '?';
        if (!c->name[0])
            strcpy(c->name, "host");
        c->hello = 1;
        send_hello(c);
        become_active(c);
        send_status(c);
        return;
    }
    if (c != g_active) {
        if (type == MR_H_PING && n >= 12) {   /* anyone may measure */
            uint8_t r[16];
            memcpy(r, p, 12);
            put32(r + 12, g_frame);
            send_msg(c, MR_D_PONG, r, 16, 0);
        }
        return;   /* only the host in control drives the Move */
    }
    switch (type) {
    case MR_H_LED:
        for (i = 0; i + 3 <= n; i += 3) {
            const uint8_t st = p[i], hi = st & 0xF0;
            if ((hi == 0x90 || hi == 0xB0) && p[i + 1] < 128 && p[i + 2] < 128) {
                const led_t v = { 1, st, p[i + 2], 0, 0, 0 };
                led_set(hi == 0xB0, p[i + 1], &v);
            }
        }
        break;
    case MR_H_LED_RGB:
        for (i = 0; i + 5 <= n; i += 5)
            if (p[i] <= 1 && p[i + 1] < 128) {
                const led_t v = { 2, 0, 0, p[i + 2], p[i + 3], p[i + 4] };
                led_set(p[i], p[i + 1], &v);
            }
        break;
    case MR_H_DISPLAY:
        if (n == MR_SCREEN_BYTES) {
            memcpy(g_host_px, p, MR_SCREEN_BYTES);
            g_host_drew = 1;
            frame_publish(g_host_px);
        }
        break;
    case MR_H_AUDIO: {
        const uint32_t frames = (uint32_t)(n / 4), went = mr_ring_push(&r_aout, p, frames);
        g_overruns += frames - went;
        break;
    }
    case MR_H_MIDI:
        if (!g_cfg.simulator)
            midi_out(p, n);
        break;
    case MR_H_CONFIG:
        for (i = 0; i + 5 <= n; i += 5) {
            const int32_t v = (int32_t)get32(p + i + 1);
            switch (p[i]) {
            case MR_CFG_AUDIO_CLOCK: g_cfg_clock = v != 0; break;
            case MR_CFG_AUDIO_IN: g_cfg_ain = v != 0; break;
            case MR_CFG_AUDIO_BATCH: g_cfg_batch = v < 1 ? 1 : v > 8 ? 8 : v; break;
            case MR_CFG_OUT_GAIN: g_out_gain = v < 0 ? 0 : v > 256 ? 256 : v; break;
            case MR_CFG_LEDS_OFF: leds_off(); break;
            default: break;
            }
        }
        break;
    case MR_H_PING:
        if (n >= 12) {
            uint8_t r[16];
            memcpy(r, p, 12);
            put32(r + 12, g_frame);
            send_msg(c, MR_D_PONG, r, 16, 0);
        }
        break;
    case MR_H_QUIT:
        g_quit_why = 1;
        break;
    case MR_H_RAW:
        for (i = 0; i + 4 <= n;) {   /* cable 0, CIN 4..E, grouped six at a time so a short SysEx stays whole */
            led_rec_t g;
            memset(&g, 0, sizeof g);
            while (i + 4 <= n && g.n < 6) {
                const int cable = p[i] >> 4, cin = p[i] & 0x0F;
                if (cable == 0 && cin >= 0x4 && cin <= 0xE)
                    memcpy(g.pkt[g.n++], p + i, 4);
                i += 4;
            }
            if (g.n && !mr_ring_push(&r_led, &g, 1))
                break;
        }
        break;
    default:
        break;
    }
}

static int parse_stream(conn_t *c)   /* whole messages out of c->in */
{
    size_t off = 0;
    while (c->in_n - off >= 2) {
        const size_t len = get16(c->in + off);
        if (len == 0) {
            send_bye(c, MR_BYE_PROTOCOL);
            return -1;
        }
        if (c->in_n - off < 2 + len)
            break;
        on_msg(c, c->in[off + 2], c->in + off + 3, len - 1);
        off += 2 + len;
        if (c->fd < 0)
            return -1;
    }
    if (off) {
        memmove(c->in, c->in + off, c->in_n - off);
        c->in_n -= off;
    }
    return 0;
}

static int in_append(conn_t *c, const uint8_t *p, size_t n)
{
    if (c->in_n + n > IN_CAP)
        return -1;
    memcpy(c->in + c->in_n, p, n);
    c->in_n += n;
    return 0;
}

static int ws_frames(conn_t *c)   /* unwrap whole frames from c->raw into the stream */
{
    size_t off = 0;
    for (;;) {
        const uint8_t *f = c->raw + off;
        const size_t have = c->raw_n - off;
        size_t h = 2, plen;
        int op, i;
        uint8_t mask[4];
        if (have < 2)
            break;
        op = f[0] & 0x0F;
        plen = f[1] & 0x7F;
        if (plen == 126) {
            if (have < 4)
                break;
            plen = (size_t)f[2] << 8 | f[3];
            h = 4;
        } else if (plen == 127) {
            if (have < 10)
                break;
            plen = 0;
            for (i = 2; i < 10; i++)
                plen = plen << 8 | f[i];
            h = 10;
            if (plen > IN_CAP)
                return -1;
        }
        if (!(f[1] & 0x80))
            return -1;   /* a client's frames are masked, always */
        if (have < h + 4 + plen)
            break;
        memcpy(mask, f + h, 4);
        h += 4;
        {
            uint8_t *d = (uint8_t *)f + h;
            size_t k;
            for (k = 0; k < plen; k++)
                d[k] ^= mask[k & 3];
            if (op == 0x2 || op == 0x0) {
                if (in_append(c, d, plen) < 0)
                    return -1;
            } else if (op == 0x8) {
                return -1;
            } else if (op == 0x9) {   /* ping: pong it back */
                uint8_t hdr[2] = { 0x8A, (uint8_t)(plen > 125 ? 125 : plen) };
                out_put(c, hdr, 2);
                out_put(c, d, hdr[1]);
            }
        }
        off += h + plen;
    }
    if (off) {
        memmove(c->raw, c->raw + off, c->raw_n - off);
        c->raw_n -= off;
    }
    return 0;
}

static const char *find_header(const char *req, const char *name, char *val, size_t cap)
{
    const char *p = req;
    const size_t nl = strlen(name);
    while ((p = strchr(p, '\n'))) {
        p++;
        if (!strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1;
            size_t k = 0;
            while (*v == ' ')
                v++;
            while (*v && *v != '\r' && *v != '\n' && k + 1 < cap)
                val[k++] = *v++;
            val[k] = 0;
            return val;
        }
    }
    return NULL;
}

static void http_request(conn_t *c)
{
    char path[128] = "", key[64], resp[512];
    const char *req = (const char *)c->raw;
    sscanf(req, "GET %127s", path);
    if (!strncmp(path, "/ws", 3) && find_header(req, "Sec-WebSocket-Key", key, sizeof key)) {
        char acc[32];
        int n;
        mr_ws_accept(key, acc);
        n = snprintf(resp, sizeof resp,
                     "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n", acc);
        out_put(c, resp, (size_t)n);
        c->kind = C_WS;
        c->raw_n = 0;   /* (a client waits for the 101 before it sends a frame) */
        return;
    }
    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        const int n = snprintf(resp, sizeof resp,
                               "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %u\r\n"
                               "Cache-Control: no-store\r\nConnection: close\r\n\r\n", (unsigned)mr_web_page_len);
        if (out_put(c, resp, (size_t)n) == 0)
            out_put(c, mr_web_page, mr_web_page_len);
    } else {
        static const char nf[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        out_put(c, nf, sizeof nf - 1);
    }
    c->kind = C_HTTP;
    c->closing = 1;
}

static void conn_read(conn_t *c)
{
    uint8_t buf[16384];
    for (;;) {
        const ssize_t r = recv(c->fd, buf, sizeof buf, 0);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            conn_close(c);
            return;
        }
        if (r < 0)
            return;
        if (c->kind == C_NEW) {
            if (c->raw_n + (size_t)r > 8192) {
                conn_close(c);
                return;
            }
            memcpy(c->raw + c->raw_n, buf, (size_t)r);
            c->raw_n += (size_t)r;
            if (c->raw_n >= 4 && !memcmp(c->raw, "GET ", 4)) {
                c->raw[c->raw_n] = 0;
                if (strstr((char *)c->raw, "\r\n\r\n"))
                    http_request(c);
                continue;
            }
            if (c->raw_n >= 4 || (c->raw_n > 0 && c->raw[0] != 'G')) {   /* not HTTP: the plain stream */
                c->kind = C_TCP;
                if (in_append(c, c->raw, c->raw_n) < 0) {
                    conn_close(c);
                    return;
                }
                c->raw_n = 0;
                if (parse_stream(c) < 0)
                    return;
            }
            continue;
        }
        if (c->kind == C_TCP) {
            if (in_append(c, buf, (size_t)r) < 0) {
                send_bye(c, MR_BYE_PROTOCOL);
                return;
            }
            if (parse_stream(c) < 0)
                return;
        } else if (c->kind == C_WS) {
            if (c->raw_n + (size_t)r > IN_CAP) {
                conn_close(c);
                return;
            }
            memcpy(c->raw + c->raw_n, buf, (size_t)r);
            c->raw_n += (size_t)r;
            if (ws_frames(c) < 0) {
                conn_close(c);
                return;
            }
            if (parse_stream(c) < 0)
                return;
        }
        /* C_HTTP: whatever else it sends is ignored */
    }
}

static void conn_write(conn_t *c)
{
    while (c->out_off < c->out_n) {
        const ssize_t w = send(c->fd, c->out + c->out_off, c->out_n - c->out_off, 0);
        if (w < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                conn_close(c);
            return;
        }
        c->out_off += (size_t)w;
    }
    c->out_off = c->out_n = 0;
}

static int via_of(int fd)
{
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    uint32_t a = 0;
    if (getsockname(fd, (struct sockaddr *)&ss, &sl) != 0)
        return MR_VIA_WIFI;
    if (ss.ss_family == AF_INET)
        a = ntohl(((struct sockaddr_in *)&ss)->sin_addr.s_addr);
    else if (ss.ss_family == AF_INET6) {
        const uint8_t *b = ((struct sockaddr_in6 *)&ss)->sin6_addr.s6_addr;
        static const uint8_t mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
        static const uint8_t loop6[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
        if (!memcmp(b, loop6, 16))
            return MR_VIA_LOCAL;
        if (memcmp(b, mapped, 12))
            return MR_VIA_WIFI;
        a = (uint32_t)b[12] << 24 | (uint32_t)b[13] << 16 | (uint32_t)b[14] << 8 | b[15];
    }
    if ((a >> 24) == 127)
        return MR_VIA_LOCAL;
    if ((a >> 8) == ((172u << 16) | (16u << 8) | 254u))
        return MR_VIA_USB;
    return MR_VIA_WIFI;
}

static void accept_all(void)
{
    for (;;) {
        const int fd = accept(g_listen, NULL, NULL);
        int i, one = 1;
        conn_t *c = NULL;
        if (fd < 0)
            return;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        for (i = 0; i < MAX_CONNS; i++)
            if (g_conns[i].fd < 0) {
                c = &g_conns[i];
                break;
            }
        if (!c) {
            close(fd);
            continue;
        }
        c->fd = fd;
        c->kind = C_NEW;
        c->via = via_of(fd);
        c->in = malloc(IN_CAP);
        c->raw = malloc(IN_CAP + 1);
        c->out = malloc(OUT_CAP);
        c->born = now_ms();
        strcpy(c->name, "?");
        if (!c->in || !c->raw || !c->out)
            conn_free(c);
    }
}

static int listen_on(int port)
{
    int fd = socket(AF_INET6, SOCK_STREAM, 0), one = 1, zero = 0;
    if (fd >= 0) {
        struct sockaddr_in6 a6;
        memset(&a6, 0, sizeof a6);
        a6.sin6_family = AF_INET6;
        a6.sin6_addr = in6addr_any;
        a6.sin6_port = htons((uint16_t)port);
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
        if (bind(fd, (struct sockaddr *)&a6, sizeof a6) != 0) {
            close(fd);
            fd = -1;
        }
    }
    if (fd < 0) {   /* no IPv6: IPv4 alone */
        struct sockaddr_in a4;
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        memset(&a4, 0, sizeof a4);
        a4.sin_family = AF_INET;
        a4.sin_addr.s_addr = htonl(INADDR_ANY);
        a4.sin_port = htons((uint16_t)port);
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (bind(fd, (struct sockaddr *)&a4, sizeof a4) != 0) {
            close(fd);
            return -1;
        }
    }
    if (listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

/* ------------------------------------------------------------------ the status screen --- */
static uint8_t g_status_px[MR_SCREEN_BYTES];
static char g_ip_usb[20], g_ip_wifi[20];

static void text_at(uint8_t *px, int x, int page, const char *s, int inv)
{
    for (; *s && x <= MR_SCREEN_W - MR_FONT_W; s++, x += MR_FONT_ADV) {
        const uint8_t *g = mr_glyph(*s);
        int k;
        for (k = 0; k < MR_FONT_ADV && x + k < MR_SCREEN_W; k++)
            px[page * MR_SCREEN_W + x + k] = (uint8_t)((k < MR_FONT_W ? g[k] : 0) ^ (inv ? 0xFF : 0));
    }
}

static void read_ips(void)
{
    struct ifaddrs *ifa, *i;
    g_ip_usb[0] = g_ip_wifi[0] = 0;
    if (getifaddrs(&ifa) != 0)
        return;
    for (i = ifa; i; i = i->ifa_next) {
        char buf[INET_ADDRSTRLEN];
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK))
            continue;
        inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, buf, sizeof buf);
        if (!strncmp(buf, "172.16.254.", 11) || !strncmp(i->ifa_name, "usb", 3))
            snprintf(g_ip_usb, sizeof g_ip_usb, "%s", buf);
        else if (!g_ip_wifi[0])
            snprintf(g_ip_wifi, sizeof g_ip_wifi, "%s", buf);
    }
    freeifaddrs(ifa);
}

static void draw_status(void)
{
    char line[32];
    uint8_t *px = g_status_px;
    memset(px, 0, MR_SCREEN_BYTES);
    memset(px, 0xFF, MR_SCREEN_W);
    text_at(px, 2, 0, "MOVE REMOTE", 1);
    text_at(px, 128 - 6 * (int)strlen(MR_VERSION) - 2, 0, MR_VERSION, 1);
    snprintf(line, sizeof line, "USB  %s", g_ip_usb[0] ? g_ip_usb : "-");
    text_at(px, 0, 2, line, 0);
    snprintf(line, sizeof line, "WIFI %s", g_ip_wifi[0] ? g_ip_wifi : "-");
    text_at(px, 0, 3, line, 0);
    snprintf(line, sizeof line, "PORT %d", g_cfg.port);
    text_at(px, 0, 4, line, 0);
    if (g_active) {
        snprintf(line, sizeof line, "%.13s %s", g_active->name,
                 g_active->via == MR_VIA_USB ? "USB" : g_active->via == MR_VIA_LOCAL ? "LOCAL" : "WIFI");
        text_at(px, 0, 5, line, 0);
    } else
        text_at(px, 0, 5, "waiting for a host", 0);
    text_at(px, 0, 7, "SHIFT+VOL+JOG: exit", 0);
}

/* ------------------------------------------------------------------ the test hook --- */
/* Only when <test_dir>/mr-test exists at the start (a reboot clears /dev/shm): lines written to the FIFO
 * <test_dir>/mr-cmd press controls ("note N VEL", "cc N VAL", "at N PRESSURE", "shot", "quit"), and
 * <test_dir>/mr-state says how things stand four times a second. A TEST IS QUIET: output is silent, or at the percent
 * written in mr-test. peak= in the state line is the level before that, so a silent run still shows sound was made */
static int g_test_fd = -1;
static char g_test_path[3][300];

static void test_open(void)
{
    char p[300];
    FILE *f;
    int vol = 0;
    snprintf(p, sizeof p, "%s/mr-test", g_cfg.test_dir);
    if (!(f = fopen(p, "r")))
        return;
    if (fscanf(f, "%d", &vol) != 1)
        vol = 0;
    fclose(f);
    g_test_vol = vol < 0 ? 0 : vol > 100 ? 100 : vol;
    snprintf(g_test_path[0], sizeof g_test_path[0], "%s/mr-cmd", g_cfg.test_dir);
    snprintf(g_test_path[1], sizeof g_test_path[1], "%s/mr-state", g_cfg.test_dir);
    snprintf(g_test_path[2], sizeof g_test_path[2], "%s/mr-screen.pbm", g_cfg.test_dir);
    unlink(g_test_path[0]);
    if (mkfifo(g_test_path[0], 0666) != 0)
        return;
    chmod(g_test_path[0], 0666);
    g_test_fd = open(g_test_path[0], O_RDWR | O_NONBLOCK | O_CLOEXEC);   /* RDWR: never an end of file */
    logf_("test hook: on (%s), output at %d%%", g_test_path[0], g_test_vol);
}

static void test_shot(void)
{
    FILE *f;
    char tmp[310];
    const uint8_t *px = g_host_drew ? g_host_px : g_status_px;
    int x, y;
    snprintf(tmp, sizeof tmp, "%s.tmp", g_test_path[2]);
    if (!(f = fopen(tmp, "w")))
        return;
    fprintf(f, "P1\n128 64\n");
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 128; x++)
            fputc(px[(y >> 3) * 128 + x] >> (y & 7) & 1 ? '1' : '0', f);
        fputc('\n', f);
    }
    fclose(f);
    rename(tmp, g_test_path[2]);
}

static int test_poll(uint64_t ms)
{
    static char buf[512];
    static int n;
    static uint64_t last;
    ssize_t r;
    int quit = 0;
    if (g_test_fd < 0)
        return 0;
    while ((r = read(g_test_fd, buf + n, sizeof buf - 1 - (size_t)n)) > 0) {
        char *nl;
        n += (int)r;
        buf[n] = 0;
        while ((nl = strchr(buf, '\n'))) {
            char cmd[8] = "";
            int a = 0, b = 0;
            *nl = 0;
            if (!strcmp(buf, "shot"))
                test_shot();
            else if (!strcmp(buf, "quit"))
                quit = 1;
            else if (sscanf(buf, "%7s %d %d", cmd, &a, &b) == 3 && a >= 0 && a < 128 && b >= 0 && b < 128) {
                uint8_t p[4] = { 0, 0, (uint8_t)a, (uint8_t)b };
                if (!strcmp(cmd, "note"))
                    p[0] = b ? 0x09 : 0x08, p[1] = b ? 0x90 : 0x80;
                else if (!strcmp(cmd, "cc"))
                    p[0] = 0x0B, p[1] = 0xB0;
                else if (!strcmp(cmd, "at"))
                    p[0] = 0x0A, p[1] = 0xA0;
                if (p[1])
                    mr_ring_push(&r_inject, p, 1);
            }
            n -= (int)(nl + 1 - buf);
            memmove(buf, nl + 1, (size_t)n + 1);
        }
        if (n >= (int)sizeof buf - 1)
            n = 0;
    }
    if (ms - last >= 250u) {
        char tmp[310];
        FILE *f;
        last = ms;
        snprintf(tmp, sizeof tmp, "%s.tmp", g_test_path[1]);
        if ((f = fopen(tmp, "w"))) {
            fprintf(f, "ms=%llu host=%s via=%d frame=%u late=%u under=%u over=%u dropped=%u led_pending=%d out_fill=%u "
                       "vol=%d peak=%d power=%d drew=%d rt=%d\n",
                    (unsigned long long)ms, g_active ? g_active->name : "-", g_active ? g_active->via : -1,
                    (unsigned)g_frame, (unsigned)g_late, (unsigned)g_underruns, (unsigned)g_overruns,
                    (unsigned)g_dropped, g_ndirty + (int)mr_ring_count(&r_led), (unsigned)mr_ring_count(&r_aout),
                    g_test_vol, g_peak, g_power_bits, g_host_drew, g_rt);
            g_peak = 0;
            fclose(f);
            rename(tmp, g_test_path[1]);
        }
    }
    return quit;
}

/* ------------------------------------------------------------------ the main thread --- */
int mr_init(const mr_cfg_t *cfg)
{
    int i;
    g_cfg = *cfg;
    if (!g_cfg.port)
        g_cfg.port = MR_PORT;
    if (!g_cfg.test_dir)
        g_cfg.test_dir = "/dev/shm";
    for (i = 0; i < MAX_CONNS; i++)
        g_conns[i].fd = -1;
    if (mr_ring_init(&r_in, sizeof(in_rec_t), 4096) || mr_ring_init(&r_ain, 4, 16384) ||
        mr_ring_init(&r_batch, sizeof(batch_rec_t), 512) || mr_ring_init(&r_led, sizeof(led_rec_t), 256) ||
        mr_ring_init(&r_mout, 4, 1024) || mr_ring_init(&r_aout, 4, 16384) || mr_ring_init(&r_inject, 4, 256))
        return -1;
    if (pipe(g_wake) != 0)
        return -1;
    for (i = 0; i < 2; i++) {
        fcntl(g_wake[i], F_SETFL, fcntl(g_wake[i], F_GETFL) | O_NONBLOCK);
        fcntl(g_wake[i], F_SETFD, FD_CLOEXEC);
    }
    if ((g_listen = listen_on(g_cfg.port)) < 0) {
        logf_("net: cannot listen on port %d: %s", g_cfg.port, strerror(errno));
        return -1;
    }
    logf_("net: listening on port %d", g_cfg.port);
    test_open();
    read_ips();
    draw_status();
    frame_publish(g_status_px);
    leds_off();
    return 0;
}

void mr_request_stop(void) { g_stop_req = 1; }
void mr_set_realtime(int rt) { g_rt = rt; }
int mr_test_vol(void) { return g_test_vol; }

static void pump_input(void)   /* the rt thread's events to the host, as few messages as fit */
{
    in_rec_t recs[512];
    uint32_t n;
    while ((n = mr_ring_pop(&r_in, recs, 512)) > 0) {
        uint8_t p[512 * 8];
        uint32_t i;
        if (!g_active)
            continue;
        for (i = 0; i < n; i++) {
            put32(p + i * 8, recs[i].frame);
            memcpy(p + i * 8 + 4, recs[i].pkt, 4);
        }
        send_msg(g_active, MR_D_INPUT, p, n * 8, 0);
    }
}

static void pump_audio(void)
{
    batch_rec_t b;
    while (mr_ring_pop(&r_batch, &b, 1)) {
        static uint8_t p[11 + 8 * MR_BLOCK * 4];
        const int with_pcm = b.pcm == b.nframes && b.pcm > 0;
        size_t n = 9;
        if (b.pcm && !with_pcm) {   /* config flipped mid-batch: the odd frames go */
            mr_ring_pop(&r_ain, NULL, b.pcm);
        }
        put32(p, b.frame);
        put16(p + 4, b.nframes);
        put16(p + 6, b.fill);
        p[8] = (uint8_t)(b.flags | (with_pcm ? MR_AUDIO_HAS_PCM : 0));
        if (with_pcm) {
            mr_ring_pop(&r_ain, p + 9, b.pcm);
            n += (size_t)b.pcm * 4;
        }
        if (g_active && send_msg(g_active, MR_D_AUDIO, p, n, 1) < 0)
            g_dropped++;
    }
}

int mr_step(int max_wait_ms)
{
    struct pollfd pf[MAX_CONNS + 3];
    conn_t *pc[MAX_CONNS + 3];
    int np = 0, i, timeout;
    uint64_t ms;
    static uint64_t last_status, last_screen, last_ips, quit_at;
    static int quitting;

    pf[np].fd = g_wake[0], pf[np].events = POLLIN, pc[np++] = NULL;
    pf[np].fd = g_listen, pf[np].events = POLLIN, pc[np++] = NULL;
    for (i = 0; i < MAX_CONNS; i++)
        if (g_conns[i].fd >= 0) {
            pf[np].fd = g_conns[i].fd;
            pf[np].events = (short)(POLLIN | (g_conns[i].out_n > g_conns[i].out_off ? POLLOUT : 0));
            pc[np++] = &g_conns[i];
        }
    timeout = g_ndirty > 0 ? 2 : max_wait_ms;
    if (g_test_fd >= 0 && timeout > 20)
        timeout = 20;
    if (poll(pf, (nfds_t)np, timeout) < 0 && errno != EINTR)
        return MR_STOP_SIGNAL;
    if (pf[0].revents & POLLIN) {
        char drain[64];
        __atomic_store_n(&g_wake_pending, 0, __ATOMIC_RELEASE);
        while (read(g_wake[0], drain, sizeof drain) > 0) {}
    }
    if (pf[1].revents & POLLIN)
        accept_all();
    for (i = 2; i < np; i++) {
        conn_t *c = pc[i];
        if (c->fd != pf[i].fd)
            continue;
        if (pf[i].revents & (POLLIN | POLLHUP | POLLERR))
            conn_read(c);
        if (c->fd >= 0 && c->closing == 2)
            conn_close(c);
    }
    ms = now_ms();
    if (test_poll(ms))
        g_quit_why = 1;

    pump_input();
    pump_audio();
    led_feed();

    if (g_active && ms - last_status >= 500u) {
        last_status = ms;
        send_status(g_active);
    }
    if (ms - last_ips >= 3000u) {
        last_ips = ms;
        read_ips();
    }
    if (!g_host_drew && ms - last_screen >= 500u) {
        last_screen = ms;
        draw_status();
        frame_publish(g_status_px);
    } else if (g_host_drew)
        last_screen = 0;

    /* stale connections: an HTTP request or a plain stream that never says HELLO */
    for (i = 0; i < MAX_CONNS; i++) {
        conn_t *c = &g_conns[i];
        if (c->fd >= 0 && c != g_active && !c->hello && ms - c->born > 10000u)
            conn_close(c);
    }

    /* why we might stop */
    if (!quitting) {
        int why = MR_RUN;
        if (g_stop_req)
            why = MR_STOP_SIGNAL;
        else if (g_quit_req)
            why = MR_STOP_EXIT;
        else if (g_quit_why)
            why = MR_STOP_EXIT;
        else if (g_power_since && (uint32_t)(g_frame - g_power_since) > 2u * MR_SAMPLE_RATE)
            why = MR_STOP_POWER;
        if (why != MR_RUN) {
            quitting = why;
            quit_at = ms;
            if (g_active)
                send_bye(g_active, why == MR_STOP_POWER ? MR_BYE_SHUTDOWN : g_quit_req ? MR_BYE_EXIT : MR_BYE_QUIT);
            leds_off();
            logf_("stopping: %s", why == MR_STOP_POWER ? "power off" : why == MR_STOP_SIGNAL ? "signal"
                                  : g_quit_req ? "the exit gesture" : "the host asked");
        }
    }

    /* flush what can go; close what is done */
    for (i = 0; i < MAX_CONNS; i++) {
        conn_t *c = &g_conns[i];
        if (c->fd < 0)
            continue;
        if (c->out_n > c->out_off)
            conn_write(c);
        if (c->fd >= 0 && c->closing && c->out_n <= c->out_off)
            conn_close(c);
    }

    if (quitting) {   /* a moment for the BYE and the dark LEDs to get out */
        int all_out = 1;
        for (i = 0; i < MAX_CONNS; i++)
            if (g_conns[i].fd >= 0 && g_conns[i].out_n > g_conns[i].out_off)
                all_out = 0;
        if ((all_out && g_ndirty == 0 && mr_ring_count(&r_led) == 0 && ms - quit_at > 50u) || ms - quit_at > 1000u)
            return quitting;
    }
    return MR_RUN;
}

void mr_shutdown(void)
{
    int i;
    for (i = 0; i < MAX_CONNS; i++)
        if (g_conns[i].fd >= 0)
            conn_free(&g_conns[i]);
    g_active = NULL;
    if (g_listen >= 0)
        close(g_listen);
    g_listen = -1;
    if (g_test_fd >= 0) {
        close(g_test_fd);
        unlink(g_test_path[0]);
    }
}
