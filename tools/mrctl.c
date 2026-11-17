/* SPDX-License-Identifier: MPL-2.0 */
/* mrctl: Move Remote from a terminal. A test bench for the device and an example host for libmoveremote.
 *
 *   mrctl [--host H] [--port N] info               what the device says about itself
 *   mrctl ... monitor [--count N] [--timeout S]    every input event, decoded, one a line
 *   mrctl ... demo                                 pads, steps, buttons and knobs light and draw (Ctrl-C to stop)
 *   mrctl ... ping [N]                             round trips: min / median / max
 *   mrctl ... tone [PERCENT] [SECONDS] [QUEUE]     a 440 Hz sine out of the Move (default 3%, 2 s): QUIET.
 *                                                  QUEUE: frames held on the device (512 USB, 4096 Wi-Fi)
 *   mrctl ... text "LINE" ["LINE" ...]             text on the screen (up to 8 lines), then leave it
 *   mrctl ... selftest [--inject FIFO]             the protocol end to end (with the simulator: make test)
 *   mrctl ... quit                                 back to Move's own software
 *   mrctl ... led note|cc N[-M] VALUE [ANIM]       set LEDs (palette / brightness), held 5 s: led cc 16-31 127
 *   mrctl ... rgb note|cc N[-M] R G B              set RGB LEDs, held 5 s: rgb note 68-99 255 0 0
 *
 * With no --host: the USB link (172.16.254.1) first, then move.local. */
#define _GNU_SOURCE
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/moveremote/client.h"
#include "../include/moveremote/gfx.h"

static volatile sig_atomic_t g_stop;
static void on_int(int s) { (void)s; g_stop = 1; }

static const char *host_arg;

#define CONNECT_HINT                                                                                                  \
    "  Is it running (Schwung: Tools > Move Remote)? \"No route to host\" or \"Operation not permitted\" from a Mac\n" \
    "  terminal usually means macOS's Local Network permission: System Settings > Privacy & Security > Local Network,\n" \
    "  allow the terminal app.\n"
static int port_arg;

/* ------------------------------------------------------------------ shared callbacks --- */
typedef struct {
    int count, max_count, bye;
    int pads[32], steps[16], held_step[16], knob[9], jog, shift;
    char last[64];
    int dirty;
    /* audio */
    mr_pacer_t pacer;
    double phase, amp;
    int tone_on;
    long ticks, in_pcm, underrun_ticks;
    /* ping */
    double rtt[256];
    int nrtt;
    mr_status_t st;
    int have_st;
    mr_client_t *c;
} app_t;

static void print_event(const mr_event_t *e)
{
    static const char *TN[] = { "?", "pad", "pad-pressure", "pressure", "step", "button", "knob", "touch",
                                "jog", "jog-click", "jack", "midi", "sysex", "other" };
    const char *bn = e->type == MR_EV_BUTTON ? mr_button_name(e->index) : NULL;
    printf("%10u  %-12s index=%-3d value=%-4d %s  [%02X %02X %02X %02X]\n", e->frame, TN[e->type], e->index, e->value,
           bn ? bn : "", e->raw[0], e->raw[1], e->raw[2], e->raw[3]);
    fflush(stdout);
}

static void cb_status(void *u, const mr_status_t *s)
{
    app_t *a = u;
    a->st = *s;
    a->have_st = 1;
}

static void cb_bye(void *u, int reason)
{
    app_t *a = u;
    static const char *R[] = { "?", "the exit gesture", "another host took over", "the Move is shutting down",
                               "asked to quit", "a protocol error" };
    a->bye = reason;
    fprintf(stderr, "device: goodbye (%s)\n", reason >= 1 && reason <= 5 ? R[reason] : "?");
}

static void cb_pong(void *u, uint32_t token, uint64_t host_time, uint32_t frame)
{
    app_t *a = u;
    (void)token, (void)frame;
    if (a->nrtt < 256)
        a->rtt[a->nrtt++] = (double)(mr_now_us() - host_time) / 1000.0;
}

