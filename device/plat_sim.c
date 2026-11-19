/* SPDX-License-Identifier: MPL-2.0 */
/* The simulator: the device core on a Mac (or any POSIX machine), with no surface. A thread stands in for the SPI
 * link, ticking a block every 2.902 ms; presses come from the test hook (always on here), the input pcm is a quiet
 * 440 Hz tone so a host can see it arrive, and the output goes nowhere but the peak meter.
 *
 *   moveremote-sim [--port N] [--dir DIR]     DIR holds mr-test / mr-cmd / mr-state / mr-screen.pbm
 *                                              (default $TMPDIR/moveremote-sim)
 *
 * USB-A (mr_core.h mr_sim_*, for an embedder's tests): a fake device behind a model of the surface controller's
 * bridge. Nothing is plugged in until mr_sim_usba_plug(1). */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../common/ring.h"
#include "../include/moveremote/proto.h"
#include "mr_core.h"

static volatile int g_audio_stop;

/* ---- USB-A: the device's packets wait in the bridge (at most MR_SIM_BRIDGE_PKTS, the rest lost), each transfer
 * takes up to MR_SIM_BRIDGE_IN; the transfer's cable-2 packets go to the device */
static mr_ring_t g_br_in, g_br_out;    /* device -> Move (the bridge's buffer), Move -> device */
static volatile int g_plugged;
static volatile uint32_t g_led_pkts, g_usba_pkts, g_br_lost;

void mr_sim_usba_plug(int on) { __atomic_store_n(&g_plugged, on ? 1 : 0, __ATOMIC_RELEASE); }

int mr_sim_usba_dev_send(const uint8_t (*pkts)[4], int n)
{
    int i, took = 0;
    if (!g_plugged || !g_br_in.buf)
        return 0;
    for (i = 0; i < n; i++) {
        uint8_t p[4];
        memcpy(p, pkts[i], 4);
        p[0] = (uint8_t)(0x20 | (p[0] & 0x0F));   /* the bridge's cable */
        if (mr_ring_count(&g_br_in) >= MR_SIM_BRIDGE_PKTS || !mr_ring_push(&g_br_in, p, 1))
            __atomic_add_fetch(&g_br_lost, 1, __ATOMIC_RELAXED);
        else
            took++;
    }
    return took;
}

int mr_sim_usba_dev_recv(uint8_t (*pkts)[4], int max)
{
    return g_br_out.buf ? (int)mr_ring_pop(&g_br_out, pkts, (uint32_t)(max < 0 ? 0 : max)) : 0;
}

void mr_sim_counts(uint32_t *led_pkts, uint32_t *usba_out_pkts, uint32_t *bridge_lost)
{
    if (led_pkts)
        *led_pkts = g_led_pkts;
    if (usba_out_pkts)
        *usba_out_pkts = g_usba_pkts;
    if (bridge_lost)
        *bridge_lost = g_br_lost;
}

void mr_plat_frame(const uint8_t px[1024]) { (void)px; }

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void *audio_main(void *arg)
{
    uint8_t out[MR_OUT_MAX][4];
    uint8_t in[MR_SIM_BRIDGE_IN * 8];
    int16_t ain[MR_BLOCK * 2], aout[MR_BLOCK * 2];
    uint8_t serving[MR_SCREEN_BYTES];
    uint64_t next = now_us();
    double ph = 0;
    int nout = 0, blk = 0, i;
    (void)arg;
    while (!g_audio_stop) {
        for (i = 0; i < MR_BLOCK; i++) {
            const int16_t v = (int16_t)(1000.0 * sin(ph));
            ph += 2.0 * M_PI * 440.0 / MR_SAMPLE_RATE;
            ain[i * 2] = ain[i * 2 + 1] = v;
        }
        if (ph > 2.0 * M_PI * 1000)
            ph = fmod(ph, 2.0 * M_PI);
        if (blk++ % 6 == 0)
            mr_rt_display(serving);
        {
            int nin = 0, k;
            const int plugged = __atomic_load_n(&g_plugged, __ATOMIC_ACQUIRE);
            uint8_t p[4];
            memset(in, 0, sizeof in);
            while (nin < MR_SIM_BRIDGE_IN && mr_ring_pop(&g_br_in, p, 1))
                if (plugged)
                    memcpy(in + 8 * nin++, p, 4);
            mr_rt_block(in, nin, ain, aout, out, &nout);
            for (k = 0; k < nout; k++) {
                const int cable = out[k][0] >> 4;
                if (cable == 0)
                    g_led_pkts++;
                else if (cable == 2) {
                    g_usba_pkts++;
                    if (plugged)
                        mr_ring_push(&g_br_out, out[k], 1);
                }
            }
        }
        next += 2902;
        {
            const uint64_t t = now_us();
            if (next > t)
                usleep((useconds_t)(next - t));
            else if (t - next > 400)
                mr_rt_late(), next = t;
        }
    }
    return NULL;
}

static void on_term(int sig)
{
    (void)sig;
    mr_request_stop();
}

/* an embedder builds this file with -DMR_PLAT_MAIN=<its name> and calls it from its own main */
#ifndef MR_PLAT_MAIN
#define MR_PLAT_MAIN main
#endif
int MR_PLAT_MAIN(int argc, char **argv);
int MR_PLAT_MAIN(int argc, char **argv)
{
    char dir[400], p[460];
    const char *tmp = getenv("TMPDIR");
    mr_cfg_t cfg = { NULL, 1, MR_PORT };
    pthread_t audio;
    FILE *f;
    int i, how;
    snprintf(dir, sizeof dir, "%s/moveremote-sim", tmp && *tmp ? tmp : "/tmp");
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc)
            cfg.port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dir") && i + 1 < argc)
            snprintf(dir, sizeof dir, "%s", argv[++i]);
        else {
            fprintf(stderr, "usage: moveremote-sim [--port N] [--dir DIR]\n");
            return 2;
        }
    }
    mkdir(dir, 0777);
    snprintf(p, sizeof p, "%s/mr-test", dir);
    if ((f = fopen(p, "w"))) {   /* the test hook, always: the simulator has no surface of its own */
        fputs("0\n", f);
        fclose(f);
    }
    cfg.test_dir = dir;
    if (mr_ring_init(&g_br_in, 4, 128) || mr_ring_init(&g_br_out, 4, 8192))
        return 1;
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGPIPE, SIG_IGN);
    if (mr_init(&cfg) != 0)
        return 1;
    fprintf(stderr, "moveremote-sim %s: port %d, test files in %s\n", MR_VERSION, cfg.port, dir);
    pthread_create(&audio, NULL, audio_main, NULL);
    while ((how = mr_step(50)) == MR_RUN) {}
    g_audio_stop = 1;
    pthread_join(audio, NULL);
    mr_shutdown();
    fprintf(stderr, "moveremote-sim: stopped (%d)\n", how);
    return 0;
}
