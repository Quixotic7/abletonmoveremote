/* SPDX-License-Identifier: MPL-2.0 */
/* The Move's platform: Move Remote as a standalone Schwung tool. Move's own software is stopped while this runs
 * (Schwung's launch-standalone.sh stops it, and starts it again when this exits), so this process owns the SPI link
 * to the surface: the audio in and out, the pads' and buttons' MIDI, the LEDs, the screen and USB-A.
 *
 * The SPI layout and the launch contract are Schwung's (https://github.com/charlesvestal/schwung, MIT:
 * src/lib/schwung_spi_lib.h), and the shape follows movy-host (https://github.com/DimaDake/schwung-movy, MIT) by way
 * of evilgeniusmove's plat_move.c: an exclusive open, one transfer a 128-frame block, the screen answered a slice at
 * a time, a real-time audio thread on core 3 with a setuid helper to make it so. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../include/moveremote/proto.h"
#include "mr_core.h"

/* ---- the SPI page (schwung_spi_lib.h) */
#define SPI_DEVICE "/dev/ablspi0.0"
#define SPI_PAGE 4096
#define SPI_FREQ 20000000
#define SPI_IOC(nr) _IOC(_IOC_NONE, 0, (nr), 0)
#define SPI_IOC_WAIT_SEND_SIZE 10
#define SPI_IOC_SET_SPEED 11
#define OFF_OUT_MIDI 0
#define OFF_OUT_DISP_STAT 80
#define OFF_OUT_DISP_DATA 84
#define OFF_OUT_AUDIO 256
#define OUT_DISP_CHUNK 172
#define OFF_IN_MIDI 2048
#define OFF_IN_DISP_STAT (2048 + 248)
#define OFF_IN_AUDIO (2048 + 256)
#define MIDI_IN_MAX 31

extern char **environ;

static int g_fd = -1;
static uint8_t *g_map;
static volatile sig_atomic_t g_stop;
static volatile int g_audio_stop;
static int g_audio_tid;
static char g_dir[400];
static uint8_t g_serving[MR_SCREEN_BYTES];
static uint8_t *g_live;   /* /dev/shm/schwung-display-live: Schwung's display-server streams it (/mirror) */

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
    va_list ap;
    __builtin_va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    __builtin_va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ------------------------------------------------------------------ SPI --- */
static void spi_pump(void) { ioctl(g_fd, SPI_IOC(SPI_IOC_WAIT_SEND_SIZE), 0x300); }

static int spi_open(void)
{
    g_fd = open(SPI_DEVICE, O_RDWR | O_CLOEXEC);   /* exclusive: EBUSY while Move still holds it */
    if (g_fd < 0)
        return -errno;
    g_map = mmap(NULL, SPI_PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, 0);
    if (g_map == MAP_FAILED) {
        const int e = errno;
        close(g_fd);
        g_fd = -1;
        return -e;
    }
    memset(g_map, 0, SPI_PAGE);
    ioctl(g_fd, SPI_IOC(SPI_IOC_SET_SPEED), SPI_FREQ);
    g_map[0] = 0x0F;   /* a MIDI system reset: ends the surface's power-on LED show */
    g_map[1] = 0xFF;
    spi_pump();
    memset(g_map, 0, 4);
    return 0;
}

static void serve_display(const uint8_t *packed)   /* the panel pulls: it names the slice it wants (1..6) */
{
    uint32_t idx;
    memcpy(&idx, g_map + OFF_IN_DISP_STAT, 4);
    if (idx < 1 || idx > 6)
        return;
    {
        const int off = (int)(idx - 1) * OUT_DISP_CHUNK;
        const int len = idx == 6 ? MR_SCREEN_BYTES - off : OUT_DISP_CHUNK;
        memcpy(g_map + OFF_OUT_DISP_STAT, &idx, 4);
        memcpy(g_map + OFF_OUT_DISP_DATA, packed + off, (size_t)len);
    }
}