static void cb_audio(void *u, uint32_t frame, const int16_t *in, int frames, int out_fill, int flags)
{
    app_t *a = u;
    (void)frame;
    a->ticks++;
    if (in)
        a->in_pcm += frames;
    if ((flags & MR_AUDIO_UNDERRUN) && a->tone_on)   /* (not the drain after the tone stops) */
        a->underrun_ticks++;
    if (a->tone_on) {
        static int16_t buf[16384 * 2];
        int n = mr_pacer_frames(&a->pacer, frames, out_fill), i;
        if (n > 16384)
            n = 16384;
        for (i = 0; i < n; i++) {
            const int16_t v = (int16_t)(a->amp * 32767.0 * sin(a->phase));
            a->phase += 2.0 * M_PI * 440.0 / MR_SAMPLE_RATE;
            buf[i * 2] = buf[i * 2 + 1] = v;
        }
        a->phase = fmod(a->phase, 2.0 * M_PI);
        mr_audio(a->c, buf, n);
    }
}

static mr_client_t *connect_app(app_t *a, const char *name)
{
    mr_callbacks_t cb;
    mr_client_t *c;
    memset(&cb, 0, sizeof cb);
    cb.on_status = cb_status;
    cb.on_bye = cb_bye;
    cb.on_pong = cb_pong;
    cb.on_audio = cb_audio;
    cb.user = a;
    c = mr_connect(host_arg, port_arg, name, &cb, 3000);
    if (!c) {
        fprintf(stderr, "mrctl: no Move Remote (%s)\n%s", mr_last_error(), CONNECT_HINT);
        return NULL;
    }
    a->c = c;
    {
        const mr_hello_t *h = mr_hello(c);
        fprintf(stderr, "connected: %s, proto %u, via %s%s\n", h->name, h->proto,
                h->via == MR_VIA_USB ? "USB" : h->via == MR_VIA_LOCAL ? "local" : "Wi-Fi",
                (h->caps & MR_CAP_SIMULATOR) ? " (simulator)" : "");
    }
    return c;
}

/* ------------------------------------------------------------------ commands --- */
static int cmd_info(void)
{
    app_t a;
    mr_client_t *c;
    const mr_hello_t *h;
    int i;
    memset(&a, 0, sizeof a);
    if (!(c = connect_app(&a, "mrctl info")))
        return 1;
    h = mr_hello(c);
    printf("name        %s\nproto       %u\ncaps        0x%04X\nsample rate %u\nblock       %u\nscreen      %ux%u\n"
           "via         %u\nout queue   %u frames\n", h->name, h->proto, h->caps, h->sample_rate, h->block,
           h->screen_w, h->screen_h, h->via, h->out_capacity);
    for (i = 0; i < 20 && !a.have_st; i++)
        if (mr_poll(c, 50) < 0)
            break;
    if (a.have_st)
        printf("battery     %u\npower       0x%02X\njacks       0x%02X\nflags       0x%02X (rt %s, test %s %u%%)\n"
               "late        %u\n", a.st.battery, a.st.power, a.st.jacks, a.st.flags, (a.st.flags & 1) ? "yes" : "NO",
               (a.st.flags & 2) ? "on" : "off", a.st.test_vol, a.st.late);
    mr_close(c);
    return 0;
}

static void cb_monitor(void *u, const mr_event_t *e)
{
    app_t *a = u;
    print_event(e);
    a->count++;
}

static int cmd_monitor(int max_count, double timeout_s)
{
    app_t a;
    mr_client_t *c;
    mr_callbacks_t cb;
    const uint64_t until = timeout_s > 0 ? mr_now_us() + (uint64_t)(timeout_s * 1e6) : 0;
    memset(&a, 0, sizeof a);
    memset(&cb, 0, sizeof cb);
    cb.on_input = cb_monitor;
    cb.on_bye = cb_bye;
    cb.user = &a;
    if (!(c = mr_connect(host_arg, port_arg, "mrctl monitor", &cb, 3000))) {
        fprintf(stderr, "mrctl: no Move Remote (%s)\n%s", mr_last_error(), CONNECT_HINT);
        return 1;
    }
    mr_config(c, MR_CFG_AUDIO_CLOCK, 0);
    fprintf(stderr, "monitoring (Ctrl-C to stop)\n");
    while (!g_stop && !a.bye && (!max_count || a.count < max_count) && (!until || mr_now_us() < until))
        if (mr_poll(c, 50) < 0)
            break;
    mr_close(c);
    return max_count && a.count < max_count ? 1 : 0;
}

