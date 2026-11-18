/* SPDX-License-Identifier: MPL-2.0 */
/* The device side of Move Remote, apart from the hardware: a platform (plat_move.c on the Move, plat_sim.c on a
 * Mac) owns the surface link and calls in from two threads:
 *
 *   the real-time thread   mr_rt_block() once per 128-frame block, mr_rt_display() when the panel asks for slice 1
 *   the main thread        mr_init(), then mr_step() in a loop until it says to stop, then mr_shutdown()
 *
 * Between the two there are only single-producer/single-consumer rings and atomics. */
#ifndef MR_CORE_H
#define MR_CORE_H

#include <stdint.h>

#define MR_VERSION "0.1.1"
#define MR_OUT_MAX 20                         /* USB-MIDI packets the surface takes per transfer */

typedef struct {
    const char *test_dir;  /* where the test hook's files live (/dev/shm on the Move) */
    int simulator;         /* 1: the Mac simulator (MR_CAP_SIMULATOR, no USB-A) */
    int port;              /* MR_PORT unless a test says otherwise */
} mr_cfg_t;

enum { MR_RUN = 0, MR_STOP_EXIT = 1, MR_STOP_POWER = 2, MR_STOP_SIGNAL = 3 };

int mr_init(const mr_cfg_t *cfg);

/* the real-time thread: in[] are the transfer's 8-byte events (zeros already skipped or not, either is fine);
 * ain may be NULL; aout gets MR_BLOCK stereo frames; out[] gets up to MR_OUT_MAX packets for the next transfer */
void mr_rt_block(const uint8_t *in, int nin, const int16_t *ain, int16_t *aout, uint8_t out[][4], int *nout);
/* copies the newest whole frame into serving (call when the panel asks for slice 1) */
void mr_rt_display(uint8_t *serving);
void mr_rt_late(void);                         /* the platform saw a late transfer */

/* the main thread: waits up to max_wait_ms for something to do. Returns MR_RUN or why to stop */
int mr_step(int max_wait_ms);
void mr_request_stop(void);                    /* async-signal-safe: mr_step() returns MR_STOP_SIGNAL */
void mr_set_realtime(int rt);                  /* reported in MR_D_STATUS */
int mr_test_vol(void);                         /* percent of output (100 unless the test hook is on) */
void mr_shutdown(void);

/* the platform's hook: a frame was published (the Move copies it to Schwung's display mirror) */
void mr_plat_frame(const uint8_t px[1024]);

#endif