static void spi_close(void)
{
    static const uint8_t blank[MR_SCREEN_BYTES];
    int i;
    if (g_fd < 0)
        return;
    memset(g_map + OFF_OUT_AUDIO, 0, MR_BLOCK * 4);
    memset(g_map, 0, MR_OUT_MAX * 4);
    for (i = 0; i < 16; i++) {   /* two rounds of blank slices: Move does not inherit our pixels */
        spi_pump();
        serve_display(blank);
    }
    memset(g_map + OFF_OUT_DISP_STAT, 0, 4);
    munmap(g_map, SPI_PAGE);
    close(g_fd);
    g_fd = -1;
}

void mr_plat_frame(const uint8_t px[1024])
{
    if (g_live)
        memcpy(g_live, px, MR_SCREEN_BYTES);
}

static void live_open(void)
{
    const int fd = shm_open("/schwung-display-live", O_RDWR | O_CREAT, 0666);
    void *p;
    if (fd < 0)
        return;
    fchmod(fd, 0666);   /* whichever uid made it, the other must open it next */
    if (ftruncate(fd, MR_SCREEN_BYTES) != 0) {
        close(fd);
        return;
    }
    p = mmap(NULL, MR_SCREEN_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    g_live = p == MAP_FAILED ? NULL : p;
}

/* ------------------------------------------------------------------ the audio thread --- */
static void *audio_main(void *arg)
{
    uint8_t out[MR_OUT_MAX][4];
    uint8_t in[MIDI_IN_MAX * 8];
    int16_t ain[MR_BLOCK * 2];
    cpu_set_t set;
    struct sched_param sp = { .sched_priority = 70 };
    int nout = 0, late = 0, rc;
    uint64_t last = 0, report = 0;
    (void)arg;
    g_audio_tid = (int)syscall(SYS_gettid);
    pthread_setname_np(pthread_self(), "mr-audio");
    CPU_ZERO(&set);
    CPU_SET(3, &set);   /* the SPI core: Move's own audio thread lives there */
    sched_setaffinity(0, sizeof set, &set);
    rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    logf_("audio: tid %d, SCHED_FIFO 70 %s", g_audio_tid, rc == 0 ? "ok" : "refused (the helper will try)");
    while (!g_audio_stop) {
        uint64_t t;
        memcpy(g_map + OFF_OUT_MIDI, out, (size_t)nout * 4);
        spi_pump();
        memset(g_map + OFF_OUT_MIDI, 0, MR_OUT_MAX * 4);
        t = now_us();
        if (last && t - last > 2902u + 400u) {   /* a block is 2902 us */
            late++;
            mr_rt_late();
        }
        last = t;
        if (t - report > 10000000u) {
            if (late)
                logf_("audio: %d late transfer(s) in the last 10 s", late);
            late = 0, report = t;
        }
        {
            uint32_t idx;
            memcpy(&idx, g_map + OFF_IN_DISP_STAT, 4);
            if (idx == 1)
                mr_rt_display(g_serving);
            serve_display(g_serving);
        }
        memcpy(in, g_map + OFF_IN_MIDI, sizeof in);
        memset(g_map + OFF_IN_MIDI, 0, sizeof in);   /* the mailbox keeps its events until overwritten */
        memcpy(ain, g_map + OFF_IN_AUDIO, sizeof ain);
        mr_rt_block(in, MIDI_IN_MAX, ain, (int16_t *)(void *)(g_map + OFF_OUT_AUDIO), out, &nout);
    }
    return NULL;
}

/* ------------------------------------------------------------------ real time --- */
/* ableton cannot take SCHED_FIFO. bin/heal (heal.c), installed root 04755 by schwung-heal from the stage the launcher
 * leaves, does it for this process's audio thread and nothing else */
static void rt_help(void)
{
    char helper[480], pid[16], tid[16];
    struct stat st;
    pid_t child;
    int status = -1;
    if (sched_getscheduler(g_audio_tid) == SCHED_FIFO) {
        mr_set_realtime(1);
        return;
    }
    snprintf(helper, sizeof helper, "%s/bin/heal", g_dir);
    if (stat(helper, &st) != 0 || st.st_uid != 0 || !(st.st_mode & S_ISUID)) {
        logf_("rt: the audio thread is not real-time and %s is not root 04755: running DEGRADED", helper);
        return;
    }
    snprintf(pid, sizeof pid, "%d", (int)getpid());
    snprintf(tid, sizeof tid, "%d", g_audio_tid);
    {
        char *argv[] = { helper, "rt", pid, tid, NULL };
        if (posix_spawn(&child, helper, NULL, NULL, argv, environ) == 0)
            waitpid(child, &status, 0);
    }
    mr_set_realtime(sched_getscheduler(g_audio_tid) == SCHED_FIFO);
    logf_("rt: helper exit %d, the audio thread is %s", WIFEXITED(status) ? WEXITSTATUS(status) : -1,
          sched_getscheduler(g_audio_tid) == SCHED_FIFO ? "SCHED_FIFO" : "NOT real-time (DEGRADED)");
}

static void on_term(int sig)
{
    (void)sig;
    g_stop = 1;
    mr_request_stop();
}

/* the power off: Move's own power service over D-Bus (movy-host's power.c). The call answers at once and the power
 * goes seconds later, so this process then HOLDS, deaf to TERM: were it to exit, Schwung's launcher would start Move
 * again and the power cut would catch it mid-boot */
static void power_off(void)
{
    char *argv[] = { "/usr/bin/dbus-send", "--system", "--print-reply", "--dest=com.ableton.system",
                     "/com/ableton/System/Power", "com.ableton.system.shutDown", NULL };
    pid_t child;
    int i;
    logf_("power: off, by com.ableton.system");
    signal(SIGTERM, SIG_IGN);
    if (posix_spawn(&child, argv[0], NULL, NULL, argv, environ) != 0) {
        logf_("power: cannot run %s; leaving to Move instead", argv[0]);
        return;
    }
    for (i = 0; i < 120; i++)
        sleep(1);
}

/* an embedder builds this file with -DMR_PLAT_MAIN=<its name> and calls it from its own main */
#ifndef MR_PLAT_MAIN
#define MR_PLAT_MAIN main
#endif
int MR_PLAT_MAIN(int argc, char **argv);
int MR_PLAT_MAIN(int argc, char **argv)
{
    pthread_t audio;
    struct sigaction sa;
    sigset_t ss;
    mr_cfg_t cfg = { "/dev/shm", 0, MR_PORT };
    char *slash;
    int rc, how = MR_RUN;
    (void)argc;
    if (!realpath(argv[0], g_dir))
        snprintf(g_dir, sizeof g_dir, "%s", argv[0]);
    if ((slash = strrchr(g_dir, '/')))
        *slash = 0;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    sigemptyset(&ss);   /* MoveLauncher blocks these in what it starts */
    sigaddset(&ss, SIGTERM);
    sigaddset(&ss, SIGINT);
    sigprocmask(SIG_UNBLOCK, &ss, NULL);

    if ((rc = spi_open()) != 0) {
        logf_("spi: cannot open %s: %s%s", SPI_DEVICE, strerror(-rc), rc == -EBUSY ? " (Move still holds it)" : "");
        return 1;
    }
    logf_("moveremote-host %s: SPI open", MR_VERSION);
    live_open();
    if (mr_init(&cfg) != 0) {
        logf_("moveremote-host: cannot start");
        spi_close();
        return 1;
    }
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        logf_("rt: memory not locked (%s)", strerror(errno));
    if (pthread_create(&audio, NULL, audio_main, NULL) != 0) {
        logf_("audio: no thread");
        mr_shutdown();
        spi_close();
        return 1;
    }
    while (!g_audio_tid)
        usleep(1000);
    rt_help();
    logf_("rt: memory %s", mlockall(MCL_CURRENT | MCL_FUTURE) == 0 ? "locked" : "NOT locked");

    while ((how = mr_step(50)) == MR_RUN) {}

    logf_("moveremote-host: closing (%s)", how == MR_STOP_POWER ? "power off" : how == MR_STOP_EXIT ? "exit" : "stopped");
    g_audio_stop = 1;
    pthread_join(audio, NULL);
    spi_close();
    mr_shutdown();
    if (how == MR_STOP_POWER && !g_stop)
        power_off();
    return 0;
}