/* ---- the demo */
static void hsv(double h, double s, double v, int *r, int *g, int *b)
{
    const double c = v * s, hh = fmod(h, 1.0) * 6.0, x = c * (1 - fabs(fmod(hh, 2.0) - 1)), m = v - c;
    double rr = 0, gg = 0, bb = 0;
    if (hh < 1) rr = c, gg = x; else if (hh < 2) rr = x, gg = c; else if (hh < 3) gg = c, bb = x;
    else if (hh < 4) gg = x, bb = c; else if (hh < 5) rr = x, bb = c; else rr = c, bb = x;
    *r = (int)((rr + m) * 255), *g = (int)((gg + m) * 255), *b = (int)((bb + m) * 255);
}

static void pad_light(app_t *a, int pad, int level)   /* level 0..127; 0 = the dim resting colour */
{
    int r, g, b;
    hsv((pad % 8) / 8.0 + (pad / 8) * 0.03, 0.85, level ? 0.15 + 0.85 * level / 127.0 : 0.06, &r, &g, &b);
    mr_pad_rgb(a->c, pad, r, g, b);
}

/* the white icon under step i (CC 16..31): full while its step is held, half where the jog wheel's cursor is */
static void icon_light(app_t *a, int i)
{
    const int cursor = ((a->jog % 16) + 16) % 16;
    mr_led(a->c, 1, MR_CC_STEP_ICON0 + i, a->held_step[i] ? 127 : i == cursor ? 48 : 0, 0);
}

static void cb_demo(void *u, const mr_event_t *e)
{
    app_t *a = u;
    const char *bn;
    switch (e->type) {
    case MR_EV_PAD:
        a->pads[e->index] = e->value;
        pad_light(a, e->index, e->value);
        snprintf(a->last, sizeof a->last, "PAD %d vel %d", e->index + 1, e->value);
        break;
    case MR_EV_PAD_PRESSURE:
        if (a->pads[e->index]) {
            pad_light(a, e->index, e->value > 20 ? e->value : 20);
            snprintf(a->last, sizeof a->last, "PAD %d press %d", e->index + 1, e->value);
        }
        break;
    case MR_EV_STEP:
        if (e->value) {
            a->steps[e->index] ^= 1;
            mr_led(a->c, 0, MR_NOTE_STEP0 + e->index, a->steps[e->index] ? 120 : 0, 0);
        }
        a->held_step[e->index] = e->value;
        icon_light(a, e->index);
        snprintf(a->last, sizeof a->last, "STEP %d %s", e->index + 1, e->value ? "down" : "up");
        break;
    case MR_EV_BUTTON:
        bn = mr_button_name(e->index);
        if (e->index == MR_CC_SHIFT)
            a->shift = e->value;
        if (e->index == MR_CC_PLAY || e->index == MR_CC_REC || e->index == MR_CC_SAMPLE ||
            (e->index >= MR_CC_TRACK4 && e->index <= MR_CC_TRACK1))
            mr_led(a->c, 1, e->index, e->value ? (e->index == MR_CC_REC ? 127 : 126) : 0, 0);
        else
            mr_led(a->c, 1, e->index, e->value ? 127 : 16, 0);
        snprintf(a->last, sizeof a->last, "%s %s", bn ? bn : "CC", e->value ? "down" : "up");
        break;
    case MR_EV_KNOB:
        a->knob[e->index] += e->value;
        if (a->knob[e->index] < 0) a->knob[e->index] = 0;
        if (a->knob[e->index] > 127) a->knob[e->index] = 127;
        if (e->index < 8) {
            int r, g, b;
            hsv(e->index / 8.0, 1.0, a->knob[e->index] / 127.0, &r, &g, &b);
            mr_led_rgb(a->c, 1, MR_CC_KNOB1 + e->index, r, g, b);
        }
        snprintf(a->last, sizeof a->last, "%s %+d = %d", e->index == 8 ? "VOLUME" : "KNOB", e->value, a->knob[e->index]);
        break;
    case MR_EV_TOUCH:
        snprintf(a->last, sizeof a->last, "TOUCH %s %s", e->index == 9 ? "jog" : e->index == 8 ? "vol" : "knob",
                 e->value ? "on" : "off");
        break;
    case MR_EV_JOG: {
        const int was = ((a->jog % 16) + 16) % 16;
        a->jog += e->value;
        icon_light(a, was);
        icon_light(a, ((a->jog % 16) + 16) % 16);
        snprintf(a->last, sizeof a->last, "JOG %+d", e->value);
        break;
    }
    case MR_EV_JOG_CLICK:
        snprintf(a->last, sizeof a->last, "JOG CLICK %s", e->value ? "down" : "up");
        break;
    case MR_EV_MIDI:
        snprintf(a->last, sizeof a->last, "USB-A %02X %02X %02X", e->raw[1], e->raw[2], e->raw[3]);
        mr_midi(a->c, e->raw + 1, (e->raw[1] & 0xF0) == 0xC0 || (e->raw[1] & 0xF0) == 0xD0 ? 2 : 3);   /* echo */
        break;
    default:
        return;
    }
    print_event(e);
    a->dirty = 1;
}

static void demo_draw(app_t *a)
{
    mr_canvas_t cv;
    uint8_t packed[MR_SCREEN_BYTES];
    char line[32];
    int i, held = 0;
    mr_gfx_clear(&cv, 0);
    mr_gfx_rect(&cv, 0, 0, 128, 9, 1, 1);
    mr_gfx_text(&cv, 2, 1, "MRCTL DEMO", 0);
    if (a->shift)
        mr_gfx_text(&cv, 92, 1, "SHIFT", 0);
    mr_gfx_text(&cv, 0, 12, a->last, 1);
    for (i = 0; i < 32; i++)
        held += a->pads[i] > 0;
    snprintf(line, sizeof line, "pads %d  jog %d", held, a->jog);
    mr_gfx_text(&cv, 0, 22, line, 1);
    for (i = 0; i < 9; i++) {   /* the knobs (and volume) as bars */
        const int h = a->knob[i] * 28 / 127;
        mr_gfx_rect(&cv, 2 + i * 14, 34, 10, 30, 1, 0);
        mr_gfx_rect(&cv, 3 + i * 14, 63 - h, 8, h, 1, 1);
    }
    mr_gfx_pack(&cv, packed);
    mr_display(a->c, packed);
}

static int cmd_demo(void)
{
    app_t a;
    mr_client_t *c;
    mr_callbacks_t cb;
    uint64_t last = 0;
    int i;
    memset(&a, 0, sizeof a);
    memset(&cb, 0, sizeof cb);
    cb.on_input = cb_demo;
    cb.on_bye = cb_bye;
    cb.on_status = cb_status;
    cb.user = &a;
    if (!(c = mr_connect(host_arg, port_arg, "mrctl demo", &cb, 3000))) {
        fprintf(stderr, "mrctl: no Move Remote (%s)\n%s", mr_last_error(), CONNECT_HINT);
        return 1;
    }
    a.c = c;
    mr_config(c, MR_CFG_AUDIO_CLOCK, 0);
    for (i = 0; i < 32; i++)
        pad_light(&a, i, 0);
    for (i = 0; i < 16; i++)
        icon_light(&a, i);
    snprintf(a.last, sizeof a.last, "press anything");
    a.dirty = 1;
    fprintf(stderr, "demo running: press things on the Move (Ctrl-C to stop)\n");
    while (!g_stop && !a.bye) {
        if (mr_poll(c, 10) < 0)
            break;
        if (a.dirty && mr_now_us() - last > 33000) {
            demo_draw(&a);
            a.dirty = 0;
            last = mr_now_us();
        }
    }
    mr_config(c, MR_CFG_LEDS_OFF, 1);
    mr_close(c);
    return 0;
}

static int cmp_d(const void *x, const void *y)
{
    const double a = *(const double *)x, b = *(const double *)y;
    return a < b ? -1 : a > b;
}

static int cmd_ping(int n)
{
    app_t a;
    mr_client_t *c;
    int i;
    memset(&a, 0, sizeof a);
    if (n < 1) n = 1;
    if (n > 256) n = 256;
    if (!(c = connect_app(&a, "mrctl ping")))
        return 1;
    mr_config(c, MR_CFG_AUDIO_CLOCK, 0);
    for (i = 0; i < n && !g_stop; i++) {
        const int want = a.nrtt + 1;
        const uint64_t until = mr_now_us() + 1000000;
        mr_ping(c, (uint32_t)i);
        while (a.nrtt < want && mr_now_us() < until)
            if (mr_poll(c, 20) < 0)
                break;
        usleep(20000);
    }
    if (!a.nrtt) {
        fprintf(stderr, "no answers\n");
        mr_close(c);
        return 1;
    }
    qsort(a.rtt, (size_t)a.nrtt, sizeof a.rtt[0], cmp_d);
    printf("%d pings: min %.2f ms, median %.2f ms, max %.2f ms\n", a.nrtt, a.rtt[0], a.rtt[a.nrtt / 2], a.rtt[a.nrtt - 1]);
    mr_close(c);
    return 0;
}

static int cmd_tone(double pct, double secs, int target)
{
    app_t a;
    mr_client_t *c;
    uint64_t until;
    memset(&a, 0, sizeof a);
    if (pct < 0) pct = 0;
    if (pct > 25) pct = 25;   /* the speaker is loud: this is a test, not a performance */
    if (!(c = connect_app(&a, "mrctl tone")))
        return 1;
    a.amp = pct / 100.0;
    if (target <= 0)
        target = mr_hello(c)->via == MR_VIA_WIFI ? 4096 : 512;
    mr_pacer_init(&a.pacer, target);
    a.tone_on = 1;
    until = mr_now_us() + (uint64_t)(secs * 1e6);
    while (!g_stop && !a.bye && mr_now_us() < until)
        if (mr_poll(c, 20) < 0)
            break;
    printf("tone: queue target %d frames (%.1f ms): %ld clock ticks, %ld with an underrun; device: underruns %u "
           "overruns %u late %u dropped %u\n", target, target * 1000.0 / MR_SAMPLE_RATE, a.ticks, a.underrun_ticks,
           a.st.underruns, a.st.overruns, a.st.late, a.st.dropped);
    a.tone_on = 0;
    mr_close(c);
    return 0;
}

static int cmd_text(int n, char **lines)
{
    app_t a;
    mr_client_t *c;
    mr_canvas_t cv;
    uint8_t packed[MR_SCREEN_BYTES];
    int i;
    memset(&a, 0, sizeof a);
    if (!(c = connect_app(&a, "mrctl text")))
        return 1;
    mr_gfx_clear(&cv, 0);
    for (i = 0; i < n && i < 8; i++)
        mr_gfx_text(&cv, 0, i * 8, lines[i], 1);
    mr_gfx_pack(&cv, packed);
    mr_display(c, packed);
    mr_poll(c, 100);
    mr_close(c);
    return 0;
}

/* ---- selftest: every message type, against whatever answers (the simulator in make test) */
static int g_fail;
#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        if (cond)                                                                                                     \
            printf("  ok    " __VA_ARGS__), printf("\n");                                                            \
        else                                                                                                          \
            printf("  FAIL  " __VA_ARGS__), printf("\n"), g_fail++;                                                  \
    } while (0)

static void cb_self_input(void *u, const mr_event_t *e)
{
    app_t *a = u;
    if (a->count < 16)
        a->pads[a->count] = (int)e->type << 16 | (e->index & 0xFF) << 8 | (e->value & 0xFF);
    a->count++;
}

static void spin(mr_client_t *c, int ms)
{
    const uint64_t until = mr_now_us() + (uint64_t)ms * 1000u;
    while (mr_now_us() < until)
        if (mr_poll(c, 5) < 0)
            break;
}

static int cmd_selftest(const char *inject)
{
    app_t a;
    mr_client_t *c, *c2;
    mr_callbacks_t cb;
    mr_canvas_t cv;
    uint8_t packed[MR_SCREEN_BYTES];
    int i;
    memset(&a, 0, sizeof a);
    memset(&cb, 0, sizeof cb);
    cb.on_status = cb_status;
    cb.on_bye = cb_bye;
    cb.on_pong = cb_pong;
    cb.on_audio = cb_audio;
    cb.on_input = cb_self_input;
    cb.user = &a;
    printf("selftest\n");
    c = mr_connect(host_arg, port_arg, "selftest", &cb, 3000);
    CHECK(c != NULL, "connect + HELLO");
    if (!c)
        return 1;
    a.c = c;
    CHECK(mr_hello(c)->proto == MR_PROTO && mr_hello(c)->sample_rate == 44100 && mr_hello(c)->block == 128,
          "HELLO fields (proto %u, %u Hz, block %u)", mr_hello(c)->proto, mr_hello(c)->sample_rate, mr_hello(c)->block);

    for (i = 0; i < 10; i++) {
        mr_ping(c, (uint32_t)i);
        spin(c, 10);
    }
    spin(c, 100);
    CHECK(a.nrtt == 10, "PING / PONG (%d of 10)", a.nrtt);

    a.ticks = 0;
    spin(c, 1000);
    CHECK(a.ticks > 140 && a.ticks < 205, "audio clock: %ld ticks in 1 s (172 expected at 2 blocks a tick)", a.ticks);
    CHECK(a.have_st, "STATUS arrives");

    mr_config(c, MR_CFG_AUDIO_IN, 1);
    mr_config(c, MR_CFG_AUDIO_BATCH, 4);
    spin(c, 50);
    a.ticks = 0, a.in_pcm = 0;
    spin(c, 500);
    CHECK(a.in_pcm > 0 && a.in_pcm == a.ticks * 512, "audio in: %ld frames over %ld ticks of 512", a.in_pcm, a.ticks);
    mr_config(c, MR_CFG_AUDIO_IN, 0);
    mr_config(c, MR_CFG_AUDIO_BATCH, 2);

    mr_pacer_init(&a.pacer, mr_hello(c)->via == MR_VIA_WIFI ? 4096 : 1024);
    a.amp = 0.01, a.tone_on = 1;
    spin(c, 1500);
    a.have_st = 0;
    spin(c, 600);
    CHECK(a.have_st && a.st.out_fill > a.pacer.target / 4 && a.st.out_fill < a.pacer.target * 4,
          "audio out holds its queue (fill %u, target %d)", a.st.out_fill, a.pacer.target);
    CHECK(a.st.underruns <= 1 && a.st.overruns == 0, "no dropouts (underruns %u, overruns %u)", a.st.underruns,
          a.st.overruns);
    a.tone_on = 0;

    for (i = 0; i < 32; i++)
        mr_pad_rgb(c, i, i * 8, 255 - i * 8, 64);
    for (i = 0; i < 16; i++)
        mr_led(c, 0, MR_NOTE_STEP0 + i, 120, 0);
    mr_led(c, 1, MR_CC_PLAY, 126, 11);
    mr_gfx_clear(&cv, 0);
    mr_gfx_text(&cv, 0, 0, "selftest", 1);
    mr_gfx_pack(&cv, packed);
    mr_display(c, packed);
    {
        const uint8_t midi[] = { 0x90, 60, 100, 0x80, 60, 0, 0xF0, 0x7D, 0x01, 0x02, 0xF7, 0xF8 };
        mr_midi(c, midi, sizeof midi);
    }
    a.have_st = 0;
    spin(c, 700);
    CHECK(a.have_st && a.st.led_pending == 0, "LEDs all out (pending %u)", a.st.led_pending);

    if (inject) {
        FILE *f = fopen(inject, "w");
        CHECK(f != NULL, "open %s", inject);
        if (f) {
            fprintf(f, "note 68 100\nat 68 50\nnote 68 0\ncc 71 1\ncc 71 127\nnote 9 127\ncc 49 127\n");
            fclose(f);
        }
        spin(c, 300);
        CHECK(a.count == 7, "injected input arrives (%d of 7 events)", a.count);
        CHECK(a.pads[0] == (MR_EV_PAD << 16 | 0 << 8 | 100), "pad 0 velocity 100");
        CHECK(a.pads[1] == (MR_EV_PAD_PRESSURE << 16 | 0 << 8 | 50), "pad 0 pressure 50");
        CHECK(a.pads[2] == (MR_EV_PAD << 16 | 0 << 8 | 0), "pad 0 released");
        CHECK(a.pads[3] == (MR_EV_KNOB << 16 | 0 << 8 | 1), "knob 1 +1");
        CHECK(a.pads[4] == (MR_EV_KNOB << 16 | 0 << 8 | 0xFF), "knob 1 -1");
        CHECK(a.pads[5] == (MR_EV_TOUCH << 16 | 9 << 8 | 1), "jog touched");
        CHECK(a.pads[6] == (MR_EV_BUTTON << 16 | MR_CC_SHIFT << 8 | 1), "Shift down");
    }

    /* a second host takes over; the first is told */
    a.bye = 0;
    {
        app_t b;
        mr_callbacks_t cb2;
        memset(&b, 0, sizeof b);
        memset(&cb2, 0, sizeof cb2);
        cb2.user = &b;
        c2 = mr_connect(host_arg, port_arg, "selftest 2", &cb2, 3000);
        CHECK(c2 != NULL, "a second host connects");
        spin(c, 200);
        CHECK(a.bye == MR_BYE_REPLACED, "the first is told it was replaced (bye %d)", a.bye);
        mr_close(c2);
    }
    mr_close(c);
    printf("%s (%d failed)\n", g_fail ? "FAILED" : "passed", g_fail);
    return g_fail ? 1 : 0;
}

/* mrctl led note|cc N[-M] VALUE [ANIM]  /  mrctl rgb note|cc N[-M] R G B: set LEDs, hold them for a few seconds (the
 * device turns every LED off when its host leaves) */
static int cmd_led(int rgb, int argc, char **argv)
{
    app_t a;
    mr_client_t *c;
    int cc, lo, hi, n;
    uint64_t until;
    memset(&a, 0, sizeof a);
    if (argc < (rgb ? 5 : 3) || (strcmp(argv[0], "note") && strcmp(argv[0], "cc"))) {
        fprintf(stderr, "usage: mrctl led note|cc N[-M] VALUE [ANIM]   |   mrctl rgb note|cc N[-M] R G B\n");
        return 2;
    }
    cc = !strcmp(argv[0], "cc");
    n = sscanf(argv[1], "%d-%d", &lo, &hi);
    if (n < 1)
        return 2;
    if (n < 2)
        hi = lo;
    if (!(c = connect_app(&a, "mrctl led")))
        return 1;
    mr_config(c, MR_CFG_AUDIO_CLOCK, 0);
    for (int k = lo; k <= hi; k++) {
        if (rgb)
            mr_led_rgb(c, cc, k, atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
        else
            mr_led(c, cc, k, atoi(argv[2]), argc > 3 ? atoi(argv[3]) : 0);
    }
    fprintf(stderr, "holding for 5 s (Ctrl-C to stop)\n");
    until = mr_now_us() + 5000000u;
    while (!g_stop && !a.bye && mr_now_us() < until)
        if (mr_poll(c, 50) < 0)
            break;
    mr_close(c);
    return 0;
}

static int cmd_quit(void)
{
    app_t a;
    mr_client_t *c;
    memset(&a, 0, sizeof a);
    if (!(c = connect_app(&a, "mrctl quit")))
        return 1;
    mr_quit(c);
    for (int i = 0; i < 40 && !a.bye; i++)
        if (mr_poll(c, 50) < 0)
            break;
    mr_close(c);
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: mrctl [--host H] [--port N] COMMAND\n"
            "  info | monitor [--count N] [--timeout S] | demo | ping [N] | tone [PERCENT] [SECONDS] [QUEUE]\n"
            "  text LINE... | selftest [--inject FIFO] | quit\n"
            "  led note|cc N[-M] VALUE [ANIM] | rgb note|cc N[-M] R G B   (held 5 s)\n");
}

int main(int argc, char **argv)
{
    int i = 1;
    signal(SIGINT, on_int);
    signal(SIGTERM, on_int);
    signal(SIGPIPE, SIG_IGN);
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "--host") && i + 1 < argc)
            host_arg = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc)
            port_arg = atoi(argv[++i]);
        else {
            usage();
            return 2;
        }
    }
    if (i >= argc) {
        usage();
        return 2;
    }
    if (!strcmp(argv[i], "info"))
        return cmd_info();
    if (!strcmp(argv[i], "monitor")) {
        int count = 0;
        double to = 0;
        for (i++; i + 1 < argc; i += 2) {
            if (!strcmp(argv[i], "--count")) count = atoi(argv[i + 1]);
            else if (!strcmp(argv[i], "--timeout")) to = atof(argv[i + 1]);
        }
        return cmd_monitor(count, to);
    }
    if (!strcmp(argv[i], "demo"))
        return cmd_demo();
    if (!strcmp(argv[i], "ping"))
        return cmd_ping(i + 1 < argc ? atoi(argv[i + 1]) : 20);
    if (!strcmp(argv[i], "tone"))
        return cmd_tone(i + 1 < argc ? atof(argv[i + 1]) : 3.0, i + 2 < argc ? atof(argv[i + 2]) : 2.0,
                        i + 3 < argc ? atoi(argv[i + 3]) : 0);
    if (!strcmp(argv[i], "text"))
        return cmd_text(argc - i - 1, argv + i + 1);
    if (!strcmp(argv[i], "selftest"))
        return cmd_selftest(i + 2 < argc && !strcmp(argv[i + 1], "--inject") ? argv[i + 2] : NULL);
    if (!strcmp(argv[i], "quit"))
        return cmd_quit();
    if (!strcmp(argv[i], "led") || !strcmp(argv[i], "rgb"))
        return cmd_led(!strcmp(argv[i], "rgb"), argc - i - 1, argv + i + 1);
    usage();
    return 2;
}
